//==- CallConvCascade.cpp - Type cascade for calling-convention rewrites --==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Function-pointer type cascade for the calling-convention lowering pass.
// See CallConvCascade.h and clang/docs/CIR/ABILowering.rst for the design.
//
// This file mirrors the pattern in
// clang/lib/CIR/Dialect/Transforms/CXXABILowering.cpp: an mlir::TypeConverter
// driven by mlir::applyPartialConversion with a generic MatchAnyOpTypeTag
// pattern that clones each operation with converted result types and rewrites
// the contents of attributes that embed types.  Self-referential records
// terminate via a per-thread recursive stack of in-progress records and an
// incomplete-then-complete() placeholder.
//
//===----------------------------------------------------------------------===//

#include "CallConvCascade.h"

#include "mlir/Dialect/OpenACC/OpenACCOpsDialect.h.inc"
#include "mlir/Dialect/OpenMP/OpenMPOpsDialect.h.inc"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Transforms/DialectConversion.h"
#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Dialect/IR/CIRTypes.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace mlir;

namespace {

// -- Attribute legality and rewriting ---------------------------------------
//
// These helpers parallel the equivalents in CXXABILowering.cpp.  Where
// CXXABILowering checks for type-bearing attributes that may embed
// DataMemberType / MethodType, we check for attributes that may embed a
// cir::FuncType whose signature is being rewritten.

bool isCallConvAttributeLegal(const TypeConverter &tc, Attribute attr) {
  if (!attr)
    return true;

  return TypeSwitch<Attribute, bool>(attr)
      // Pure scalars / opaque references with no embedded type.
      .Case<DenseArrayAttr, FloatAttr, UnitAttr, StringAttr, IntegerAttr,
            SymbolRefAttr, cir::AnnotationAttr>([](Attribute) { return true; })
      // Attributes that just carry a single type.
      .Case<cir::ZeroAttr, cir::PoisonAttr, cir::UndefAttr, mlir::TypeAttr,
            cir::ConstPtrAttr, cir::CXXCtorAttr, cir::CXXDtorAttr,
            cir::CXXAssignAttr>([&tc](Attribute a) {
        Type ty;
        TypeSwitch<Attribute, void>(a)
            .Case<cir::ZeroAttr>([&ty](cir::ZeroAttr za) { ty = za.getType(); })
            .Case<cir::PoisonAttr>(
                [&ty](cir::PoisonAttr pa) { ty = pa.getType(); })
            .Case<cir::UndefAttr>(
                [&ty](cir::UndefAttr uda) { ty = uda.getType(); })
            .Case<mlir::TypeAttr>(
                [&ty](mlir::TypeAttr ta) { ty = ta.getValue(); })
            .Case<cir::ConstPtrAttr>(
                [&ty](cir::ConstPtrAttr cpa) { ty = cpa.getType(); })
            .Case<cir::CXXCtorAttr>(
                [&ty](cir::CXXCtorAttr ca) { ty = ca.getType(); })
            .Case<cir::CXXDtorAttr>(
                [&ty](cir::CXXDtorAttr da) { ty = da.getType(); })
            .Case<cir::CXXAssignAttr>(
                [&ty](cir::CXXAssignAttr aa) { ty = aa.getType(); });
        return tc.isLegal(ty);
      })
      // Collection attributes: legal iff every element is legal.
      .Case<mlir::ArrayAttr>([&tc](mlir::ArrayAttr array) {
        return llvm::all_of(array.getValue(), [&tc](Attribute a) {
          return isCallConvAttributeLegal(tc, a);
        });
      })
      .Case<mlir::DictionaryAttr>([&tc](mlir::DictionaryAttr dict) {
        return llvm::all_of(dict.getValue(), [&tc](mlir::NamedAttribute na) {
          return isCallConvAttributeLegal(tc, na.getValue());
        });
      })
      .Case<cir::ConstArrayAttr>([&tc](cir::ConstArrayAttr array) {
        return tc.isLegal(array.getType()) &&
               isCallConvAttributeLegal(tc, array.getElts());
      })
      .Case<cir::GlobalViewAttr>([&tc](cir::GlobalViewAttr gva) {
        return tc.isLegal(gva.getType()) &&
               isCallConvAttributeLegal(tc, gva.getIndices());
      })
      .Case<cir::VTableAttr>([&tc](cir::VTableAttr vta) {
        return tc.isLegal(vta.getType()) &&
               isCallConvAttributeLegal(tc, vta.getData());
      })
      .Case<cir::TypeInfoAttr>([&tc](cir::TypeInfoAttr tia) {
        return tc.isLegal(tia.getType()) &&
               isCallConvAttributeLegal(tc, tia.getData());
      })
      .Case<cir::DynamicCastInfoAttr>([&tc](cir::DynamicCastInfoAttr dcia) {
        return isCallConvAttributeLegal(tc, dcia.getSrcRtti()) &&
               isCallConvAttributeLegal(tc, dcia.getDestRtti());
      })
      .Case<cir::ConstRecordAttr>([&tc](cir::ConstRecordAttr cra) {
        return tc.isLegal(cra.getType()) &&
               isCallConvAttributeLegal(tc, cra.getMembers());
      })
      // Anything else -- if any new attribute embeds a type that could
      // contain a function pointer, it must be added to both this switch
      // and the rewriteCallConvAttribute switch below.
      .Default(true);
}

mlir::Attribute rewriteCallConvAttribute(const TypeConverter &tc,
                                         MLIRContext *ctx, Attribute attr);

// Some attributes carry an optional ArrayAttr child (e.g.
// GlobalViewAttr::indices).  When the optional child is null,
// rewriteCallConvAttribute also returns null, and `mlir::cast<ArrayAttr>` on
// the null would assert.  This helper makes the null case explicit.
static mlir::ArrayAttr rewriteOptionalArrayAttr(const TypeConverter &tc,
                                                MLIRContext *ctx,
                                                Attribute inner) {
  if (!inner)
    return nullptr;
  mlir::Attribute rewritten = rewriteCallConvAttribute(tc, ctx, inner);
  if (!rewritten)
    return nullptr;
  return mlir::cast<mlir::ArrayAttr>(rewritten);
}

mlir::Attribute rewriteCallConvAttribute(const TypeConverter &tc,
                                         MLIRContext *ctx, Attribute attr) {
  if (isCallConvAttributeLegal(tc, attr))
    return attr;

  return TypeSwitch<Attribute, mlir::Attribute>(attr)
      .Case<cir::ZeroAttr>([&tc](cir::ZeroAttr za) {
        return cir::ZeroAttr::get(tc.convertType(za.getType()));
      })
      .Case<cir::PoisonAttr>([&tc](cir::PoisonAttr pa) {
        return cir::PoisonAttr::get(tc.convertType(pa.getType()));
      })
      .Case<cir::UndefAttr>([&tc](cir::UndefAttr uda) {
        return cir::UndefAttr::get(tc.convertType(uda.getType()));
      })
      .Case<mlir::TypeAttr>([&tc](mlir::TypeAttr ta) {
        return mlir::TypeAttr::get(tc.convertType(ta.getValue()));
      })
      .Case<cir::ConstPtrAttr>([&tc](cir::ConstPtrAttr cpa) {
        return cir::ConstPtrAttr::get(tc.convertType(cpa.getType()),
                                      cpa.getValue());
      })
      .Case<cir::CXXCtorAttr>([&tc](cir::CXXCtorAttr ca) {
        return cir::CXXCtorAttr::get(tc.convertType(ca.getType()),
                                     ca.getCtorKind(), ca.getIsTrivial());
      })
      .Case<cir::CXXDtorAttr>([&tc](cir::CXXDtorAttr da) {
        return cir::CXXDtorAttr::get(tc.convertType(da.getType()),
                                     da.getIsTrivial());
      })
      .Case<cir::CXXAssignAttr>([&tc](cir::CXXAssignAttr aa) {
        return cir::CXXAssignAttr::get(tc.convertType(aa.getType()),
                                       aa.getAssignKind(), aa.getIsTrivial());
      })
      .Case<mlir::ArrayAttr>([&tc, ctx](mlir::ArrayAttr array) {
        SmallVector<Attribute> elts;
        for (Attribute a : array.getValue())
          elts.push_back(rewriteCallConvAttribute(tc, ctx, a));
        return mlir::ArrayAttr::get(ctx, elts);
      })
      .Case<mlir::DictionaryAttr>([&tc, ctx](mlir::DictionaryAttr dict) {
        SmallVector<mlir::NamedAttribute> elts;
        for (mlir::NamedAttribute na : dict.getValue())
          elts.emplace_back(na.getName(),
                            rewriteCallConvAttribute(tc, ctx, na.getValue()));
        return mlir::DictionaryAttr::get(ctx, elts);
      })
      .Case<cir::ConstArrayAttr>([&tc, ctx](cir::ConstArrayAttr array) {
        return cir::ConstArrayAttr::get(
            ctx, tc.convertType(array.getType()),
            rewriteCallConvAttribute(tc, ctx, array.getElts()),
            array.getTrailingZerosNum());
      })
      .Case<cir::GlobalViewAttr>([&tc, ctx](cir::GlobalViewAttr gva) {
        return cir::GlobalViewAttr::get(
            tc.convertType(gva.getType()), gva.getSymbol(),
            rewriteOptionalArrayAttr(tc, ctx, gva.getIndices()));
      })
      .Case<cir::VTableAttr>([&tc, ctx](cir::VTableAttr vta) {
        return cir::VTableAttr::get(
            tc.convertType(vta.getType()),
            rewriteOptionalArrayAttr(tc, ctx, vta.getData()));
      })
      .Case<cir::TypeInfoAttr>([&tc, ctx](cir::TypeInfoAttr tia) {
        return cir::TypeInfoAttr::get(
            tc.convertType(tia.getType()),
            rewriteOptionalArrayAttr(tc, ctx, tia.getData()));
      })
      .Case<cir::DynamicCastInfoAttr>([&tc,
                                       ctx](cir::DynamicCastInfoAttr dcia) {
        return cir::DynamicCastInfoAttr::get(
            mlir::cast<cir::GlobalViewAttr>(
                rewriteCallConvAttribute(tc, ctx, dcia.getSrcRtti())),
            mlir::cast<cir::GlobalViewAttr>(
                rewriteCallConvAttribute(tc, ctx, dcia.getDestRtti())),
            dcia.getRuntimeFunc(), dcia.getBadCastFunc(), dcia.getOffsetHint());
      })
      .Case<cir::ConstRecordAttr>([&tc, ctx](cir::ConstRecordAttr cra) {
        return cir::ConstRecordAttr::get(
            ctx, tc.convertType(cra.getType()),
            rewriteOptionalArrayAttr(tc, ctx, cra.getMembers()));
      })
      .DefaultUnreachable("unrewritten illegal call-conv attribute kind");
}

// -- Type converter ---------------------------------------------------------
//
// Carries the FuncType -> FuncType rewrite map and a per-thread record stack
// for cycle detection.  Mirror of CXXABILowering's CIRABITypeConverter.

class CirCallConvTypeConverter : public TypeConverter {
  const cir::callconv::FuncTypeMap &rewrites;

