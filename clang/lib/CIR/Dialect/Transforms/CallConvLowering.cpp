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
    // Suppress coercion when it is a no-op:
    //
    // 1. Same-width integer signedness difference (e.g. !s32i vs !u32i).
    // 2. The original type is NOT a record.  The ABITypeMapper
    //    fallback maps every non-record CIR type (pointers, bools,
    //    etc.) to an integer of equal bit width.  The classifier
    //    echoes that integer back as the coercion type, producing a
    //    spurious coercion.  Real coercion only applies to records
    //    that must be split/packed into registers.
    if (coerced && origTy && coerced != origTy) {
      if (!isa<cir::RecordType>(origTy))
        coerced = nullptr;
      else if (auto coercedInt = dyn_cast<cir::IntType>(coerced))
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

/// Map a CIR type to an abi::Type* with CIR-specific knowledge.
///
/// The generic ABITypeMapper only handles MLIR built-in types.  CIR
/// types (cir::IntType, cir::PointerType, cir::RecordType, etc.)
/// need dialect-aware mapping to preserve signedness, field layout,
/// and pointer semantics.
static const llvm::abi::Type *
mapCIRType(mlir::Type type, ABITypeMapper &typeMapper,
           const DataLayout &dl) {
  llvm::abi::TypeBuilder &tb = typeMapper.getTypeBuilder();

  if (auto intTy = dyn_cast<cir::IntType>(type)) {
    uint64_t width = intTy.getWidth();
    uint64_t abiAlign = dl.getTypeABIAlignment(type);
    return tb.getIntegerType(width, llvm::Align(abiAlign), intTy.isSigned());
  }

  if (isa<cir::PointerType>(type)) {
    uint64_t sizeBits = dl.getTypeSizeInBits(type);
    uint64_t abiAlign = dl.getTypeABIAlignment(type);
    return tb.getPointerType(sizeBits, llvm::Align(abiAlign),
                             /*AddressSpace=*/0);
  }

  if (isa<cir::BoolType>(type)) {
    uint64_t sizeBits = dl.getTypeSizeInBits(type);
    uint64_t abiAlign = dl.getTypeABIAlignment(type);
    return tb.getIntegerType(sizeBits, llvm::Align(abiAlign),
                             /*Signed=*/false);
  }

  if (isa<cir::VoidType>(type))
    return tb.getVoidType();

  if (auto recTy = dyn_cast<cir::RecordType>(type)) {
    SmallVector<llvm::abi::FieldInfo> fields;
    uint64_t offsetBits = 0;
    for (mlir::Type fieldTy : recTy.getMembers()) {
      const llvm::abi::Type *mappedField =
          mapCIRType(fieldTy, typeMapper, dl);
      uint64_t fieldSize = dl.getTypeSizeInBits(fieldTy);
      fields.push_back({mappedField, offsetBits,
                         /*BitFieldWidth=*/0,
                         /*IsBitField=*/false,
                         /*IsUnnamedBitfield=*/false});
      offsetBits += fieldSize;
    }
    llvm::TypeSize size = dl.getTypeSizeInBits(type);
    uint64_t abiAlign = dl.getTypeABIAlignment(type);
    // Plain C/C++ data structs without non-trivial copy/dtor can be
    // passed in registers.  CIR record types at this stage are always
    // trivially copyable (non-trivial semantics are lowered earlier).
    return tb.getRecordType(fields, size, llvm::Align(abiAlign),
                            llvm::abi::StructPacking::Default,
                            /*BaseClasses=*/{},
                            /*VirtualBaseClasses=*/{},
                            /*CXXRecord=*/false,
                            /*Polymorphic=*/false,
                            /*NonTrivialCopy=*/false,
                            /*NonTrivialDtor=*/false,
                            /*FlexibleArray=*/false,
                            /*UnalignedFields=*/false,
                            /*CanPassInRegister=*/true);
  }

  // Fall back to the generic mapper for MLIR built-in types.
  return typeMapper.map(type);
}

/// Classify a function using the real LLVM ABI Lowering Library.
///
/// Maps CIR types to abi::Type* via CIR-aware mapping, calls the
/// X86_64 classifier, and converts the results back to
/// FunctionClassification.
FunctionClassification
classifyWithABILibrary(FunctionOpInterface funcOp,
                       ABITypeMapper &typeMapper,
                       const DataLayout &dl,
                       const llvm::abi::ABIInfo &abiInfo) {
  FunctionClassification fc;
  MLIRContext *ctx = funcOp->getContext();

  // Map return type.
  ArrayRef<Type> resultTypes = funcOp.getResultTypes();
  const llvm::abi::Type *retAbiTy = nullptr;
  if (resultTypes.empty())
    retAbiTy = typeMapper.getTypeBuilder().getVoidType();
  else
    retAbiTy = mapCIRType(resultTypes[0], typeMapper, dl);

  // Map argument types.
  SmallVector<const llvm::abi::Type *> argAbiTypes;
  for (Type argTy : funcOp.getArgumentTypes()) {
    const llvm::abi::Type *mapped = mapCIRType(argTy, typeMapper, dl);
    assert(mapped && "failed to map CIR type to ABI type");
    argAbiTypes.push_back(mapped);
  }
  assert(retAbiTy && "failed to map return type to ABI type");

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
          classifyWithABILibrary(funcOp, typeMapper, dataLayout, abiInfo);

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
