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
// (ABITypeMapper + ABIRewriteContext) to map types, classify arguments
// via the LLVM ABI Lowering Library, and drive dialect-specific rewrites.
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
#include "llvm/ABI/ABIFunctionInfo.h"
#include "llvm/ABI/ABIInfo.h"
#include "llvm/ABI/TargetCodegenInfo.h"
#include "llvm/ABI/Types.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/TargetParser/Triple.h"

using namespace mlir;
using namespace mlir::abi;

namespace mlir {
#define GEN_PASS_DEF_CALLCONVLOWERING
#include "clang/CIR/Dialect/Passes.h.inc"
} // namespace mlir

namespace {

/// Convert an abi::Type* coercion type back to a CIR type.
///
/// The classifier produces coercion types expressed as abi::Type*
/// (e.g. abi::IntegerType(64) for the div_t -> i64 case).  We need
/// to convert these back to CIR types for the rewrite context.
static mlir::Type abiTypeToCIR(const llvm::abi::Type *ty, MLIRContext *ctx) {
  if (!ty)
    return nullptr;

  if (ty->isVoid())
    return cir::VoidType::get(ctx);

  if (auto *intTy = llvm::dyn_cast<llvm::abi::IntegerType>(ty)) {
    uint64_t width = intTy->getSizeInBits().getFixedValue();
    return cir::IntType::get(ctx, width, intTy->isSigned());
  }

  // Fallback: represent as an unsigned integer of the same bit width.
  uint64_t bits = ty->getSizeInBits().getFixedValue();
  if (bits > 0)
    return cir::IntType::get(ctx, bits, /*isSigned=*/false);

  return nullptr;
}

/// Convert an abi::ABIArgInfo classification result into our
/// dialect-agnostic ArgClassification.
///
/// \p origTy is the original CIR type before classification.  When the
/// coercion type has the same bit width as the original, we use the
/// original to avoid spurious signedness mismatches.
static ArgClassification
convertABIArgInfo(const llvm::abi::ABIArgInfo &info, MLIRContext *ctx,
                  mlir::Type origTy) {
  switch (info.getKind()) {
  case llvm::abi::ABIArgInfo::Direct: {
    mlir::Type coerced = abiTypeToCIR(info.getCoerceToType(), ctx);
    // If the coerced type has the same bit width as the original,
    // the coercion is a no-op (just a signedness difference).  Use
    // null to mean "keep original type".
    if (coerced && origTy && coerced != origTy) {
      if (auto coercedInt = dyn_cast<cir::IntType>(coerced))
        if (auto origInt = dyn_cast<cir::IntType>(origTy))
          if (coercedInt.getWidth() == origInt.getWidth())
            coerced = nullptr;
    }
    return ArgClassification::getDirect(coerced);
  }
  case llvm::abi::ABIArgInfo::Extend: {
    mlir::Type coerced = abiTypeToCIR(info.getCoerceToType(), ctx);
    return ArgClassification::getExtend(coerced, info.isSignExt());
  }
  case llvm::abi::ABIArgInfo::Indirect:
    return ArgClassification::getIndirect(
        llvm::Align(info.getIndirectAlign()), info.getIndirectByVal());
  case llvm::abi::ABIArgInfo::Ignore:
    return ArgClassification::getIgnore();
  default:
    llvm_unreachable("ABI arg info kind not yet supported in CIR");
  }
}

/// Classify a function using the real LLVM ABI Lowering Library.
///
/// Maps CIR types to abi::Type* via ABITypeMapper, calls the X86_64
/// classifier, and converts the results back to FunctionClassification.
FunctionClassification
classifyWithABILibrary(FunctionOpInterface funcOp,
                       ABITypeMapper &typeMapper,
                       const llvm::abi::ABIInfo &abiInfo) {
  FunctionClassification fc;
  MLIRContext *ctx = funcOp->getContext();

  // Map return type.
  ArrayRef<Type> resultTypes = funcOp.getResultTypes();
  const llvm::abi::Type *retAbiTy = nullptr;
  if (resultTypes.empty())
    retAbiTy = typeMapper.getTypeBuilder().getVoidType();
  else
    retAbiTy = typeMapper.map(resultTypes[0]);

  // Map argument types.
  SmallVector<const llvm::abi::Type *> argAbiTypes;
  for (Type argTy : funcOp.getArgumentTypes()) {
    const llvm::abi::Type *mapped = typeMapper.map(argTy);
    assert(mapped && "ABITypeMapper failed to map CIR type");
    argAbiTypes.push_back(mapped);
  }
  assert(retAbiTy && "ABITypeMapper failed to map return type");

  // Create ABIFunctionInfo, classify, and convert results.
  std::unique_ptr<llvm::abi::ABIFunctionInfo,
                  void (*)(llvm::abi::ABIFunctionInfo *)>
      abiFI(llvm::abi::ABIFunctionInfo::create(llvm::CallingConv::C, retAbiTy,
                                                argAbiTypes),
            [](llvm::abi::ABIFunctionInfo *p) { p->operator delete(p); });
  abiInfo.computeInfo(*abiFI);

  // Convert return classification.
  Type origRetTy = resultTypes.empty() ? Type() : resultTypes[0];
  fc.ReturnInfo = convertABIArgInfo(abiFI->getReturnInfo(), ctx, origRetTy);

  // Convert argument classifications.
  ArrayRef<Type> argTypes = funcOp.getArgumentTypes();
  for (unsigned i = 0, e = abiFI->getNumArgs(); i < e; ++i) {
    Type origArgTy = i < argTypes.size() ? argTypes[i] : Type();
    fc.ArgInfos.push_back(
        convertABIArgInfo(abiFI->getArgInfo(i).ArgInfo, ctx, origArgTy));
  }

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

    // Read target triple from the module.  When not present (e.g. CIR
    // parsed from text without a triple), fall back to x86_64-linux.
    llvm::Triple triple("x86_64-unknown-linux-gnu");
    if (auto tripleAttr = module->getAttrOfType<StringAttr>(
            cir::CIRDialect::getTripleAttrName()))
      triple = llvm::Triple(tripleAttr.getValue());
    auto targetCGI = llvm::abi::createX8664TargetCodeGenInfo(
        typeMapper.getTypeBuilder(), triple,
        llvm::abi::X86AVXABILevel::None,
        /*Has64BitPointers=*/true,
        llvm::abi::ABICompatInfo());
    const llvm::abi::ABIInfo &abiInfo = targetCGI->getABIInfo();

    // Phase 1: Classify and rewrite all functions (both definitions
    // and declarations).
    llvm::DenseMap<StringRef, FunctionClassification> classificationMap;

    module.walk([&](FunctionOpInterface funcOp) {
      FunctionClassification fc =
          classifyWithABILibrary(funcOp, typeMapper, abiInfo);

      if (auto nameAttr = funcOp->getAttrOfType<StringAttr>("sym_name"))
        classificationMap[nameAttr.getValue()] = fc;

      OpBuilder builder(funcOp);
      if (failed(rewriteCtx.rewriteFunctionDefinition(funcOp, fc, builder)))
        return signalPassFailure();
    });

    // Phase 2: Rewrite call sites to match rewritten callees.
    module.walk([&](cir::CallOp callOp) {
      auto callee = callOp.getCalleeAttr();
      if (!callee)
        return;

      auto it = classificationMap.find(callee.getValue());
      if (it == classificationMap.end())
        return;

      OpBuilder builder(callOp);
      if (failed(rewriteCtx.rewriteCallSite(callOp, it->second, builder)))
        return signalPassFailure();
    });
  }
};

} // namespace

std::unique_ptr<Pass> mlir::createCallConvLoweringPass() {
  return std::make_unique<CallConvLoweringPass>();
}
