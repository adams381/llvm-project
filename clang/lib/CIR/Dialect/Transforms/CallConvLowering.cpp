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
// A mock X86_64 classifier handles common patterns (struct coercion,
// integer extension, indirect returns) until the LLVM ABI Lowering Library
// provides real target-specific classifiers.
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

/// Classify a single type for the x86_64 System V ABI.
///
/// This is a mock that covers common patterns.  It will be replaced by
/// the real LLVM ABI library classifier when it lands upstream.
ArgClassification classifyTypeX86_64(Type ty, const DataLayout &dl,
                                     bool isReturnType) {
  // Records: inspect size and decide coercion vs indirect.
  if (auto recTy = dyn_cast<cir::RecordType>(ty)) {
    if (!recTy.isComplete())
      return ArgClassification::getDirect();

    uint64_t sizeInBytes = dl.getTypeSize(recTy);

    // Structs > 16 bytes: pass/return indirectly.
    if (sizeInBytes > 16)
      return ArgClassification::getIndirect(
          llvm::Align(dl.getTypeABIAlignment(recTy)));

    // Structs <= 8 bytes with all-integer fields: coerce to a single
    // integer register.  This covers div_t {int, int} -> i64.
    if (sizeInBytes <= 8) {
      bool allInteger = true;
      for (Type member : recTy.getMembers()) {
        if (!isa<cir::IntType>(member)) {
          allInteger = false;
          break;
        }
      }
      if (allInteger) {
        unsigned coerceBits = sizeInBytes * 8;
        auto coerceTy =
            cir::IntType::get(ty.getContext(), coerceBits, /*isSigned=*/false);
        return ArgClassification::getDirect(coerceTy);
      }
    }

    // Other small structs: pass directly for now.
    return ArgClassification::getDirect();
  }

  // Small integers (i8, i16): extend when passed as arguments.
  if (auto intTy = dyn_cast<cir::IntType>(ty)) {
    if (!isReturnType && intTy.getWidth() < 32) {
      auto i32Ty =
          cir::IntType::get(ty.getContext(), 32, intTy.isSigned());
      return ArgClassification::getExtend(i32Ty, intTy.isSigned());
    }
  }

  return ArgClassification::getDirect();
}

/// Mock X86_64 System V ABI classifier.
FunctionClassification
mockClassifyX86_64(FunctionOpInterface funcOp, const DataLayout &dl) {
  FunctionClassification fc;

  // Classify the return type.
  ArrayRef<Type> resultTypes = funcOp.getResultTypes();
  if (resultTypes.empty())
    fc.ReturnInfo = ArgClassification::getIgnore();
  else
    fc.ReturnInfo = classifyTypeX86_64(resultTypes[0], dl,
                                       /*isReturnType=*/true);

  // Classify each argument.
  for (Type argTy : funcOp.getArgumentTypes())
    fc.ArgInfos.push_back(
        classifyTypeX86_64(argTy, dl, /*isReturnType=*/false));

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
      if (funcOp.isDeclaration())
        return;

      FunctionClassification fc = mockClassifyX86_64(funcOp, dataLayout);

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
