//===- CIRABIRewriteContext.cpp - CIR-specific ABI rewriting --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CIRABIRewriteContext.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Types.h"
#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Dialect/IR/CIROpsEnums.h"
#include <limits>

using namespace cir;
using namespace mlir;
using namespace mlir::abi;

/// Emit a value coercion between two types.  For scalar-to-scalar (e.g.
/// integer sign extension), a direct cir.cast is sufficient.  When one of
/// the types is a record (struct), LLVM IR's bitcast cannot reinterpret
/// between aggregate and scalar types, so we go through memory:
///   alloca srcTy → store src → pointer bitcast → load dstTy.
static Value emitCoercion(OpBuilder &rewriter, Location loc, Type dstTy,
                          Value src) {
  Type srcTy = src.getType();
  if (srcTy == dstTy)
    return src;

  bool needsMemory =
      mlir::isa<cir::RecordType, cir::ComplexType>(srcTy) ||
      mlir::isa<cir::RecordType, cir::ComplexType>(dstTy) ||
      (mlir::isa<cir::VectorType>(srcTy) != mlir::isa<cir::VectorType>(dstTy));

  if (!needsMemory) {
    return cir::CastOp::create(rewriter, loc, dstTy, cir::CastKind::bitcast,
                               src);
  }

  auto srcPtrTy = cir::PointerType::get(srcTy);
  auto dstPtrTy = cir::PointerType::get(dstTy);

  auto alloca =
      cir::AllocaOp::create(rewriter, loc, srcPtrTy, srcTy,
                            /*name=*/rewriter.getStringAttr("coerce"),
                            /*alignment=*/rewriter.getI64IntegerAttr(8));

  cir::StoreOp::create(rewriter, loc, src, alloca,
                       /*isVolatile=*/mlir::UnitAttr(),
                       /*alignment=*/mlir::IntegerAttr(),
                       /*sync_scope=*/cir::SyncScopeKindAttr(),
                       /*mem_order=*/cir::MemOrderAttr());

  auto ptrCast = cir::CastOp::create(rewriter, loc, dstPtrTy,
                                     cir::CastKind::bitcast, alloca);

  return cir::LoadOp::create(rewriter, loc, dstTy, ptrCast,
                             /*isDeref=*/mlir::UnitAttr(),
                             /*isVolatile=*/mlir::UnitAttr(),
                             /*alignment=*/mlir::IntegerAttr(),
                             /*sync_scope=*/cir::SyncScopeKindAttr(),
                             /*mem_order=*/cir::MemOrderAttr());
}

/// Insert coercion before each cir.return to coerce the return value from
/// the original type to the ABI type.
static void insertReturnCoercion(FunctionOpInterface funcOp, Type origRetTy,
                                 Type coercedRetTy, OpBuilder &rewriter) {
  SmallVector<cir::ReturnOp> returnOps;
  funcOp->walk([&](cir::ReturnOp retOp) { returnOps.push_back(retOp); });

  for (cir::ReturnOp retOp : returnOps) {
    if (retOp.getInput().empty())
      continue;

    Value origVal = retOp.getInput()[0];
    if (origVal.getType() == coercedRetTy)
      continue;

    rewriter.setInsertionPoint(retOp);
    Value coerced =
        emitCoercion(rewriter, retOp.getLoc(), coercedRetTy, origVal);
    retOp->setOperand(0, coerced);
  }
}

/// Rewrite each cir.return to store the return value through the sret
/// pointer (first block argument) and return void.
static void insertSRetStores(FunctionOpInterface funcOp, Type origRetTy,
                             OpBuilder &rewriter) {
  Value sretPtr = funcOp.getArguments()[0];

  SmallVector<cir::ReturnOp> returnOps;
  funcOp->walk([&](cir::ReturnOp retOp) { returnOps.push_back(retOp); });

  for (cir::ReturnOp retOp : returnOps) {
    if (retOp.getInput().empty())
      continue;

    Value retVal = retOp.getInput()[0];
    rewriter.setInsertionPoint(retOp);
    cir::StoreOp::create(rewriter, retOp.getLoc(), retVal, sretPtr,
                         /*isVolatile=*/mlir::UnitAttr(),
                         /*alignment=*/mlir::IntegerAttr(),
                         /*sync_scope=*/cir::SyncScopeKindAttr(),
                         /*mem_order=*/cir::MemOrderAttr());
    cir::ReturnOp::create(rewriter, retOp.getLoc());
    retOp->erase();
  }
}

