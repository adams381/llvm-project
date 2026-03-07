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
      mlir::isa<cir::RecordType>(srcTy) || mlir::isa<cir::RecordType>(dstTy);

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
    // (struct -> integer).
    if (argClass.Kind != ArgKind::Extend && argClass.Kind != ArgKind::Direct)
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
      newArgTypes.push_back(argClass.CoercedType ? argClass.CoercedType
                                                 : origTy);
      if (argClass.CoercedType && argClass.CoercedType != origTy)
        hasArgChanges = true;
      break;
    case ArgKind::Indirect:
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

  // If nothing changed, skip the rewrite.
  if (!hasSRet && !hasArgChanges &&
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
  }

  Type newFnTy = funcOp.cloneTypeWith(newArgTypes, newResultTypes);
  funcOp.setFunctionTypeAttr(TypeAttr::get(newFnTy));

  // Attach llvm.sret attribute to the sret pointer argument so the
  // CIR-to-LLVM lowering can propagate it to the LLVM IR signature.
  if (hasSRet) {
    MLIRContext *ctx = funcOp->getContext();
    Type retTy = oldResultTypes[0];
    unsigned numArgs = newArgTypes.size();

    SmallVector<Attribute> argAttrDicts(numArgs, DictionaryAttr::get(ctx));
    SmallVector<NamedAttribute> sretAttrs;
    sretAttrs.push_back(
        rewriter.getNamedAttr("llvm.sret", TypeAttr::get(retTy)));
    sretAttrs.push_back(rewriter.getNamedAttr(
        "llvm.align",
        rewriter.getI64IntegerAttr(fc.ReturnInfo.IndirectAlign.value())));
    argAttrDicts[0] = DictionaryAttr::get(ctx, sretAttrs);
    funcOp->setAttr("arg_attrs", ArrayAttr::get(ctx, argAttrDicts));
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
  // Preserve call attributes (noreturn, side_effect, etc.).
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
