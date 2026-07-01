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
#include "llvm/TargetParser/Triple.h"

namespace cir {

mlir::LogicalResult
runCIRToCIRPasses(mlir::ModuleOp theModule, mlir::MLIRContext &mlirContext,
                  clang::ASTContext &astContext, bool enableVerifier,
                  bool enableIdiomRecognizer, bool enableCIRSimplify,
                  bool enableLibOpt, llvm::StringRef libOptOptions,
                  bool enableCallConvLowering) {

  llvm::TimeTraceScope scope("CIR To CIR Passes");

  mlir::PassManager pm(&mlirContext);
  pm.addPass(mlir::createCIRCanonicalizePass());

  if (enableCIRSimplify)
    pm.addPass(mlir::createCIRSimplifyPass());

  if (enableIdiomRecognizer)
    pm.addPass(mlir::createIdiomRecognizerPass(&astContext));

  if (enableLibOpt) {
    auto libOptPass = mlir::createLibOptPass();
    auto errorHandler = [](const llvm::Twine &) -> mlir::LogicalResult {
      return mlir::LogicalResult::failure();
    };

    if (libOptPass->initializeOptions(libOptOptions, errorHandler).failed())
      return mlir::failure();

    pm.addPass(std::move(libOptPass));
  }

  pm.addPass(mlir::createTargetLoweringPass());
  pm.addPass(mlir::createCXXABILoweringPass());

  // Calling-convention lowering currently supports only the x86_64 System V
  // ABI classifier from the LLVM ABI library.  Gate on x86_64 so other
  // targets are unaffected, and on the -fno-clangir-call-conv-lowering
  // opt-out.
  if (enableCallConvLowering &&
      astContext.getTargetInfo().getTriple().getArch() ==
          llvm::Triple::x86_64) {
    unsigned avxLevel = 0;
    llvm::StringRef abi = astContext.getTargetInfo().getABI();
    if (abi == "avx512")
      avxLevel = 2;
    else if (abi == "avx")
      avxLevel = 1;
    pm.addPass(mlir::createCallConvLoweringPass("x86_64", avxLevel));
  }

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
  pm.addPass(createCIREHABILoweringPass());
  pm.addPass(createGotoSolverPass());
}

} // namespace mlir
