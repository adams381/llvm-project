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

  if (auto *fltTy = llvm::dyn_cast<llvm::abi::FloatType>(ty)) {
    const llvm::fltSemantics *sem = fltTy->getSemantics();
    if (sem == &llvm::APFloat::IEEEhalf())
      return cir::FP16Type::get(ctx);
    if (sem == &llvm::APFloat::IEEEsingle())
      return cir::SingleType::get(ctx);
    if (sem == &llvm::APFloat::IEEEdouble())
      return cir::DoubleType::get(ctx);
    if (sem == &llvm::APFloat::x87DoubleExtended())
      return cir::FP80Type::get(ctx);
    if (sem == &llvm::APFloat::IEEEquad())
      return cir::FP128Type::get(ctx);
  }

  if (auto *vecTy = llvm::dyn_cast<llvm::abi::VectorType>(ty)) {
    mlir::Type elemCIR = abiTypeToCIR(vecTy->getElementType(), ctx);
    if (!elemCIR)
      return nullptr;
    uint64_t numElts = vecTy->getNumElements().getFixedValue();
    return cir::VectorType::get(elemCIR, numElts);
  }

  // Multi-register coercion: the ABI library produces a coerced
  // RecordType (e.g. {i64, i64} for a 16-byte struct returned in
  // two registers).  Convert each field to a CIR type and build
  // an anonymous CIR RecordType that lowers to an LLVM struct.
  if (auto *recTy = llvm::dyn_cast<llvm::abi::RecordType>(ty)) {
    SmallVector<mlir::Type> fieldTypes;
    for (const auto &field : recTy->getFields()) {
      mlir::Type fieldCIR = abiTypeToCIR(field.FieldType, ctx);
      if (!fieldCIR)
        return nullptr;
      fieldTypes.push_back(fieldCIR);
    }
    return cir::RecordType::get(ctx, fieldTypes, /*packed=*/false,
                                /*padded=*/false,
                                cir::RecordType::RecordKind::Struct);
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
static ArgClassification convertABIArgInfo(const llvm::abi::ABIArgInfo &info,
                                           MLIRContext *ctx, mlir::Type origTy,
                                           bool recordCoercionEnabled,
                                           bool isArgument = true) {
  // Empty trivially-copyable record arguments are not passed
  // per the x86_64 ABI.  Non-trivially-copyable empty records
  // (e.g. struct with only a destructor) must still be passed
  // indirectly.
  //
  // Return types are NOT handled here.  When a non-coroutine
  // function returning an empty struct (e.g. get_return_object)
  // is rewritten to return void, the coroutine body that calls
  // it still has a cir.store of the old return type into the
  // coroutine frame.  The Phase 3 call-site rewrite inserts a
  // dummy alloca+load, but the frame store was generated before
  // ABI lowering and references the original type, causing a
  // verification failure.  Fixing this requires either running
  // ABI lowering after coroutine lowering or teaching the pass
  // to rewrite frame stores.
  if (isArgument && origTy)
    if (auto recTy = dyn_cast<cir::RecordType>(origTy))
      if (recTy.isEmpty() && recTy.isTriviallyCopyable())
        return ArgClassification::getIgnore();

  switch (info.getKind()) {
  case llvm::abi::ABIArgInfo::Direct: {
    mlir::Type coerced = abiTypeToCIR(info.getCoerceToType(), ctx);
    // Suppress coercion when it is a no-op:
    //
    // 1. Coerced type equals original (e.g. i32 → i32).
    // 2. Original is a scalar (not record/complex/vector) — the
    //    mapper fallback produces spurious integer coercions.
    // 3. Same-width integer signedness difference on records.
    if (coerced && origTy) {
      if (coerced == origTy)
        coerced = nullptr;
      else if (!isa<cir::RecordType, cir::ComplexType, cir::VectorType>(origTy))
        coerced = nullptr;
      else if (auto coercedInt = dyn_cast<cir::IntType>(coerced))
        if (auto origInt = dyn_cast<cir::IntType>(origTy))
          if (coercedInt.getWidth() == origInt.getWidth())
            coerced = nullptr;
      // Suppress coercion for padded records whose only member
      // matches the coerced type (e.g. empty C++ class with padding
      // byte).  The memory-based coercion ops produce IR that the
      // CIR-to-LLVM lowering cannot handle for these types.
      if (coerced)
        if (auto recTy = dyn_cast<cir::RecordType>(origTy))
          if (recTy.getPadded() && recTy.getMembers().size() == 1 &&
              recTy.getMembers()[0] == coerced)
            coerced = nullptr;
    }
    auto ac = ArgClassification::getDirect(coerced);
    ac.CanFlatten = info.getCanBeFlattened();
    return ac;
  }
  case llvm::abi::ABIArgInfo::Extend: {
    mlir::Type coerced = abiTypeToCIR(info.getCoerceToType(), ctx);
    // BoolType: keep Extend classification (for signext/zeroext
    // attr) but suppress type coercion since cir.cast integral
    // on BoolType would fail verification.
    if (origTy && isa<cir::BoolType>(origTy))
      return ArgClassification::getExtend(nullptr, info.isSignExt());
    // Other non-integer types: promote during CIR-to-LLVM
    // lowering, not here.
    if (origTy && !isa<cir::IntType>(origTy))
      return ArgClassification::getDirect(nullptr);
    return ArgClassification::getExtend(coerced, info.isSignExt());
  }
  case llvm::abi::ABIArgInfo::Indirect: {
    if (!recordCoercionEnabled) {
      if (!origTy ||
          !isa<cir::RecordType, cir::VectorType, cir::ComplexType>(origTy))
        return ArgClassification::getDirect(nullptr);
    }
    return ArgClassification::getIndirect(llvm::Align(info.getIndirectAlign()),
                                          info.getIndirectByVal());
  }
  case llvm::abi::ABIArgInfo::Ignore:
    if (!recordCoercionEnabled && origTy && isa<cir::RecordType>(origTy))
      return ArgClassification::getDirect(nullptr);
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
static const llvm::abi::Type *mapCIRType(mlir::Type type,
                                         ABITypeMapper &typeMapper,
                                         const DataLayout &dl,
                                         bool recordCoercionEnabled) {
  llvm::abi::TypeBuilder &tb = typeMapper.getTypeBuilder();

  // llvm::Align requires a power of 2.  DataLayout may return
  // non-power-of-2 for unusual types (e.g. _BitInt(31)).
  auto safeAlign = [](uint64_t a) -> llvm::Align {
    return llvm::Align(llvm::PowerOf2Ceil(std::max<uint64_t>(a, 1)));
  };

  if (auto intTy = dyn_cast<cir::IntType>(type)) {
    uint64_t width = intTy.getWidth();
    return tb.getIntegerType(width, safeAlign(dl.getTypeABIAlignment(type)),
                             intTy.isSigned());
  }

  if (isa<cir::PointerType>(type)) {
    uint64_t sizeBits = dl.getTypeSizeInBits(type);
    return tb.getPointerType(sizeBits, safeAlign(dl.getTypeABIAlignment(type)),
                             /*AddressSpace=*/0);
  }

  if (isa<cir::BoolType>(type)) {
    uint64_t sizeBits = dl.getTypeSizeInBits(type);
    return tb.getIntegerType(sizeBits, safeAlign(dl.getTypeABIAlignment(type)),
                             /*Signed=*/false);
  }

  if (isa<cir::VoidType>(type))
    return tb.getVoidType();

  if (isa<cir::SingleType>(type))
    return tb.getFloatType(llvm::APFloat::IEEEsingle(),
                           safeAlign(dl.getTypeABIAlignment(type)));
  if (isa<cir::DoubleType>(type))
    return tb.getFloatType(llvm::APFloat::IEEEdouble(),
                           safeAlign(dl.getTypeABIAlignment(type)));
  if (isa<cir::FP16Type>(type))
    return tb.getFloatType(llvm::APFloat::IEEEhalf(),
                           safeAlign(dl.getTypeABIAlignment(type)));
  if (isa<cir::FP80Type>(type))
    return tb.getFloatType(llvm::APFloat::x87DoubleExtended(),
                           safeAlign(dl.getTypeABIAlignment(type)));
  if (isa<cir::FP128Type>(type))
    return tb.getFloatType(llvm::APFloat::IEEEquad(),
                           safeAlign(dl.getTypeABIAlignment(type)));

  if (auto ldTy = dyn_cast<cir::LongDoubleType>(type))
    return mapCIRType(ldTy.getUnderlying(), typeMapper, dl,
                      recordCoercionEnabled);

  if (auto arrTy = dyn_cast<cir::ArrayType>(type)) {
    const llvm::abi::Type *elemAbi = mapCIRType(
        arrTy.getElementType(), typeMapper, dl, recordCoercionEnabled);
    uint64_t sizeBits = dl.getTypeSizeInBits(type).getFixedValue();
    return tb.getArrayType(elemAbi, arrTy.getSize(), sizeBits);
  }

  if (auto complexTy = dyn_cast<cir::ComplexType>(type)) {
    const llvm::abi::Type *elemAbi = mapCIRType(
        complexTy.getElementType(), typeMapper, dl, recordCoercionEnabled);
    return tb.getComplexType(elemAbi, safeAlign(dl.getTypeABIAlignment(type)));
  }

  if (auto vecTy = dyn_cast<cir::VectorType>(type)) {
    const llvm::abi::Type *elemAbi = mapCIRType(
        vecTy.getElementType(), typeMapper, dl, recordCoercionEnabled);
    llvm::ElementCount ec = llvm::ElementCount::getFixed(vecTy.getSize());
    uint64_t sizeBits = dl.getTypeSizeInBits(type).getFixedValue();
    uint64_t alignBytes = sizeBits / 8;
    return tb.getVectorType(elemAbi, ec, safeAlign(alignBytes));
  }

  if (auto recTy = dyn_cast<cir::RecordType>(type)) {
    SmallVector<llvm::abi::FieldInfo> fields;
    bool isUnion = recTy.isUnion();
    bool isPacked = recTy.getPacked();
    auto members = recTy.getMembers();
    // Exclude trailing padding member when it only serves to
    // satisfy an alignment attribute.  OGCG only passes actual
    // data fields, so alignment-only padding inflates eightbyte
    // classification.  Detect by checking if removing the last
    // member (padding array) would keep the struct within the
    // same number of eightbytes.
    bool excludeTrailingPad = false;
    if (recTy.getPadded() && !isUnion && members.size() > 1) {
      auto lastTy = members.back();
      if (isa<cir::ArrayType>(lastTy)) {
        uint64_t withoutPad = 0;
        for (unsigned i = 0; i < members.size() - 1; ++i)
          withoutPad += dl.getTypeSizeInBits(members[i]).getFixedValue();
        uint64_t withPad =
            withoutPad + dl.getTypeSizeInBits(lastTy).getFixedValue();
        excludeTrailingPad = (withoutPad + 63) / 64 < (withPad + 63) / 64;
      }
    }
    auto endIt = excludeTrailingPad ? members.end() - 1 : members.end();
    uint64_t offsetBits = 0;
    for (auto it = members.begin(); it != endIt; ++it) {
      mlir::Type fieldTy = *it;
      const llvm::abi::Type *mappedField =
          mapCIRType(fieldTy, typeMapper, dl, recordCoercionEnabled);
      uint64_t fieldSize = dl.getTypeSizeInBits(fieldTy);
      if (!isUnion && !isPacked) {
        uint64_t fieldAlignBits = dl.getTypeABIAlignment(fieldTy) * 8;
        offsetBits = llvm::alignTo(offsetBits, fieldAlignBits);
      }
      fields.push_back({mappedField, isUnion ? 0 : offsetBits,
                        /*BitFieldWidth=*/0,
                        /*IsBitField=*/false,
                        /*IsUnnamedBitfield=*/false});
      if (!isUnion)
        offsetBits += fieldSize;
    }
    // Use data size (excluding tail alignment padding) for the
    // ABI library.  CIR padded records include explicit tail
    // padding members that inflate the layout size beyond the
    // data size.  The ABI classifier uses the data size to
    // determine how many eightbytes to classify.
    llvm::TypeSize size = dl.getTypeSizeInBits(type);
    uint64_t rawAlign = dl.getTypeABIAlignment(type);
    // CIRGen sets the triviallyCopyable flag from
    // RecordDecl::canPassInRegisters() via complete().  Anonymous
    // records default to trivially copyable in RecordTypeStorage.
    // The recordCoercionEnabled override handles cir-opt tests
    // where text-parsed records default to non-trivially-copyable.
    bool canPass = recTy.isTriviallyCopyable() || recordCoercionEnabled;
    if (isUnion)
      return tb.getUnionType(fields, size, safeAlign(rawAlign),
                             llvm::abi::StructPacking::Default,
                             /*IsTransparent=*/false,
                             /*CanPassInRegs=*/canPass);
    bool nonTrivial = !recTy.isTriviallyCopyable();
    return tb.getRecordType(fields, size, safeAlign(rawAlign),
                            llvm::abi::StructPacking::Default,
                            /*BaseClasses=*/{},
                            /*VirtualBaseClasses=*/{},
                            /*CXXRecord=*/nonTrivial,
                            /*Polymorphic=*/false,
                            /*NonTrivialCopy=*/nonTrivial,
                            /*NonTrivialDtor=*/nonTrivial,
                            /*FlexibleArray=*/false,
                            /*UnalignedFields=*/false,
                            /*CanPassInRegister=*/canPass);
  }

  // Fall back to the generic mapper for MLIR built-in types.
  return typeMapper.map(type);
}

/// Classify a function using the real LLVM ABI Lowering Library.
///
/// Maps CIR types to abi::Type* via CIR-aware mapping, calls the
/// X86_64 classifier, and converts the results back to
/// FunctionClassification.
FunctionClassification classifyWithABILibrary(FunctionOpInterface funcOp,
                                              ABITypeMapper &typeMapper,
                                              const DataLayout &dl,
                                              const llvm::abi::ABIInfo &abiInfo,
                                              bool recordCoercionEnabled) {
  FunctionClassification fc;
  MLIRContext *ctx = funcOp->getContext();

  // Map return type.
  ArrayRef<Type> resultTypes = funcOp.getResultTypes();
  const llvm::abi::Type *retAbiTy = nullptr;
  if (resultTypes.empty())
    retAbiTy = typeMapper.getTypeBuilder().getVoidType();
  else
    retAbiTy =
        mapCIRType(resultTypes[0], typeMapper, dl, recordCoercionEnabled);

  // Map argument types.
  SmallVector<const llvm::abi::Type *> argAbiTypes;
  for (Type argTy : funcOp.getArgumentTypes()) {
    const llvm::abi::Type *mapped =
        mapCIRType(argTy, typeMapper, dl, recordCoercionEnabled);
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
  fc.ReturnInfo = convertABIArgInfo(abiFI->getReturnInfo(), ctx, origRetTy,
                                    recordCoercionEnabled,
                                    /*isArgument=*/false);

  // Convert argument classifications.
  ArrayRef<Type> argTypes = funcOp.getArgumentTypes();
  for (unsigned i = 0, e = abiFI->getNumArgs(); i < e; ++i) {
    Type origArgTy = i < argTypes.size() ? argTypes[i] : Type();
    fc.ArgInfos.push_back(convertABIArgInfo(abiFI->getArgInfo(i).ArgInfo, ctx,
                                            origArgTy, recordCoercionEnabled));
  }

  // Fix up union coercion types.  The ABI library's
  // reduceUnionForX8664 picks the "best" member by alignment,
  // which may be narrower than the union's data content.  Classic
  // Clang uses the union's full sizeof (including alignment
  // padding).  Widen undersized coercion types to match.
  auto fixupUnionCoercion = [&](ArgClassification &ac, Type origTy) {
    if (ac.Kind != ArgKind::Direct || !ac.CoercedType || !origTy)
      return;
    auto recTy = dyn_cast<cir::RecordType>(origTy);
    if (!recTy || !recTy.isUnion())
      return;
    auto coercedInt = dyn_cast<cir::IntType>(ac.CoercedType);
    if (!coercedInt)
      return;
    // Compute the union's true sizeof in bits: max member size
    // (excluding padding), rounded up to alignment.  For packed
    // unions, alignment is 1 (no rounding).
    auto members = recTy.getMembers();
    auto endIt = recTy.getPadded() && !members.empty() ? members.end() - 1
                                                       : members.end();
    uint64_t maxMemberBits = 0;
    uint64_t maxAlignBytes = 1;
    for (auto it = members.begin(); it != endIt; ++it) {
      maxMemberBits =
          std::max(maxMemberBits, dl.getTypeSizeInBits(*it).getFixedValue());
      if (!recTy.getPacked())
        maxAlignBytes = std::max(maxAlignBytes, dl.getTypeABIAlignment(*it));
    }
    uint64_t alignBits = maxAlignBytes * 8;
    uint64_t unionBits = llvm::alignTo(maxMemberBits, alignBits);
    uint64_t coercedBits = coercedInt.getWidth();
    if (coercedBits < unionBits && unionBits <= 64)
      ac.CoercedType = cir::IntType::get(ctx, unionBits, coercedInt.isSigned());
  };

  if (!resultTypes.empty())
    fixupUnionCoercion(fc.ReturnInfo, origRetTy);
  for (unsigned i = 0; i < fc.ArgInfos.size(); ++i) {
    Type origArgTy = i < argTypes.size() ? argTypes[i] : Type();
    fixupUnionCoercion(fc.ArgInfos[i], origArgTy);
  }

  return fc;
}

struct CallConvLoweringPass
    : public impl::CallConvLoweringBase<CallConvLoweringPass> {
  CallConvLoweringPass() = default;
  explicit CallConvLoweringPass(bool enableRecordCoercion)
      : CallConvLoweringBase() {
    recordCoercionEnabled = enableRecordCoercion;
  }
  CallConvLoweringPass(bool enableRecordCoercion, bool noAliasForByVal)
      : CallConvLoweringBase() {
    recordCoercionEnabled = enableRecordCoercion;
    passByValueIsNoAlias = noAliasForByVal;
  }
  CallConvLoweringPass(bool enableRecordCoercion, bool noAliasForByVal,
                       unsigned avxLevel)
      : CallConvLoweringBase() {
    recordCoercionEnabled = enableRecordCoercion;
    passByValueIsNoAlias = noAliasForByVal;
    x86AvxAbiLevel = avxLevel;
  }

  void runOnOperation() override {
    ModuleOp module = getOperation();
    DataLayout dataLayout(module);
    ABITypeMapper typeMapper(dataLayout);
    cir::CIRABIRewriteContext rewriteCtx(module, passByValueIsNoAlias);

    // Read target triple from the module.  When not present (e.g. CIR
    // parsed from text without a triple), fall back to x86_64-linux.
    llvm::Triple triple("x86_64-unknown-linux-gnu");
    if (auto tripleAttr = module->getAttrOfType<StringAttr>(
            cir::CIRDialect::getTripleAttrName()))
      triple = llvm::Triple(tripleAttr.getValue());
    auto avxLevel =
        static_cast<llvm::abi::X86AVXABILevel>(x86AvxAbiLevel.getValue());
    auto targetCGI = llvm::abi::createX8664TargetCodeGenInfo(
        typeMapper.getTypeBuilder(), triple, avxLevel,
        /*Has64BitPointers=*/true, llvm::abi::ABICompatInfo());
    const llvm::abi::ABIInfo &abiInfo = targetCGI->getABIInfo();

    // Phase 1: Classify and rewrite all functions (both definitions
    // and declarations).
    llvm::DenseMap<StringRef, FunctionClassification> classificationMap;

    module.walk([&](FunctionOpInterface funcOp) {
      // Skip coroutine functions — their return semantics are handled
      // by the coroutine lowering passes and must not be rewritten.
      if (funcOp->hasAttr("coroutine"))
        return;

      FunctionClassification fc = classifyWithABILibrary(
          funcOp, typeMapper, dataLayout, abiInfo, recordCoercionEnabled);

      if (auto nameAttr = funcOp->getAttrOfType<StringAttr>("sym_name"))
        classificationMap[nameAttr.getValue()] = fc;

      OpBuilder builder(funcOp);
      if (failed(rewriteCtx.rewriteFunctionDefinition(funcOp, fc, builder)))
        return signalPassFailure();
    });

    // Phase 2: Update cir.get_global ops whose result type embeds a
    // function type that was rewritten in Phase 1.
    module.walk([&](cir::GetGlobalOp getOp) {
      FlatSymbolRefAttr sym = getOp.getNameAttr();
      auto funcOp = module.lookupSymbol<FunctionOpInterface>(sym);
      if (!funcOp)
        return;
      auto ptrTy = dyn_cast<cir::PointerType>(getOp.getType());
      if (!ptrTy)
        return;
      auto fnTy = dyn_cast<cir::FuncType>(ptrTy.getPointee());
      if (!fnTy)
        return;
      auto currentFnTy = dyn_cast<cir::FuncType>(funcOp.getFunctionType());
      if (!currentFnTy || fnTy == currentFnTy)
        return;
      getOp.getResult().setType(cir::PointerType::get(currentFnTy));
    });

    // Phase 3: Rewrite call sites to match rewritten callees.
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

std::unique_ptr<Pass>
mlir::createCallConvLoweringPass(bool recordCoercionEnabled) {
  return std::make_unique<CallConvLoweringPass>(recordCoercionEnabled);
}

std::unique_ptr<Pass>
mlir::createCallConvLoweringPass(bool recordCoercionEnabled,
                                 bool passByValueIsNoAlias) {
  return std::make_unique<CallConvLoweringPass>(recordCoercionEnabled,
                                                passByValueIsNoAlias);
}

std::unique_ptr<Pass>
mlir::createCallConvLoweringPass(bool recordCoercionEnabled,
                                 bool passByValueIsNoAlias,
                                 unsigned x86AvxAbiLevel) {
  return std::make_unique<CallConvLoweringPass>(
      recordCoercionEnabled, passByValueIsNoAlias, x86AvxAbiLevel);
}