/// For each argument that requires ABI coercion (Extend or Direct with a
/// coerced type), insert a cast at the function entry and replace all uses
/// of the block argument with the cast result.
static void insertArgAdaptation(FunctionOpInterface funcOp,
                                const FunctionClassification &fc,
                                unsigned sretOffset, OpBuilder &rewriter) {
  Region &body = funcOp->getRegion(0);
  if (body.empty())
    return;

  Block &entryBlock = body.front();

  // Track the insertion point so casts are emitted in argument order.
  Operation *lastInserted = nullptr;

  for (auto [idx, argClass] : llvm::enumerate(fc.ArgInfos)) {
    if (!argClass.CoercedType)
      continue;

    // Handle Extend (integer widening) and Direct with coercion
    // (struct -> integer).  Skip flattened args — they are handled
    // separately after this function.
    if (argClass.Kind != ArgKind::Extend && argClass.Kind != ArgKind::Direct)
      continue;
    if (argClass.CanFlatten && argClass.CoercedType)
      if (auto cr = dyn_cast<cir::RecordType>(argClass.CoercedType))
        if (cr.getMembers().size() > 1)
          continue;

    unsigned blockIdx = idx + sretOffset;
    BlockArgument blockArg = entryBlock.getArgument(blockIdx);

    Type oldArgTy = blockArg.getType();
    Type newArgTy = argClass.CoercedType;

    if (oldArgTy == newArgTy)
      continue;

    blockArg.setType(newArgTy);

    if (lastInserted)
      rewriter.setInsertionPointAfter(lastInserted);
    else
      rewriter.setInsertionPointToStart(&entryBlock);

    Value adapted;
    SmallPtrSet<Operation *, 4> coercionOps;

    if (argClass.Kind == ArgKind::Extend) {
      auto cast = cir::CastOp::create(rewriter, funcOp.getLoc(), oldArgTy,
                                      cir::CastKind::integral, blockArg);
      adapted = cast;
      coercionOps.insert(cast.getOperation());
    } else {
      // Memory-based reinterpretation: alloca + store + pointer
      // bitcast + load.  All four ops reference blockArg directly or
      // indirectly and must be excluded from replaceAllUsesExcept.
      auto srcPtrTy = cir::PointerType::get(newArgTy);
      auto dstPtrTy = cir::PointerType::get(oldArgTy);
      Location loc = funcOp.getLoc();

      auto alloca =
          cir::AllocaOp::create(rewriter, loc, srcPtrTy, newArgTy,
                                /*name=*/rewriter.getStringAttr("coerce"),
                                /*alignment=*/rewriter.getI64IntegerAttr(8));

      auto store = cir::StoreOp::create(rewriter, loc, blockArg, alloca,
                                        /*isVolatile=*/mlir::UnitAttr(),
                                        /*alignment=*/mlir::IntegerAttr(),
                                        /*sync_scope=*/cir::SyncScopeKindAttr(),
                                        /*mem_order=*/cir::MemOrderAttr());

      auto ptrCast = cir::CastOp::create(rewriter, loc, dstPtrTy,
                                         cir::CastKind::bitcast, alloca);

      auto load = cir::LoadOp::create(rewriter, loc, oldArgTy, ptrCast,
                                      /*isDeref=*/mlir::UnitAttr(),
                                      /*isVolatile=*/mlir::UnitAttr(),
                                      /*alignment=*/mlir::IntegerAttr(),
                                      /*sync_scope=*/cir::SyncScopeKindAttr(),
                                      /*mem_order=*/cir::MemOrderAttr());

      adapted = load;
      coercionOps.insert(alloca.getOperation());
      coercionOps.insert(store.getOperation());
      coercionOps.insert(ptrCast.getOperation());
      coercionOps.insert(load.getOperation());
    }
    lastInserted = adapted.getDefiningOp();

    blockArg.replaceAllUsesExcept(adapted, coercionOps);
  }
}

