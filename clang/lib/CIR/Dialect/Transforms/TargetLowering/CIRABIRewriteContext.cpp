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
  funcOp->walk([&](cir::ReturnOp retOp) {
    if (retOp.getInput().empty())
      return;

    Value origVal = retOp.getInput()[0];
    if (origVal.getType() == coercedRetTy)
      return;

    rewriter.setInsertionPoint(retOp);
    auto castOp = cir::CastOp::create(rewriter, retOp.getLoc(), coercedRetTy,
                                       cir::CastKind::bitcast, origVal);
    retOp->setOperand(0, castOp);
  });
}

/// Rewrite each cir.return to store the return value through the sret
/// pointer (first block argument) and return void.
static void insertSRetStores(FunctionOpInterface funcOp, Type origRetTy,
                             OpBuilder &rewriter) {
  Value sretPtr = funcOp.getArguments()[0];
  funcOp->walk([&](cir::ReturnOp retOp) {
    if (retOp.getInput().empty())
      return;

    Value retVal = retOp.getInput()[0];
    rewriter.setInsertionPoint(retOp);
    cir::StoreOp::create(rewriter, retOp.getLoc(), retVal, sretPtr,
                         /*isVolatile=*/mlir::UnitAttr(),
                         /*alignment=*/mlir::IntegerAttr(),
                         /*sync_scope=*/cir::SyncScopeKindAttr(),
                         /*mem_order=*/cir::MemOrderAttr());
    cir::ReturnOp::create(rewriter, retOp.getLoc());
    retOp->erase();
  });
}

LogicalResult CIRABIRewriteContext::rewriteFunctionDefinition(
    FunctionOpInterface funcOp, const FunctionClassification &fc,
    OpBuilder &rewriter) {
  ArrayRef<Type> oldArgTypes = funcOp.getArgumentTypes();
  ArrayRef<Type> oldResultTypes = funcOp.getResultTypes();

  bool hasSRet = fc.ReturnInfo.Kind == ArgKind::Indirect &&
                 !oldResultTypes.empty();
  bool returnCoerced = false;

  // Compute new argument types.
  SmallVector<Type> newArgTypes;

  // Prepend sret pointer argument if return is indirect.
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
    // Indirect return: CIR uses explicit VoidType (not empty results).
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
  if (!hasSRet &&
      newArgTypes == SmallVector<Type>(oldArgTypes) &&
      newResultTypes == SmallVector<Type>(oldResultTypes))
    return success();

  // Insert body adaptations before changing the signature.
  if (returnCoerced)
    insertReturnCoercion(funcOp, oldResultTypes[0],
                         fc.ReturnInfo.CoercedType, rewriter);

  // For sret, add a block argument for the pointer, then rewrite returns.
  if (hasSRet) {
    auto ptrTy = cir::PointerType::get(oldResultTypes[0]);
    Region &body = funcOp->getRegion(0);
    body.insertArgument(0u, ptrTy, funcOp.getLoc());
    insertSRetStores(funcOp, oldResultTypes[0], rewriter);
  }

  Type newFnTy = funcOp.cloneTypeWith(newArgTypes, newResultTypes);
  funcOp.setFunctionTypeAttr(TypeAttr::get(newFnTy));

  return success();
}

LogicalResult
CIRABIRewriteContext::rewriteCallSite(Operation *callOp,
                                      const FunctionClassification &fc,
                                      OpBuilder &rewriter) {
  auto call = cast<cir::CallOp>(callOp);
  bool hasSRet = fc.ReturnInfo.Kind == ArgKind::Indirect;

  // Handle indirect return (sret) at call site.
  if (hasSRet && call.getNumResults() > 0) {
    Type origRetTy = call.getResult().getType();
    auto ptrTy = cir::PointerType::get(origRetTy);

    rewriter.setInsertionPoint(call);

    // Allocate stack space for the return value.
    auto alloca = cir::AllocaOp::create(
        rewriter, call.getLoc(), ptrTy, origRetTy,
        /*name=*/rewriter.getStringAttr("sret"),
        /*alignment=*/rewriter.getI64IntegerAttr(8));

    // Prepend the sret pointer to the argument list.
    SmallVector<Value> newArgs;
    newArgs.push_back(alloca);
    newArgs.append(call.getArgOperands().begin(),
                   call.getArgOperands().end());

    // Create a void call (no result).
    auto voidTy = cir::VoidType::get(call.getContext());
    auto newCall = cir::CallOp::create(
        rewriter, call.getLoc(), call.getCalleeAttr(), voidTy, newArgs);
    (void)newCall;

    // Load the result from the sret pointer.
    rewriter.setInsertionPointAfter(newCall);
    auto load = cir::LoadOp::create(rewriter, call.getLoc(), origRetTy,
                                    alloca,
                                    /*isDeref=*/mlir::UnitAttr(),
                                    /*isVolatile=*/mlir::UnitAttr(),
                                    /*alignment=*/mlir::IntegerAttr(),
                                    /*sync_scope=*/cir::SyncScopeKindAttr(),
                                    /*mem_order=*/cir::MemOrderAttr());

    call.getResult().replaceAllUsesWith(load);
    call->erase();
    return success();
  }

  // Handle direct return coercion (existing logic).
  bool returnCoerced = false;
  Type coercedRetTy;
  if ((fc.ReturnInfo.Kind == ArgKind::Direct ||
       fc.ReturnInfo.Kind == ArgKind::Extend) &&
      fc.ReturnInfo.CoercedType) {
    returnCoerced = true;
    coercedRetTy = fc.ReturnInfo.CoercedType;
  }

  if (!returnCoerced)
    return success();

  Type origRetTy = call.getResult().getType();
  if (origRetTy == coercedRetTy)
    return success();

  rewriter.setInsertionPoint(call);
  auto newCall = cir::CallOp::create(
      rewriter, call.getLoc(), call.getCalleeAttr(), coercedRetTy,
      call.getArgOperands());

  rewriter.setInsertionPointAfter(newCall);
  auto castBack = cir::CastOp::create(rewriter, call.getLoc(), origRetTy,
                                       cir::CastKind::bitcast,
                                       newCall.getResult());

  call.getResult().replaceAllUsesWith(castBack);
  call->erase();

  return success();
}
