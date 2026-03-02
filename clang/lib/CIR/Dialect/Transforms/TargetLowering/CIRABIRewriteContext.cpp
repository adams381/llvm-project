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
  if (fc.ReturnInfo.Kind == ArgKind::Direct ||
      fc.ReturnInfo.Kind == ArgKind::Extend) {
    if (fc.ReturnInfo.CoercedType)
      newResultTypes.push_back(fc.ReturnInfo.CoercedType);
    else
      newResultTypes = SmallVector<Type>(oldResultTypes);
  }

  // If nothing changed, skip the rewrite.
  if (newArgTypes == SmallVector<Type>(oldArgTypes) &&
      newResultTypes == SmallVector<Type>(oldResultTypes))
    return success();

  Type newFnTy = funcOp.cloneTypeWith(newArgTypes, newResultTypes);
  funcOp.setFunctionTypeAttr(TypeAttr::get(newFnTy));

  return success();
}

LogicalResult
CIRABIRewriteContext::rewriteCallSite(Operation *callOp,
                                      const FunctionClassification &fc,
                                      OpBuilder &rewriter) {
  // TODO: Implement call-site rewriting.
  // This involves:
  //  1. Coercing arguments to ABI types before the call.
  //  2. Creating sret alloca + passing pointer for indirect returns.
  //  3. Adapting the call result back to the original type.
  return success();
}