  // Per-thread stack of cir::RecordType currently being converted.  Used to
  // break cycles in self-referential records.
  llvm::DenseMap<uint64_t, std::unique_ptr<SmallVector<cir::RecordType>>>
      conversionCallStack;
  llvm::sys::SmartRWMutex<true> callStackMutex;

  llvm::SmallVector<cir::RecordType> convertedRecordTypes;
  llvm::sys::SmartRWMutex<true> recordTypeMutex;

  SmallVector<cir::RecordType> &getCurrentThreadRecursiveStack() {
    {
      std::shared_lock<decltype(callStackMutex)> lock(callStackMutex,
                                                      std::defer_lock);
      if (mlirContext->isMultithreadingEnabled())
        lock.lock();
      auto it = conversionCallStack.find(llvm::get_threadid());
      if (it != conversionCallStack.end())
        return *it->second;
    }
    std::unique_lock<decltype(callStackMutex)> lock(callStackMutex);
    auto inserted = conversionCallStack.insert(
        std::make_pair(llvm::get_threadid(),
                       std::make_unique<SmallVector<cir::RecordType>>()));
    return *inserted.first->second;
  }

  void addConvertedRecordType(cir::RecordType rt) {
    std::unique_lock<decltype(recordTypeMutex)> lock(recordTypeMutex);
    convertedRecordTypes.push_back(rt);
  }