LogicalResult CIRABIRewriteContext::rewriteFunctionDefinition(
    FunctionOpInterface funcOp, const FunctionClassification &fc,
    OpBuilder &rewriter) {
  ArrayRef<Type> oldArgTypes = funcOp.getArgumentTypes();
  ArrayRef<Type> oldResultTypes = funcOp.getResultTypes();
  bool isDecl = funcOp.isDeclaration();

  bool hasSRet =
      fc.ReturnInfo.Kind == ArgKind::Indirect && !oldResultTypes.empty();
  bool returnCoerced = false;
  bool hasArgChanges = false;
  SmallVector<unsigned> ignoredArgIndices;

  // Compute new argument types.
  SmallVector<Type> newArgTypes;

  if (hasSRet) {
    auto ptrTy = cir::PointerType::get(oldResultTypes[0]);
    newArgTypes.push_back(ptrTy);
  }

  for (auto [idx, argClass] : llvm::enumerate(fc.ArgInfos)) {
    Type origTy = oldArgTypes[idx];
    switch (argClass.Kind) {
    case ArgKind::Direct:
    case ArgKind::Extend:
      if (argClass.CanFlatten && argClass.CoercedType) {
        if (auto coercedRec = dyn_cast<cir::RecordType>(argClass.CoercedType)) {
          if (coercedRec.getMembers().size() > 1) {
            for (mlir::Type fieldTy : coercedRec.getMembers())
              newArgTypes.push_back(fieldTy);
            hasArgChanges = true;
            break;
          }
        }
      }
      newArgTypes.push_back(argClass.CoercedType ? argClass.CoercedType
                                                 : origTy);
      if (argClass.CoercedType && argClass.CoercedType != origTy)
        hasArgChanges = true;
      break;
    case ArgKind::Indirect:
      newArgTypes.push_back(cir::PointerType::get(origTy));
      hasArgChanges = true;
      break;
    case ArgKind::Expand:
      newArgTypes.push_back(origTy);
      break;
    case ArgKind::Ignore:
      ignoredArgIndices.push_back(idx);
      hasArgChanges = true;
      break;
    }
  }

  // Compute new result types.  CIR's FuncType always has exactly one
  // result (VoidType for void functions), so newResultTypes must never
  // be empty.
  SmallVector<Type> newResultTypes;
  if (hasSRet) {
    newResultTypes.push_back(cir::VoidType::get(funcOp->getContext()));
  } else if (fc.ReturnInfo.Kind == ArgKind::Direct ||
             fc.ReturnInfo.Kind == ArgKind::Extend) {
    if (fc.ReturnInfo.CoercedType) {
      newResultTypes.push_back(fc.ReturnInfo.CoercedType);
      returnCoerced = !oldResultTypes.empty() &&
                      fc.ReturnInfo.CoercedType != oldResultTypes[0];
    } else {
      newResultTypes = SmallVector<Type>(oldResultTypes);
    }
  } else if (fc.ReturnInfo.Kind == ArgKind::Ignore) {
    newResultTypes.push_back(cir::VoidType::get(funcOp->getContext()));
  } else {
    newResultTypes = SmallVector<Type>(oldResultTypes);
  }

  // If nothing changed, skip the rewrite — unless we have
  // Extend args/returns that need signext/zeroext attrs.
  bool hasExtend = fc.ReturnInfo.Kind == ArgKind::Extend;
  for (auto &argClass : fc.ArgInfos)
    if (argClass.Kind == ArgKind::Extend)
      hasExtend = true;
  if (!hasSRet && !hasArgChanges && !hasExtend &&
      newArgTypes == SmallVector<Type>(oldArgTypes) &&
      newResultTypes == SmallVector<Type>(oldResultTypes))
    return success();

  // Body modifications only apply to definitions.
  //
  // Arg adaptation must run before return coercion because setType()
  // on block arguments retroactively changes the SSA type.  If return
  // coercion ran first and created memory-based coercion ops that
  // reference a block argument, a subsequent setType() would leave
  // those ops with a stale type until replaceAllUsesExcept fixes them
  // — but the intermediate state is invalid IR.  By adapting arguments
  // first (which redirects all pre-existing uses), return coercion
  // sees the already-adapted values.
  if (!isDecl) {
    if (hasSRet) {
      auto ptrTy = cir::PointerType::get(oldResultTypes[0]);
      Region &body = funcOp->getRegion(0);
      body.insertArgument(0u, ptrTy, funcOp.getLoc());
      insertSRetStores(funcOp, oldResultTypes[0], rewriter);
    }

    unsigned sretOffset = hasSRet ? 1 : 0;
    if (hasArgChanges)
      insertArgAdaptation(funcOp, fc, sretOffset, rewriter);

    // For Indirect args, the block argument type was changed to a
    // pointer.  Insert a load at function entry so existing uses see
    // the original value type.
    if (!funcOp->getRegion(0).empty()) {
      Block &entry = funcOp->getRegion(0).front();
      for (auto [idx, argClass] : llvm::enumerate(fc.ArgInfos)) {
        if (argClass.Kind != ArgKind::Indirect)
          continue;
        unsigned blockIdx = idx + sretOffset;
        BlockArgument blockArg = entry.getArgument(blockIdx);
        Type origTy = oldArgTypes[idx];
        auto ptrTy = cir::PointerType::get(origTy);
        blockArg.setType(ptrTy);

        rewriter.setInsertionPointToStart(&entry);
        auto load =
            cir::LoadOp::create(rewriter, funcOp.getLoc(), origTy, blockArg,
                                /*isDeref=*/mlir::UnitAttr(),
                                /*isVolatile=*/mlir::UnitAttr(),
                                /*alignment=*/mlir::IntegerAttr(),
                                /*sync_scope=*/cir::SyncScopeKindAttr(),
                                /*mem_order=*/cir::MemOrderAttr());
        SmallPtrSet<Operation *, 1> loadOps;
        loadOps.insert(load.getOperation());
        blockArg.replaceAllUsesExcept(load, loadOps);
      }
    }

    // For flattened Direct args (CanFlatten + RecordType coercion),
    // replace the single struct block arg with N scalar block args
    // and reconstruct the struct at function entry.
    if (!funcOp->getRegion(0).empty()) {
      Block &entry = funcOp->getRegion(0).front();
      // Process flattened args in reverse to keep indices stable.
      // Use the original block index (i + sretOffset) since the block
      // still has the original arguments at this point.  Reverse
      // iteration ensures that insertions at higher indices do not
      // shift the positions of earlier (lower-indexed) arguments.
      for (int i = fc.ArgInfos.size() - 1; i >= 0; --i) {
        auto &argClass = fc.ArgInfos[i];
        if (argClass.Kind != ArgKind::Direct || !argClass.CanFlatten ||
            !argClass.CoercedType)
          continue;
        auto coercedRec = dyn_cast<cir::RecordType>(argClass.CoercedType);
        if (!coercedRec || coercedRec.getMembers().size() <= 1)
          continue;

        unsigned origBlockIdx = i + sretOffset;
        Type origTy = oldArgTypes[i];
        auto members = coercedRec.getMembers();
        unsigned numFields = members.size();

        // The block arg at origBlockIdx currently has the original
        // struct type.  Replace it with N scalar args, then
        // reconstruct the original struct via the coerced type.
        BlockArgument origArg = entry.getArgument(origBlockIdx);

        // Insert N new scalar block args after the original.
        for (unsigned f = 0; f < numFields; ++f)
          entry.insertArgument(origBlockIdx + 1 + f, members[f],
                               funcOp.getLoc());

        // Store scalars into a coerced-type alloca, then bitcast
        // to the original struct type and load.  This handles the
        // case where the coerced fields differ from the original
        // struct's fields (e.g. dim3 {i32,i32,i32} -> {i64,i32}).
        rewriter.setInsertionPointToStart(&entry);
        Type coercedTy = argClass.CoercedType;
        auto coercedPtrTy = cir::PointerType::get(coercedTy);
        auto alloca = cir::AllocaOp::create(
            rewriter, funcOp.getLoc(), coercedPtrTy, coercedTy,
            /*name=*/rewriter.getStringAttr("coerce"),
            /*alignment=*/rewriter.getI64IntegerAttr(8));

        for (unsigned f = 0; f < numFields; ++f) {
          BlockArgument scalarArg = entry.getArgument(origBlockIdx + 1 + f);
          auto memberPtr = cir::GetMemberOp::create(
              rewriter, funcOp.getLoc(), cir::PointerType::get(members[f]),
              alloca,
              /*name=*/"", f);
          cir::StoreOp::create(rewriter, funcOp.getLoc(), scalarArg, memberPtr,
                               /*isVolatile=*/mlir::UnitAttr(),
                               /*alignment=*/mlir::IntegerAttr(),
                               /*sync_scope=*/cir::SyncScopeKindAttr(),
                               /*mem_order=*/cir::MemOrderAttr());
        }

        auto origPtrTy = cir::PointerType::get(origTy);
        auto ptrCast = cir::CastOp::create(rewriter, funcOp.getLoc(), origPtrTy,
                                           cir::CastKind::bitcast, alloca);
        auto loaded =
            cir::LoadOp::create(rewriter, funcOp.getLoc(), origTy, ptrCast,
                                /*isDeref=*/mlir::UnitAttr(),
                                /*isVolatile=*/mlir::UnitAttr(),
                                /*alignment=*/mlir::IntegerAttr(),
                                /*sync_scope=*/cir::SyncScopeKindAttr(),
                                /*mem_order=*/cir::MemOrderAttr());

        origArg.replaceAllUsesWith(loaded);
        entry.eraseArgument(origBlockIdx);
      }
    }

    // Erase block arguments for Ignore'd args (in reverse to keep
    // indices valid).  Replace any remaining uses with undef first.
    if (!ignoredArgIndices.empty()) {
      Region &body = funcOp->getRegion(0);
      if (!body.empty()) {
        Block &entry = body.front();
        for (int i = ignoredArgIndices.size() - 1; i >= 0; --i) {
          unsigned blockIdx = ignoredArgIndices[i] + sretOffset;
          if (blockIdx < entry.getNumArguments()) {
            BlockArgument arg = entry.getArgument(blockIdx);
            if (!arg.use_empty()) {
              rewriter.setInsertionPointToStart(&entry);
              auto ptrTy = cir::PointerType::get(arg.getType());
              auto alloca = cir::AllocaOp::create(
                  rewriter, funcOp.getLoc(), ptrTy, arg.getType(),
                  /*name=*/rewriter.getStringAttr("ignored"),
                  /*alignment=*/rewriter.getI64IntegerAttr(1));
              auto load = cir::LoadOp::create(
                  rewriter, funcOp.getLoc(), arg.getType(), alloca,
                  /*isDeref=*/mlir::UnitAttr(),
                  /*isVolatile=*/mlir::UnitAttr(),
                  /*alignment=*/mlir::IntegerAttr(),
                  /*sync_scope=*/cir::SyncScopeKindAttr(),
                  /*mem_order=*/cir::MemOrderAttr());
              arg.replaceAllUsesWith(load);
            }
            entry.eraseArgument(blockIdx);
          }
        }
      }
    }

    if (returnCoerced)
      insertReturnCoercion(funcOp, oldResultTypes[0], fc.ReturnInfo.CoercedType,
                           rewriter);

    // When the return type is Ignore (empty struct), rewrite all
    // return ops to drop their operand so they return void.
    if (fc.ReturnInfo.Kind == ArgKind::Ignore && !oldResultTypes.empty()) {
      funcOp.walk([&](cir::ReturnOp retOp) {
        if (retOp.getNumOperands() > 0) {
          rewriter.setInsertionPoint(retOp);
          auto voidRet = cir::ReturnOp::create(rewriter, retOp.getLoc());
          retOp->erase();
        }
      });
    }
  }

  Type newFnTy = funcOp.cloneTypeWith(newArgTypes, newResultTypes);
  funcOp.setFunctionTypeAttr(TypeAttr::get(newFnTy));

  // Attach llvm.sret / llvm.byval attributes to the appropriate
  // pointer arguments so the CIR-to-LLVM lowering can propagate
  // them to the LLVM IR signature.
  {
    MLIRContext *ctx = funcOp->getContext();
    unsigned numArgs = newArgTypes.size();
    bool needsAttrs = hasSRet;
    bool hasIgnoredArgs = false;
    for (auto &argClass : fc.ArgInfos) {
      if (argClass.Kind == ArgKind::Indirect)
        needsAttrs = true;
      if (argClass.Kind == ArgKind::Ignore)
        hasIgnoredArgs = true;
      if (argClass.Kind == ArgKind::Extend)
        needsAttrs = true;
    }
    // Also rebuild arg_attrs when args were dropped (Ignore) or
    // expanded (flattened) and existing arg_attrs would have the
    // wrong count.
    if ((hasIgnoredArgs || hasArgChanges) && funcOp->hasAttr("arg_attrs"))
      needsAttrs = true;

    if (needsAttrs) {
      SmallVector<Attribute> argAttrDicts(numArgs, DictionaryAttr::get(ctx));

      // Preserve existing arg_attrs from CodeGen (e.g. this pointer
      // nonnull/dereferenceable/align/dead_on_return), shifting by
      // sretOff and skipping Ignore'd args.
      unsigned sretOff = hasSRet ? 1 : 0;
      if (auto existingAttrs = funcOp->getAttrOfType<ArrayAttr>("arg_attrs")) {
        unsigned newIdx = sretOff;
        for (unsigned oldIdx = 0; oldIdx < existingAttrs.size(); ++oldIdx) {
          if (oldIdx < fc.ArgInfos.size() &&
              fc.ArgInfos[oldIdx].Kind == ArgKind::Ignore)
            continue;
          if (newIdx < numArgs)
            argAttrDicts[newIdx] = existingAttrs[oldIdx];
          // Account for flattened args (one original → N new).
          if (oldIdx < fc.ArgInfos.size()) {
            auto &ac = fc.ArgInfos[oldIdx];
            if (ac.CanFlatten && ac.CoercedType)
              if (auto coercedRec = dyn_cast<cir::RecordType>(ac.CoercedType))
                if (coercedRec.getMembers().size() > 1) {
                  newIdx += coercedRec.getMembers().size();
                  continue;
                }
          }
          ++newIdx;
        }
      }

      if (hasSRet) {
        SmallVector<NamedAttribute> sretAttrs;
        sretAttrs.push_back(rewriter.getNamedAttr(
            "llvm.sret", TypeAttr::get(oldResultTypes[0])));
        sretAttrs.push_back(rewriter.getNamedAttr(
            "llvm.align",
            rewriter.getI64IntegerAttr(fc.ReturnInfo.IndirectAlign.value())));
        sretAttrs.push_back(
            rewriter.getNamedAttr("llvm.noalias", rewriter.getUnitAttr()));
        sretAttrs.push_back(
            rewriter.getNamedAttr("llvm.writable", rewriter.getUnitAttr()));
        sretAttrs.push_back(rewriter.getNamedAttr("llvm.dead_on_unwind",
                                                  rewriter.getUnitAttr()));
        argAttrDicts[0] = DictionaryAttr::get(ctx, sretAttrs);
      }

      for (auto [idx, argClass] : llvm::enumerate(fc.ArgInfos)) {
        if (argClass.Kind != ArgKind::Indirect)
          continue;
        if (argClass.ByVal) {
          SmallVector<NamedAttribute> byvalAttrs;
          byvalAttrs.push_back(rewriter.getNamedAttr(
              "llvm.byval", TypeAttr::get(oldArgTypes[idx])));
          byvalAttrs.push_back(rewriter.getNamedAttr(
              "llvm.align",
              rewriter.getI64IntegerAttr(argClass.IndirectAlign.value())));
          byvalAttrs.push_back(
              rewriter.getNamedAttr("llvm.noundef", rewriter.getUnitAttr()));
          if (passByValueIsNoAlias) {
            auto recTy = dyn_cast<cir::RecordType>(oldArgTypes[idx]);
            if (recTy && recTy.isTriviallyCopyable())
              byvalAttrs.push_back(rewriter.getNamedAttr(
                  "llvm.noalias", rewriter.getUnitAttr()));
          }
          argAttrDicts[idx + sretOff] = DictionaryAttr::get(ctx, byvalAttrs);
        } else {
          SmallVector<NamedAttribute> indirectAttrs;
          indirectAttrs.push_back(
              rewriter.getNamedAttr("llvm.noundef", rewriter.getUnitAttr()));
          auto recTy = dyn_cast<cir::RecordType>(oldArgTypes[idx]);
          if (!recTy || recTy.isTriviallyDestructible())
            indirectAttrs.push_back(rewriter.getNamedAttr(
                "llvm.dead_on_return",
                rewriter.getI64IntegerAttr(
                    std::numeric_limits<uint64_t>::max())));
          argAttrDicts[idx + sretOff] = DictionaryAttr::get(ctx, indirectAttrs);
        }
      }

      // Add signext/zeroext for Extend args.
      for (auto [idx, argClass] : llvm::enumerate(fc.ArgInfos)) {
        if (argClass.Kind != ArgKind::Extend)
          continue;
        unsigned newIdx = idx + sretOff;
        if (newIdx >= numArgs)
          continue;
        auto existing = mlir::cast<DictionaryAttr>(argAttrDicts[newIdx]);
        SmallVector<NamedAttribute> attrs(existing.begin(), existing.end());
        StringRef attrName =
            argClass.SignExtend ? "llvm.signext" : "llvm.zeroext";
        attrs.push_back(
            rewriter.getNamedAttr(attrName, rewriter.getUnitAttr()));
        argAttrDicts[newIdx] = DictionaryAttr::get(ctx, attrs);
      }

      funcOp->setAttr("arg_attrs", ArrayAttr::get(ctx, argAttrDicts));
    }

    // Add signext/zeroext to return value for Extend returns.
    // This is outside the needsAttrs block because a function
    // can have an Extend return with no special arg attrs.
    if (fc.ReturnInfo.Kind == ArgKind::Extend) {
      SmallVector<NamedAttribute> retAttrs;
      if (auto existing = funcOp->getAttrOfType<ArrayAttr>("res_attrs"))
        if (existing.size() > 0)
          for (auto attr : mlir::cast<DictionaryAttr>(existing[0]))
            retAttrs.push_back(attr);
      StringRef attrName =
          fc.ReturnInfo.SignExtend ? "llvm.signext" : "llvm.zeroext";
      retAttrs.push_back(
          rewriter.getNamedAttr(attrName, rewriter.getUnitAttr()));
      SmallVector<Attribute> resAttrDicts;
      resAttrDicts.push_back(DictionaryAttr::get(ctx, retAttrs));
      funcOp->setAttr("res_attrs", ArrayAttr::get(ctx, resAttrDicts));
    }
  }

  return success();
}

