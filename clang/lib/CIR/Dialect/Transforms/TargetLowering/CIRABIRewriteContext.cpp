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

/// Insert a cir.cast bitcast before each cir.return to coerce the return
/// value from the original type to the ABI type.
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
    auto castOp = cir::CastOp::create(rewriter, retOp.getLoc(), coercedRetTy,
                                      cir::CastKind::bitcast, origVal);
    retOp->setOperand(0, castOp);
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

    // Extend uses integral cast (truncation back to narrow type).
    // Direct struct coercion uses bitcast (coerced type back to struct).
    cir::CastKind castKind = argClass.Kind == ArgKind::Extend
                                 ? cir::CastKind::integral
                                 : cir::CastKind::bitcast;

    auto adaptCast = cir::CastOp::create(rewriter, funcOp.getLoc(), oldArgTy,
                                         castKind, blockArg);
    lastInserted = adaptCast.getOperation();

    blockArg.replaceAllUsesExcept(adaptCast, adaptCast.getOperation());
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
      break;
    }
  }

  // Compute new result types.
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
  }

  // If nothing changed, skip the rewrite.
  if (!hasSRet && !hasArgChanges &&
      newArgTypes == SmallVector<Type>(oldArgTypes) &&
      newResultTypes == SmallVector<Type>(oldResultTypes))
    return success();

  // Body modifications only apply to definitions.
  if (!isDecl) {
    if (returnCoerced)
      insertReturnCoercion(funcOp, oldResultTypes[0], fc.ReturnInfo.CoercedType,
                           rewriter);

    if (hasSRet) {
      auto ptrTy = cir::PointerType::get(oldResultTypes[0]);
      Region &body = funcOp->getRegion(0);
      body.insertArgument(0u, ptrTy, funcOp.getLoc());
      insertSRetStores(funcOp, oldResultTypes[0], rewriter);
    }

    unsigned sretOffset = hasSRet ? 1 : 0;
    if (hasArgChanges)
      insertArgAdaptation(funcOp, fc, sretOffset, rewriter);
  }

  Type newFnTy = funcOp.cloneTypeWith(newArgTypes, newResultTypes);
  funcOp.setFunctionTypeAttr(TypeAttr::get(newFnTy));

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

    if ((argClass.Kind == ArgKind::Extend ||
         argClass.Kind == ArgKind::Direct) &&
        argClass.CoercedType && arg.getType() != argClass.CoercedType) {
      rewriter.setInsertionPoint(call);
      cir::CastKind castKind = argClass.Kind == ArgKind::Extend
                                   ? cir::CastKind::integral
                                   : cir::CastKind::bitcast;
      auto coerced = cir::CastOp::create(rewriter, call.getLoc(),
                                         argClass.CoercedType, castKind, arg);
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
    (void)newCall;

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

  if (hasResult && returnCoerced && origRetTy != coercedRetTy) {
    rewriter.setInsertionPointAfter(newCall);
    auto castBack =
        cir::CastOp::create(rewriter, call.getLoc(), origRetTy,
                            cir::CastKind::bitcast, newCall.getResult());
    call.getResult().replaceAllUsesWith(castBack);
  } else if (hasResult) {
    call.getResult().replaceAllUsesWith(newCall.getResult());
  }

  call->erase();
  return success();
}