  // Convert the member type list of `type` recursively.
  SmallVector<Type> convertRecordMemberTypes(cir::RecordType type) {
    SmallVector<Type> loweredMembers;
    loweredMembers.reserve(type.getNumElements());
    if (failed(convertTypes(type.getMembers(), loweredMembers)))
      return {};
    return loweredMembers;
  }

  // Returns true iff `converted` differs element-wise from `original`.
  static bool membersChanged(ArrayRef<Type> original,
                             ArrayRef<Type> converted) {
    if (original.size() != converted.size())
      return true;
    for (auto [a, b] : llvm::zip(original, converted))
      if (a != b)
        return true;
    return false;
  }

  cir::RecordType convertRecordType(cir::RecordType type) {
    if (!type.getName()) {
      SmallVector<Type> members = convertRecordMemberTypes(type);
      if (!membersChanged(type.getMembers(), members))
        return type;
      return cir::RecordType::get(type.getContext(), members, type.getPacked(),
                                  type.getPadded(), type.getKind());
    }

    assert(!type.isIncomplete() || type.getMembers().empty());
    if (type.isIncomplete() || type.isABIConvertedRecord())
      return type;

    auto &recursiveStack = getCurrentThreadRecursiveStack();

    // If we're already converting this record on this thread, return the
    // in-progress placeholder.  This is the only path that needs the
    // __post_abi_ name — it breaks recursion in self-referential records.
    if (llvm::is_contained(recursiveStack, type)) {
      auto convertedType = cir::RecordType::get(
          type.getContext(), type.getABIConvertedName(), type.getKind());
      return convertedType;
    }

    recursiveStack.push_back(type);
    llvm::scope_exit popOnExit(
        [&recursiveStack]() { recursiveStack.pop_back(); });

    SmallVector<Type> convertedMembers = convertRecordMemberTypes(type);

    // If nothing in our member list actually changed, return the original
    // record unchanged.  Wholesale renaming every named record creates a
    // sea of unrealized_conversion_casts that the cascade can't reliably
    // eliminate when a value of that type flows through ops the cascade
    // doesn't rewrite (e.g., builtin.unrealized_conversion_cast itself,
    // or ops from dialects that aren't registered for partial conversion).
    if (!membersChanged(type.getMembers(), convertedMembers))
      return type;

    // Otherwise, mint the __post_abi_X placeholder and complete it with the
    // new member list.  restoreRecordTypeNames() will strip the prefix once
    // the partial conversion finishes.
    auto convertedType = cir::RecordType::get(
        type.getContext(), type.getABIConvertedName(), type.getKind());

    if (convertedType.isComplete())
      return convertedType;

    convertedType.complete(convertedMembers, type.getPacked(),
                           type.getPadded());
    addConvertedRecordType(convertedType);
    return convertedType;
  }

