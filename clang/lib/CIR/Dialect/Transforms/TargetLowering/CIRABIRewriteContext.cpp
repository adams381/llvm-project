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

/// For each extended argument, insert a truncation cast at the function
/// entry and replace all uses of the block argument with the truncated
/// value.
static void insertArgAdaptation(FunctionOpInterface funcOp,
                                const FunctionClassification &fc,
                                unsigned sretOffset, OpBuilder &rewriter) {
  Region &body = funcOp->getRegion(0);
  if (body.empty())
    return;

  Block &entryBlock = body.front();
  rewriter.setInsertionPointToStart(&entryBlock);

  for (auto [idx, argClass] : llvm::enumerate(fc.ArgInfos)) {
    if (argClass.Kind != ArgKind::Extend || !argClass.CoercedType)
      continue;

    unsigned blockIdx = idx + sretOffset;
    BlockArgument blockArg = entryBlock.getArgument(blockIdx);

    Type oldArgTy = blockArg.getType();
    Type newArgTy = argClass.CoercedType;

    if (oldArgTy == newArgTy)
      continue;

    // Change the block argument type to the coerced type.
    blockArg.setType(newArgTy);

    // Insert a truncation cast from the coerced type back to the
    // original type at the start of the entry block.
    rewriter.setInsertionPointToStart(&entryBlock);
    auto truncCast = cir::CastOp::create(rewriter, funcOp.getLoc(),
                                          oldArgTy, cir::CastKind::integral,
                                          blockArg);

    // Replace all uses of the block argument (except the cast itself)
    // with the truncated value.
    blockArg.replaceAllUsesExcept(truncCast, truncCast.getOperation());
  }
}

LogicalResult CIRABIRewriteContext::rewriteFunctionDefinition(
    FunctionOpInterface funcOp, const FunctionClassification &fc,
    OpBuilder &rewriter) {
  ArrayRef<Type> oldArgTypes = funcOp.getArgumentTypes();
  ArrayRef<Type> oldResultTypes = funcOp.getResultTypes();
  bool isDecl = funcOp.isDeclaration();

  bool hasSRet = fc.ReturnInfo.Kind == ArgKind::Indirect &&
                 !oldResultTypes.empty();
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
      newArgTypes.push_back(argClass.CoercedType ? argClass.CoercedType
                                                 : origTy);
      if (argClass.CoercedType && argClass.CoercedType != origTy)
        hasArgChanges = true;
      break;
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
      insertReturnCoercion(funcOp, oldResultTypes[0],
                           fc.ReturnInfo.CoercedType, rewriter);

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

LogicalResult
CIRABIRewriteContext::rewriteCallSite(Operation *callOp,
                                      const FunctionClassification &fc,
                                      OpBuilder &rewriter) {
  auto call = cast<cir::CallOp>(callOp);
  bool hasSRet = fc.ReturnInfo.Kind == ArgKind::Indirect;

  // Coerce arguments at the call site (e.g., extend i8 to i32).
  SmallVector<Value> newArgs;
  bool argsChanged = false;
  auto argOperands = call.getArgOperands();

  for (auto [idx, argClass] : llvm::enumerate(fc.ArgInfos)) {
    if (idx >= argOperands.size())
      break;

    Value arg = argOperands[idx];

    if (argClass.Kind == ArgKind::Extend && argClass.CoercedType &&
        arg.getType() != argClass.CoercedType) {
      rewriter.setInsertionPoint(call);
      auto widened = cir::CastOp::create(rewriter, call.getLoc(),
                                          argClass.CoercedType,
                                          cir::CastKind::integral, arg);
      newArgs.push_back(widened);
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

    auto alloca = cir::AllocaOp::create(
        rewriter, call.getLoc(), ptrTy, origRetTy,
        /*name=*/rewriter.getStringAttr("sret"),
        /*alignment=*/rewriter.getI64IntegerAttr(8));

    SmallVector<Value> sretArgs;
    sretArgs.push_back(alloca);
    sretArgs.append(newArgs.begin(), newArgs.end());

    auto voidTy = cir::VoidType::get(call.getContext());
    auto newCall = cir::CallOp::create(
        rewriter, call.getLoc(), call.getCalleeAttr(), voidTy, sretArgs);
    (void)newCall;

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
  auto newCall = cir::CallOp::create(
      rewriter, call.getLoc(), call.getCalleeAttr(), callRetTy, newArgs);

  if (hasResult && returnCoerced && origRetTy != coercedRetTy) {
    rewriter.setInsertionPointAfter(newCall);
    auto castBack = cir::CastOp::create(rewriter, call.getLoc(), origRetTy,
                                         cir::CastKind::bitcast,
                                         newCall.getResult());
    call.getResult().replaceAllUsesWith(castBack);
  } else if (hasResult) {
    call.getResult().replaceAllUsesWith(newCall.getResult());
  }

  call->erase();
  return success();
}
