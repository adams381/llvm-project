//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements machinery for any CIR <-> CIR passes used by clang.
//
//===----------------------------------------------------------------------===//

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/PassManager.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/CIR/Dialect/Passes.h"
#include "llvm/Support/TimeProfiler.h"

namespace cir {
mlir::LogicalResult
runCIRToCIRPasses(mlir::ModuleOp theModule, mlir::MLIRContext &mlirContext,
                  clang::ASTContext &astContext, bool enableVerifier,
                  bool enableCIRSimplify, bool passByValueIsNoAlias) {

  llvm::TimeTraceScope scope("CIR To CIR Passes");

  mlir::PassManager pm(&mlirContext);
  pm.addPass(mlir::createCIRCanonicalizePass());

  if (enableCIRSimplify)
    pm.addPass(mlir::createCIRSimplifyPass());

  pm.addPass(mlir::createTargetLoweringPass());
  pm.addPass(mlir::createCXXABILoweringPass());

  unsigned avxLevel = 0; // X86AVXABILevel::None
  if (astContext.getTargetInfo().getTriple().getArch() ==
      llvm::Triple::x86_64) {
    llvm::StringRef abi = astContext.getTargetInfo().getABI();
    if (abi == "avx512")
      avxLevel = 2; // X86AVXABILevel::AVX512
    else if (abi == "avx")
      avxLevel = 1; // X86AVXABILevel::AVX
  }

  pm.addPass(mlir::createCallConvLoweringPass(/*recordCoercionEnabled=*/false,
                                              passByValueIsNoAlias, avxLevel));
  pm.addPass(mlir::createLoweringPreparePass(&astContext));

  pm.enableVerifier(enableVerifier);
  (void)mlir::applyPassManagerCLOptions(pm);
  return pm.run(theModule);
}

} // namespace cir

namespace mlir {

void populateCIRPreLoweringPasses(OpPassManager &pm) {
  pm.addPass(createHoistAllocasPass());
  pm.addPass(createCIRFlattenCFGPass());
  pm.addPass(createGotoSolverPass());
}

} // namespace mlir