  MLIRContext *mlirContext;

public:
  CirCallConvTypeConverter(MLIRContext &ctx,
                           const cir::callconv::FuncTypeMap &rewrites)
      : rewrites(rewrites), mlirContext(&ctx) {
    // Identity fallback for any type the cascade does not touch.
    addConversion([](Type ty) -> Type { return ty; });

    // FuncType: look up in the rewrite map.  When the rewrite map has a
    // hit, the immediate FuncType return is the ABI-rewritten signature --
    // BUT its inputs / return type may still reference SOURCE record types
    // that have not yet been renamed when this rewrite happens.  As the
    // cascade walks more ops, those records may get renamed
    // (`__post_abi_*`) by other rewrites; the rewritten FuncType from the
    // rewrite map then references stale records, and a follow-up
    // `convertType` (e.g. from `isLegal`) returns a structurally different
    // type, which makes the legalizer mark our just-rewritten op illegal
    // again.  To stay consistent, recurse into the rewritten FuncType's
    // inputs / return so any stateful record renames get applied
    // transitively.  When no rewrite-map hit, recurse the same way so a
    // function pointer type that only embeds rewritten types in its
    // inputs / return also gets rebuilt.
    //
    // Termination invariant: any cycle through this lambda must include
    // a `RecordType` step, because MLIR's structural type uniquing forbids
    // constructing a `cir::FuncType` that directly references itself.
    // `convertRecordType`'s per-thread `recursiveStack` plus the
    // `__post_abi_*` placeholder break record self-reference, so any
    // FuncType -> Record -> FuncType chain terminates (correctness for
    // self-referential cycles is a separate concern -- see
    // `callconv-cascade-fnptr-self-ref.cpp` which is currently XFAIL'd
    // for that reason).  Do NOT introduce a Phase 1 rewrite whose
    // rewrite-map value directly mentions its own key in its input or
    // return list with no intervening RecordType: that would let this
    // lambda re-enter without progress until stack overflow.
    addConversion([this](cir::FuncType ty) -> Type {
      cir::FuncType effective = ty;
      auto it = this->rewrites.find(ty);
      if (it != this->rewrites.end())
        effective = it->second;

      SmallVector<Type> loweredInputs;
      loweredInputs.reserve(effective.getNumInputs());
      if (failed(convertTypes(effective.getInputs(), loweredInputs)))
        return {};
      Type loweredReturn = convertType(effective.getReturnType());
      if (!loweredReturn)
        return {};
      return cir::FuncType::get(loweredInputs, loweredReturn,
                                /*isVarArg=*/effective.getVarArg());
    });

    addConversion([this](cir::PointerType ty) -> Type {
      Type loweredPointee = convertType(ty.getPointee());
      if (!loweredPointee)
        return {};
      return cir::PointerType::get(ty.getContext(), loweredPointee,
                                   ty.getAddrSpace());
    });

    addConversion([this](cir::ArrayType ty) -> Type {
      Type loweredElement = convertType(ty.getElementType());
      if (!loweredElement)
        return {};
      return cir::ArrayType::get(loweredElement, ty.getSize());
    });

    addConversion(
        [this](cir::RecordType ty) -> Type { return convertRecordType(ty); });
  }

