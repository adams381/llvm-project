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
#include "clang/CIR/Dialect/IR/CIRDialect.h"

using namespace cir;
using namespace mlir;
using namespace mlir::abi;

/// Insert a cir.cast bitcast before each cir.return in the function body
/// to coerce the return value from the original type to the ABI type.
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

LogicalResult CIRABIRewriteContext::rewriteFunctionDefinition(
    FunctionOpInterface funcOp, const FunctionClassification &fc,
    OpBuilder &rewriter) {
  ArrayRef<Type> oldArgTypes = funcOp.getArgumentTypes();
  ArrayRef<Type> oldResultTypes = funcOp.getResultTypes();

  // Compute new argument types.
  SmallVector<Type> newArgTypes;
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
  bool returnCoerced = false;
  if (fc.ReturnInfo.Kind == ArgKind::Direct ||
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
  if (newArgTypes == SmallVector<Type>(oldArgTypes) &&
      newResultTypes == SmallVector<Type>(oldResultTypes))
    return success();

  // If the return type was coerced, insert bitcasts at every cir.return
  // before changing the function signature.
  if (returnCoerced)
    insertReturnCoercion(funcOp, oldResultTypes[0],
                         fc.ReturnInfo.CoercedType, rewriter);

  Type newFnTy = funcOp.cloneTypeWith(newArgTypes, newResultTypes);
  funcOp.setFunctionTypeAttr(TypeAttr::get(newFnTy));

  return success();
}

LogicalResult
CIRABIRewriteContext::rewriteCallSite(Operation *callOp,
                                      const FunctionClassification &fc,
                                      OpBuilder &rewriter) {
  // TODO: Implement call-site rewriting.
  return success();
}