LogicalResult CIRABIRewriteContext::rewriteCallSite(
    Operation *callOp, const FunctionClassification &fc, OpBuilder &rewriter) {
  auto call = cast<cir::CallOp>(callOp);
  bool hasSRet = fc.ReturnInfo.Kind == ArgKind::Indirect;

  // Coerce arguments at the call site (e.g., extend i8 to i32, or
  // bitcast a struct to its coerced integer type).
  SmallVector<Value> newArgs;
  bool argsChanged = false;
  auto argOperands = call.getArgOperands();

  for (auto [idx, argClass] : llvm::enumerate(fc.ArgInfos)) {
    if (idx >= argOperands.size())
      break;

    Value arg = argOperands[idx];

    if (argClass.Kind == ArgKind::Ignore) {
      argsChanged = true;
      continue;
    }

    if (argClass.Kind == ArgKind::Indirect) {
      rewriter.setInsertionPoint(call);
      Type origTy = arg.getType();
      auto ptrTy = cir::PointerType::get(origTy);
      auto alloca = cir::AllocaOp::create(
          rewriter, call.getLoc(), ptrTy, origTy,
          /*name=*/
          rewriter.getStringAttr(argClass.ByVal ? "byval" : "indirect"),
          /*alignment=*/
          rewriter.getI64IntegerAttr(argClass.IndirectAlign.value()));
      cir::StoreOp::create(rewriter, call.getLoc(), arg, alloca,
                           /*isVolatile=*/mlir::UnitAttr(),
                           /*alignment=*/mlir::IntegerAttr(),
                           /*sync_scope=*/cir::SyncScopeKindAttr(),
                           /*mem_order=*/cir::MemOrderAttr());
      newArgs.push_back(alloca);
      argsChanged = true;
      continue;
    }

    // Flatten multi-register struct args into individual scalars.
    // Store the original struct, bitcast to the coerced type, then
    // load each field via GetMemberOp on the coerced RecordType.
    if (argClass.Kind == ArgKind::Direct && argClass.CanFlatten &&
        argClass.CoercedType) {
      if (auto coercedRec = dyn_cast<cir::RecordType>(argClass.CoercedType)) {
        if (coercedRec.getMembers().size() > 1) {
          rewriter.setInsertionPoint(call);
          Type origTy = arg.getType();
          auto origPtrTy = cir::PointerType::get(origTy);
          auto alloca = cir::AllocaOp::create(
              rewriter, call.getLoc(), origPtrTy, origTy,
              /*name=*/rewriter.getStringAttr("coerce"),
              /*alignment=*/rewriter.getI64IntegerAttr(8));
          cir::StoreOp::create(rewriter, call.getLoc(), arg, alloca,
                               /*isVolatile=*/mlir::UnitAttr(),
                               /*alignment=*/mlir::IntegerAttr(),
                               /*sync_scope=*/cir::SyncScopeKindAttr(),
                               /*mem_order=*/cir::MemOrderAttr());
          auto coercedPtrTy = cir::PointerType::get(argClass.CoercedType);
          auto ptrCast =
              cir::CastOp::create(rewriter, call.getLoc(), coercedPtrTy,
                                  cir::CastKind::bitcast, alloca);
          for (auto [f, fieldTy] : llvm::enumerate(coercedRec.getMembers())) {
            auto memberPtr = cir::GetMemberOp::create(
                rewriter, call.getLoc(), cir::PointerType::get(fieldTy),
                ptrCast,
                /*name=*/"", f);
            auto fieldVal =
                cir::LoadOp::create(rewriter, call.getLoc(), fieldTy, memberPtr,
                                    /*isDeref=*/mlir::UnitAttr(),
                                    /*isVolatile=*/mlir::UnitAttr(),
                                    /*alignment=*/mlir::IntegerAttr(),
                                    /*sync_scope=*/cir::SyncScopeKindAttr(),
                                    /*mem_order=*/cir::MemOrderAttr());
            newArgs.push_back(fieldVal);
          }
          argsChanged = true;
          continue;
        }
      }
    }

    if ((argClass.Kind == ArgKind::Extend ||
         argClass.Kind == ArgKind::Direct) &&
        argClass.CoercedType && arg.getType() != argClass.CoercedType) {
      rewriter.setInsertionPoint(call);
      Value coerced;
      if (argClass.Kind == ArgKind::Extend)
        coerced =
            cir::CastOp::create(rewriter, call.getLoc(), argClass.CoercedType,
                                cir::CastKind::integral, arg);
      else
        coerced =
            emitCoercion(rewriter, call.getLoc(), argClass.CoercedType, arg);
      newArgs.push_back(coerced);
      argsChanged = true;
    } else {
      newArgs.push_back(arg);
    }
  }

  // Pass through variadic operands beyond the classified args.
  // Struct variadic args must be coerced to integer types because
  // the callee reads them via va_arg as raw register/memory values.
  for (unsigned i = fc.ArgInfos.size(); i < argOperands.size(); ++i) {
    Value arg = argOperands[i];
    if (isa<cir::RecordType>(arg.getType())) {
      rewriter.setInsertionPoint(call);
      auto dl = mlir::DataLayout::closest(call.getOperation());
      uint64_t sizeBits = dl.getTypeSizeInBits(arg.getType());
      auto coercedTy =
          cir::IntType::get(call.getContext(), sizeBits, /*isSigned=*/false);
      arg = emitCoercion(rewriter, call.getLoc(), coercedTy, arg);
      argsChanged = true;
    }
    newArgs.push_back(arg);
  }

  // Handle indirect return (sret) at call site.
  if (hasSRet && call.getNumResults() > 0) {
    Type origRetTy = call.getResult().getType();
    auto ptrTy = cir::PointerType::get(origRetTy);

    rewriter.setInsertionPoint(call);

    auto alloca =
        cir::AllocaOp::create(rewriter, call.getLoc(), ptrTy, origRetTy,
                              /*name=*/rewriter.getStringAttr("sret"),
                              /*alignment=*/rewriter.getI64IntegerAttr(8));

    SmallVector<Value> sretArgs;
    sretArgs.push_back(alloca);
    sretArgs.append(newArgs.begin(), newArgs.end());

    auto voidTy = cir::VoidType::get(call.getContext());
    auto newCall = cir::CallOp::create(rewriter, call.getLoc(),
                                       call.getCalleeAttr(), voidTy, sretArgs);
    // Preserve call attributes (noreturn, side_effect, etc.).
    for (NamedAttribute attr : call->getAttrs())
      if (!newCall->hasAttr(attr.getName()))
        newCall->setAttr(attr.getName(), attr.getValue());

    rewriter.setInsertionPointAfter(newCall);
    auto load = cir::LoadOp::create(rewriter, call.getLoc(), origRetTy, alloca,
                                    /*isDeref=*/mlir::UnitAttr(),
                                    /*isVolatile=*/mlir::UnitAttr(),
                                    /*alignment=*/mlir::IntegerAttr(),
                                    /*sync_scope=*/cir::SyncScopeKindAttr(),
                                    /*mem_order=*/cir::MemOrderAttr());

    call.getResult().replaceAllUsesWith(load);
    call->erase();
    return success();
  }

  // Handle Ignore return (e.g. empty struct): replace the call
  // with a void call and substitute an undef for any uses.
  if (fc.ReturnInfo.Kind == ArgKind::Ignore && call.getNumResults() > 0) {
    rewriter.setInsertionPoint(call);
    auto voidTy = cir::VoidType::get(call.getContext());
    auto newCall = cir::CallOp::create(rewriter, call.getLoc(),
                                       call.getCalleeAttr(), voidTy, newArgs);
    for (NamedAttribute attr : call->getAttrs())
      if (!newCall->hasAttr(attr.getName()))
        newCall->setAttr(attr.getName(), attr.getValue());

    if (!call.getResult().use_empty()) {
      rewriter.setInsertionPointAfter(newCall);
      Type origRetTy = call.getResult().getType();
      auto ptrTy = cir::PointerType::get(origRetTy);
      auto alloca =
          cir::AllocaOp::create(rewriter, call.getLoc(), ptrTy, origRetTy,
                                /*name=*/rewriter.getStringAttr("ignored"),
                                /*alignment=*/rewriter.getI64IntegerAttr(1));
      auto load =
          cir::LoadOp::create(rewriter, call.getLoc(), origRetTy, alloca,
                              /*isDeref=*/mlir::UnitAttr(),
                              /*isVolatile=*/mlir::UnitAttr(),
                              /*alignment=*/mlir::IntegerAttr(),
                              /*sync_scope=*/cir::SyncScopeKindAttr(),
                              /*mem_order=*/cir::MemOrderAttr());
      call.getResult().replaceAllUsesWith(load);
    }
    call->erase();
    return success();
  }

  // Handle direct return coercion.
  bool returnCoerced = false;
  Type coercedRetTy;
  if ((fc.ReturnInfo.Kind == ArgKind::Direct ||
       fc.ReturnInfo.Kind == ArgKind::Extend) &&
      fc.ReturnInfo.CoercedType) {
    returnCoerced = true;
    coercedRetTy = fc.ReturnInfo.CoercedType;
  }

  if (!returnCoerced && !argsChanged)
    return success();

  Type callRetTy;
  Type origRetTy;
  bool hasResult = call.getNumResults() > 0;

  if (hasResult) {
    origRetTy = call.getResult().getType();
    callRetTy = returnCoerced ? coercedRetTy : origRetTy;
  } else {
    callRetTy = cir::VoidType::get(call.getContext());
  }

  rewriter.setInsertionPoint(call);
  auto newCall = cir::CallOp::create(rewriter, call.getLoc(),
                                     call.getCalleeAttr(), callRetTy, newArgs);
  for (NamedAttribute attr : call->getAttrs())
    if (!newCall->hasAttr(attr.getName()))
      newCall->setAttr(attr.getName(), attr.getValue());

  if (hasResult && returnCoerced && origRetTy != coercedRetTy) {
    rewriter.setInsertionPointAfter(newCall);
    Value castBack =
        emitCoercion(rewriter, call.getLoc(), origRetTy, newCall.getResult());
    call.getResult().replaceAllUsesWith(castBack);
  } else if (hasResult) {
    call.getResult().replaceAllUsesWith(newCall.getResult());
  }

  call->erase();
  return success();
}
