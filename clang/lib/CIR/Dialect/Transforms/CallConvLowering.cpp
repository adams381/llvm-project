//===- CallConvLowering.cpp - Lower CIR calling conventions ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass rewrites CIR function signatures and call sites to match the
// target ABI calling conventions.  It uses the MLIR ABI integration layer
// (ABITypeMapper + ABIRewriteContext) to map types, classify arguments,
// and drive dialect-specific rewrites.
//
// Until the LLVM ABI Lowering Library provides target-specific classifiers,
// this pass uses a stub classifier that passes scalars directly and returns
// Ignore for void.
//
//===----------------------------------------------------------------------===//

#include "PassDetail.h"
#include "TargetLowering/CIRABIRewriteContext.h"

#include "mlir/ABI/ABIRewriteContext.h"
#include "mlir/ABI/ABITypeMapper.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Interfaces/DataLayoutInterfaces.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Dialect/Passes.h"
#include "llvm/ABI/Types.h"

using namespace mlir;
using namespace mlir::abi;

namespace mlir {
#define GEN_PASS_DEF_CALLCONVLOWERING
#include "clang/CIR/Dialect/Passes.h.inc"
} // namespace mlir

namespace {

/// Stub ABI classifier used until the LLVM ABI Lowering Library provides
/// real target-specific classifiers (X86_64, AArch64, etc.).
///
/// Rules:
///  - Void/NoneType returns  -> Ignore
///  - Scalar types           -> Direct (pass as-is)
///  - Everything else        -> Direct (placeholder)
FunctionClassification
stubClassify(FunctionOpInterface funcOp,
             ABITypeMapper &typeMapper) {
  FunctionClassification fc;

  // Classify the return type.
  ArrayRef<Type> resultTypes = funcOp.getResultTypes();
  if (resultTypes.empty())
    fc.ReturnInfo = ArgClassification::getIgnore();
  else
    fc.ReturnInfo = ArgClassification::getDirect();

  // Classify each argument.
  for (unsigned i = 0, e = funcOp.getNumArguments(); i < e; ++i)
    fc.ArgInfos.push_back(ArgClassification::getDirect());

  return fc;
}

struct CallConvLoweringPass
    : public impl::CallConvLoweringBase<CallConvLoweringPass> {
  CallConvLoweringPass() = default;

  void runOnOperation() override {
    ModuleOp module = getOperation();
    DataLayout dataLayout(module);
    ABITypeMapper typeMapper(dataLayout);
    cir::CIRABIRewriteContext rewriteCtx(module);

    module.walk([&](FunctionOpInterface funcOp) {
      // Skip declarations (no body to rewrite).
      if (funcOp.isDeclaration())
        return;

      FunctionClassification fc = stubClassify(funcOp, typeMapper);

      OpBuilder builder(funcOp);
      if (failed(rewriteCtx.rewriteFunctionDefinition(funcOp, fc, builder)))
        return signalPassFailure();
    });
  }
};

} // namespace

std::unique_ptr<Pass> mlir::createCallConvLoweringPass() {
  return std::make_unique<CallConvLoweringPass>();
}