  // After applyPartialConversion succeeds, restore the original record type
  // names (the converter renames them with an ABI-converted prefix during
  // recursion to break cycles; users see only the final names).
  void restoreRecordTypeNames() {
    std::unique_lock<decltype(recordTypeMutex)> lock(recordTypeMutex);
    for (auto rt : convertedRecordTypes)
      rt.removeABIConversionNamePrefix();
  }
};

// -- Generic conversion pattern --------------------------------------------
//
// Mirror of CIRGenericCXXABILoweringPattern: clones each illegal CIR op with
// converted result types and rewritten attributes.

class CirCallConvCascadePattern : public mlir::ConversionPattern {
public:
  CirCallConvCascadePattern(MLIRContext *ctx,
                            const TypeConverter &typeConverter)
      : mlir::ConversionPattern(typeConverter, MatchAnyOpTypeTag(),
                                /*benefit=*/1, ctx) {}

  LogicalResult
  matchAndRewrite(Operation *op, llvm::ArrayRef<Value> operands,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    const TypeConverter *typeConverter = getTypeConverter();
    assert(typeConverter && "CirCallConvCascadePattern requires a converter");

    bool operandsAndResultsLegal = typeConverter->isLegal(op);
    bool regionsLegal =
        std::all_of(op->getRegions().begin(), op->getRegions().end(),
                    [typeConverter](mlir::Region &r) {
                      return typeConverter->isLegal(&r);
                    });
    bool attrsLegal =
        llvm::all_of(op->getAttrs(), [typeConverter](mlir::NamedAttribute na) {
          return isCallConvAttributeLegal(*typeConverter, na.getValue());
        });

    if (operandsAndResultsLegal && regionsLegal && attrsLegal)
      return failure();

    OperationState newState(op->getLoc(), op->getName());
    newState.addOperands(operands);
    newState.addSuccessors(op->getSuccessors());

    SmallVector<mlir::NamedAttribute> attrs;
    for (const mlir::NamedAttribute &na : op->getAttrs())
      attrs.push_back({na.getName(), rewriteCallConvAttribute(*typeConverter,
                                                              op->getContext(),
                                                              na.getValue())});
    newState.addAttributes(attrs);

    SmallVector<Type> loweredResultTypes;
    loweredResultTypes.reserve(op->getNumResults());
    for (Type result : op->getResultTypes())
      loweredResultTypes.push_back(typeConverter->convertType(result));
    newState.addTypes(loweredResultTypes);

    for (mlir::Region &region : op->getRegions()) {
      mlir::Region *loweredRegion = newState.addRegion();
      rewriter.inlineRegionBefore(region, *loweredRegion, loweredRegion->end());
      if (failed(rewriter.convertRegionTypes(loweredRegion, *typeConverter)))
        return failure();
    }

    Operation *loweredOp = rewriter.create(newState);
    rewriter.replaceOp(op, loweredOp);
    return success();
  }
};

// -- Conversion target ------------------------------------------------------
//
// Marks any CIR operation whose operand/result/region/attribute types embed a
// rewritten cir::FuncType as illegal, forcing the generic pattern to clone it.

void populateCallConvCascadeTarget(mlir::ConversionTarget &target,
                                   const TypeConverter &typeConverter) {
  target.addLegalOp<mlir::ModuleOp>();

  auto attrsLegal = [&typeConverter](mlir::Operation *op) {
    return llvm::all_of(
        op->getAttrs(), [&typeConverter](const mlir::NamedAttribute &a) {
          return isCallConvAttributeLegal(typeConverter, a.getValue());
        });
  };

  auto dialectLegality = [&typeConverter, attrsLegal](Operation *op) {
    if (!typeConverter.isLegal(op))
      return false;
    return attrsLegal(op) &&
           std::all_of(op->getRegions().begin(), op->getRegions().end(),
                       [&typeConverter](mlir::Region &region) {
                         return typeConverter.isLegal(&region);
                       });
  };

  target.addDynamicallyLegalDialect<cir::CIRDialect>(dialectLegality);

  // OpenACC and OpenMP ops can hold cir.ptr/cir.record operand types whose
  // record content gets renamed by the cascade.  Without registering them
  // here the partial conversion would leave dangling
  // unrealized_conversion_casts when a CIR producer of a record-typed value
  // is rewritten.  Mirror of CXXABILowering's handling.
  target.addDynamicallyLegalDialect<mlir::acc::OpenACCDialect>(dialectLegality);
  target.addDynamicallyLegalDialect<mlir::omp::OpenMPDialect>(dialectLegality);

  // cir::FuncOp's signature lives on the op itself, not in its operands.
  // Coroutine functions are skipped by CallConvLowering's classification
  // phase (they keep their Task-returning signature even when another
  // non-coroutine function shares the same source FuncType and has its
  // return rewritten to void).  The cascade must respect that exemption
  // -- a coroutine's FuncOp must not be touched by the cascade even if
  // its FuncType matches a rewrite entry produced by some non-coroutine
  // peer.  This mirrors the per-function ABI policy on the FuncOp.
  target.addDynamicallyLegalOp<cir::FuncOp>(
      [&typeConverter, attrsLegal](cir::FuncOp op) {
        if (op->hasAttr("coroutine"))
          return true;
        bool attrs = attrsLegal(op);
        return attrs && typeConverter.isLegal(op.getFunctionType());
      });
  target.addDynamicallyLegalOp<cir::GlobalOp>(
      [&typeConverter](cir::GlobalOp op) {
        return typeConverter.isLegal(op.getSymType());
      });
}

// -- collectUnreachable -----------------------------------------------------
//
// applyPartialConversion walks blocks in dominance order and skips
// unreachable blocks.  Reachable verification still runs on those blocks
// after conversion, so they need to be added to the worklist explicitly.
// Mirror of CXXABILowering's helper of the same name.

void collectUnreachable(Operation *parent, SmallVector<Operation *> &ops) {
  SmallVector<mlir::Block *> unreachableBlocks;
  parent->walk([&](mlir::Block *blk) {
    if (blk->hasNoPredecessors() && !blk->isEntryBlock())
      unreachableBlocks.push_back(blk);
  });

  std::set<mlir::Block *> visited;
  for (mlir::Block *root : unreachableBlocks) {
    SmallVector<mlir::Block *> workList;
    workList.push_back(root);
    while (!workList.empty()) {
      mlir::Block *blk = workList.back();
      workList.pop_back();
      if (visited.count(blk))
        continue;
      visited.insert(blk);
      for (Operation &op : *blk)
        ops.push_back(&op);
      for (mlir::Block *succ : blk->getSuccessors())
        workList.push_back(succ);
    }
  }
}

} // namespace

namespace cir {
namespace callconv {

mlir::LogicalResult applyCallConvCascade(mlir::ModuleOp module,
                                         const FuncTypeMap &rewrites) {
  // Phase A short-circuit: when no functions have been reclassified, the
  // cascade has nothing to propagate.  This is the common case until
  // uniform rewriting lands in Phase B.
  if (rewrites.empty())
    return mlir::success();

  MLIRContext *ctx = module.getContext();
  CirCallConvTypeConverter typeConverter(*ctx, rewrites);

  RewritePatternSet patterns(ctx);
  patterns.add<CirCallConvCascadePattern>(ctx, typeConverter);

  ConversionTarget target(*ctx);
  populateCallConvCascadeTarget(target, typeConverter);

  SmallVector<Operation *> ops;
  ops.push_back(module);
  collectUnreachable(module, ops);

  if (failed(applyPartialConversion(ops, target, std::move(patterns))))
    return mlir::failure();

  typeConverter.restoreRecordTypeNames();
  return mlir::success();
}

} // namespace callconv
} // namespace cir
