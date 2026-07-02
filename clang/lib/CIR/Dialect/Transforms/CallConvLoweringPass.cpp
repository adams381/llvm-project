//===- CallConvLoweringPass.cpp - Lower CIR to ABI calling convention ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass walks every cir.func and cir.call in the module, computes a
// FunctionClassification for it (via either an ABI target or a pre-built
// classification injected as a function attribute), and dispatches to
// CIRABIRewriteContext to perform the actual IR rewriting.
//
// Two driver modes (mutually exclusive):
//
//   target=test
//     Use the MLIR test ABI target (mlir/lib/ABI/Targets/Test/) to classify
//     each function.  Predictable rules that approximate x86_64 SysV.  Real
//     targets (x86_64, AArch64) will be added once the LLVM ABI library
//     ships them.
//
//   classification-attr=<name>
//     Read a DictionaryAttr named <name> from each cir.func and parse it via
//     mlir::abi::test::parseClassificationAttr.  Used by tests to inject any
//     classification (including shapes the test target itself does not
//     produce) without depending on a real ABI target.
//
// The pass requires a `dlti.dl_spec` attribute on the module so the
// classifier can query type sizes and alignments.
//
//===----------------------------------------------------------------------===//

#include "PassDetail.h"
#include "TargetLowering/CIRABIRewriteContext.h"

#include "mlir/ABI/ABIRewriteContext.h"
#include "mlir/ABI/ABITypeMapper.h"
#include "mlir/ABI/Targets/Test/TestTarget.h"
#include "mlir/Dialect/DLTI/DLTI.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/DataLayoutInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Dialect/Passes.h"
#include "llvm/ABI/FunctionInfo.h"
#include "llvm/ABI/TargetInfo.h"
#include "llvm/ABI/Types.h"
#include "llvm/IR/CallingConv.h"

using namespace mlir;
using namespace mlir::abi;
using namespace cir;

namespace mlir {
#define GEN_PASS_DEF_CALLCONVLOWERING
#include "clang/CIR/Dialect/Passes.h.inc"
} // namespace mlir

namespace {

//===----------------------------------------------------------------------===//
// x86_64 System V classifier bridge
//
// The LLVM ABI Lowering Library (llvm::abi) computes the SysV x86_64
// classification for a signature.  These helpers map CIR types to
// llvm::abi::Type, run the classifier, and convert the result back into the
// dialect-agnostic mlir::abi::FunctionClassification that CIRABIRewriteContext
// consumes.
//===----------------------------------------------------------------------===//

/// ABI metadata for a named record, read from the module's
/// `cir.record_layouts` dictionary.  Anonymous records (synthesized by
/// CXXABILowering) have no entry and default to can-pass-in-registers; named
/// records with no entry default to cannot-pass (safe: forces Indirect).
struct RecordABIInfo {
  bool canPassInRegs = true;
  uint64_t recordAlignInBytes = 0;
};

static RecordABIInfo getRecordABIInfo(ModuleOp module, cir::RecordType recTy) {
  RecordABIInfo info;
  mlir::StringAttr name = recTy.getName();
  if (!name)
    return info;
  auto dict = module->getAttrOfType<DictionaryAttr>(
      cir::CIRDialect::getRecordLayoutsAttrName());
  auto layout =
      dict ? dict.getAs<cir::RecordLayoutAttr>(name) : cir::RecordLayoutAttr();
  if (!layout) {
    info.canPassInRegs = false;
    return info;
  }
  info.canPassInRegs =
      layout.getArgPassingKind() == cir::ArgPassingKind::CanPassInRegs;
  info.recordAlignInBytes = layout.getRecordAlign();
  return info;
}

/// Whether a record carries no data for ABI purposes: it has no members, or
/// all of its members are themselves empty records.  Any scalar or array
/// member counts as data.  A padding byte cannot be recognized by shape alone
/// -- CIRGen materializes an empty class/base as a `[1 x i8]` member, but a
/// real `char c[1]` field has the same shape -- so this deliberately does not
/// treat `[1 x i8]` as padding: misclassifying real data as empty silently
/// drops the argument.
static bool recordIsEmptyForABI(cir::RecordType recTy) {
  for (mlir::Type m : recTy.getMembers()) {
    auto rec = dyn_cast<cir::RecordType>(m);
    if (rec && recordIsEmptyForABI(rec))
      continue;
    return false;
  }
  return true;
}

/// llvm::Align requires a power of two; DataLayout can report non-power-of-two
/// alignments for unusual types (e.g. `_BitInt(31)`).
static llvm::Align safeAlign(uint64_t a) {
  return llvm::Align(llvm::PowerOf2Ceil(std::max<uint64_t>(a, 1)));
}

/// Convert an llvm::abi::Type coercion type back to a CIR type.
static mlir::Type abiTypeToCIR(const llvm::abi::Type *ty, MLIRContext *ctx) {
  if (!ty)
    return nullptr;
  if (ty->isVoid())
    return cir::VoidType::get(ctx);
  if (auto *intTy = llvm::dyn_cast<llvm::abi::IntegerType>(ty))
    return cir::IntType::get(ctx, intTy->getSizeInBits().getFixedValue(),
                             intTy->isSigned());
  if (auto *fltTy = llvm::dyn_cast<llvm::abi::FloatType>(ty)) {
    const llvm::fltSemantics *sem = fltTy->getSemantics();
    if (sem == &llvm::APFloat::IEEEhalf())
      return cir::FP16Type::get(ctx);
    if (sem == &llvm::APFloat::BFloat())
      return cir::BF16Type::get(ctx);
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
    return cir::VectorType::get(elemCIR,
                                vecTy->getNumElements().getFixedValue());
  }
  if (llvm::isa<llvm::abi::PointerType>(ty))
    return cir::PointerType::get(cir::VoidType::get(ctx));
  if (auto *recTy = llvm::dyn_cast<llvm::abi::RecordType>(ty)) {
    SmallVector<mlir::Type> fieldTypes;
    for (const auto &field : recTy->getFields()) {
      mlir::Type fieldCIR = abiTypeToCIR(field.FieldType, ctx);
      if (!fieldCIR)
        return nullptr;
      fieldTypes.push_back(fieldCIR);
    }
    return cir::StructType::get(ctx, fieldTypes, /*packed=*/false,
                                /*padded=*/false, /*is_class=*/false);
  }
  uint64_t bits = ty->getSizeInBits().getFixedValue();
  if (bits > 0)
    return cir::IntType::get(ctx, bits, /*isSigned=*/false);
  return nullptr;
}

/// Map a CIR type to an llvm::abi::Type with CIR-aware knowledge of
/// signedness, field layout, and pointer semantics.
static const llvm::abi::Type *
mapCIRType(mlir::Type type, mlir::abi::ABITypeMapper &typeMapper,
           const DataLayout &dl, bool recordCoercionEnabled, ModuleOp module) {
  llvm::abi::TypeBuilder &tb = typeMapper.getTypeBuilder();

  if (auto intTy = dyn_cast<cir::IntType>(type))
    return tb.getIntegerType(intTy.getWidth(),
                             safeAlign(dl.getTypeABIAlignment(type)),
                             intTy.isSigned());
  if (isa<cir::PointerType>(type))
    return tb.getPointerType(dl.getTypeSizeInBits(type),
                             safeAlign(dl.getTypeABIAlignment(type)),
                             /*AddressSpace=*/0);
  if (isa<cir::BoolType>(type))
    return tb.getIntegerType(dl.getTypeSizeInBits(type),
                             safeAlign(dl.getTypeABIAlignment(type)),
                             /*Signed=*/false);
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
  if (isa<cir::BF16Type>(type))
    return tb.getFloatType(llvm::APFloat::BFloat(),
                           safeAlign(dl.getTypeABIAlignment(type)));
  if (isa<cir::FP80Type>(type))
    return tb.getFloatType(llvm::APFloat::x87DoubleExtended(),
                           safeAlign(dl.getTypeABIAlignment(type)));
  if (isa<cir::FP128Type>(type))
    return tb.getFloatType(llvm::APFloat::IEEEquad(),
                           safeAlign(dl.getTypeABIAlignment(type)));
  if (auto ldTy = dyn_cast<cir::LongDoubleType>(type))
    return mapCIRType(ldTy.getUnderlying(), typeMapper, dl,
                      recordCoercionEnabled, module);
  if (auto arrTy = dyn_cast<cir::ArrayType>(type)) {
    const llvm::abi::Type *elemAbi = mapCIRType(
        arrTy.getElementType(), typeMapper, dl, recordCoercionEnabled, module);
    return tb.getArrayType(elemAbi, arrTy.getSize(),
                           dl.getTypeSizeInBits(type).getFixedValue());
  }
  if (auto complexTy = dyn_cast<cir::ComplexType>(type)) {
    const llvm::abi::Type *elemAbi =
        mapCIRType(complexTy.getElementType(), typeMapper, dl,
                   recordCoercionEnabled, module);
    return tb.getComplexType(elemAbi, safeAlign(dl.getTypeABIAlignment(type)));
  }
  if (auto vecTy = dyn_cast<cir::VectorType>(type)) {
    const llvm::abi::Type *elemAbi = mapCIRType(
        vecTy.getElementType(), typeMapper, dl, recordCoercionEnabled, module);
    llvm::ElementCount ec = llvm::ElementCount::getFixed(vecTy.getSize());
    uint64_t sizeBits = dl.getTypeSizeInBits(type).getFixedValue();
    return tb.getVectorType(elemAbi, ec, safeAlign(sizeBits / 8));
  }
  if (isa<cir::DataMemberType>(type))
    return tb.getIntegerType(64, llvm::Align(8), /*Signed=*/false);
  if (isa<cir::MethodType>(type)) {
    auto i64 = tb.getIntegerType(64, llvm::Align(8), /*Signed=*/false);
    SmallVector<llvm::abi::FieldInfo> fields;
    fields.push_back(llvm::abi::FieldInfo(i64, 0));
    fields.push_back(llvm::abi::FieldInfo(i64, 64));
    return tb.getRecordType(fields, llvm::TypeSize::getFixed(128),
                            llvm::Align(8));
  }
  if (auto recTy = dyn_cast<cir::RecordType>(type)) {
    if (!recTy.isComplete())
      return tb.getIntegerType(64, llvm::Align(8), /*Signed=*/false);
    SmallVector<llvm::abi::FieldInfo> fields;
    bool isUnion = recTy.isUnion();
    bool isPacked = recTy.getPacked();
    auto members = recTy.getMembers();
    // Drop a trailing alignment-only i8 padding array so it does not inflate
    // eightbyte classification: OGCG classifies over data fields only.
    bool excludeTrailingPad = false;
    if (recTy.getPadded() && !isUnion && members.size() > 1 &&
        isa<cir::ArrayType>(members.back())) {
      uint64_t withoutPad = 0;
      for (unsigned i = 0; i < members.size() - 1; ++i)
        withoutPad += dl.getTypeSizeInBits(members[i]).getFixedValue();
      uint64_t withPad =
          withoutPad + dl.getTypeSizeInBits(members.back()).getFixedValue();
      excludeTrailingPad = (withoutPad + 63) / 64 < (withPad + 63) / 64;
    }
    auto endIt = excludeTrailingPad ? members.end() - 1 : members.end();
    uint64_t offsetBits = 0;
    for (auto it = members.begin(); it != endIt; ++it) {
      mlir::Type fieldTy = *it;
      // Skip empty base / empty record members.
      if (auto memberRec = dyn_cast<cir::RecordType>(fieldTy))
        if (recordIsEmptyForABI(memberRec) &&
            memberRec.getMembers().size() <= 1) {
          offsetBits += dl.getTypeSizeInBits(fieldTy).getFixedValue();
          continue;
        }
      // Skip a leading i8 padding array on a padded record.
      if (auto arrTy = dyn_cast<cir::ArrayType>(fieldTy))
        if (auto elt = dyn_cast<cir::IntType>(arrTy.getElementType());
            elt && elt.getWidth() == 8 && recTy.getPadded() &&
            it == members.begin()) {
          offsetBits += dl.getTypeSizeInBits(fieldTy).getFixedValue();
          continue;
        }
      const llvm::abi::Type *mappedField =
          mapCIRType(fieldTy, typeMapper, dl, recordCoercionEnabled, module);
      uint64_t fieldSize = dl.getTypeSizeInBits(fieldTy).getFixedValue();
      if (!isUnion && !isPacked)
        offsetBits =
            llvm::alignTo(offsetBits, dl.getTypeABIAlignment(fieldTy) * 8);
      fields.push_back(
          llvm::abi::FieldInfo(mappedField, isUnion ? 0 : offsetBits));
      if (!isUnion)
        offsetBits += fieldSize;
    }
    RecordABIInfo recABI = getRecordABIInfo(module, recTy);
    uint64_t sizeBits = dl.getTypeSizeInBits(type).getFixedValue();
    if (excludeTrailingPad)
      sizeBits -= dl.getTypeSizeInBits(members.back()).getFixedValue();
    llvm::TypeSize size = llvm::TypeSize::getFixed(sizeBits);
    uint64_t rawAlign = recABI.recordAlignInBytes
                            ? recABI.recordAlignInBytes
                            : dl.getTypeABIAlignment(type);
    bool isAnonymous = !recTy.getName();
    bool canPass = recABI.canPassInRegs || recordCoercionEnabled || isAnonymous;
    llvm::abi::RecordFlags flags = llvm::abi::RecordFlags::None;
    if (canPass)
      flags = flags | llvm::abi::RecordFlags::CanPassInRegisters;
    llvm::abi::StructPacking packing = isPacked
                                           ? llvm::abi::StructPacking::Packed
                                           : llvm::abi::StructPacking::Default;
    if (isUnion)
      return tb.getUnionType(fields, size, safeAlign(rawAlign), packing, flags);
    return tb.getRecordType(fields, size, safeAlign(rawAlign), packing,
                            /*BaseClasses=*/{}, /*VirtualBaseClasses=*/{},
                            flags);
  }
  return typeMapper.map(type);
}

/// Convert an llvm::abi::ArgInfo classification into the dialect-agnostic
/// ArgClassification consumed by CIRABIRewriteContext.
static ArgClassification convertABIArgInfo(const llvm::abi::ArgInfo &info,
                                           MLIRContext *ctx, mlir::Type origTy,
                                           bool recordCoercionEnabled,
                                           ModuleOp module) {
  // Empty trivially-copyable records are neither passed nor returned.
  if (origTy)
    if (auto recTy = dyn_cast<cir::RecordType>(origTy))
      if (recordIsEmptyForABI(recTy) &&
          getRecordABIInfo(module, recTy).canPassInRegs)
        return ArgClassification::getIgnore();

  // The ABI library keeps `_BitInt(N)` as an opaque iN; classic Clang splits a
  // non-power-of-2 `_BitInt` with 64 < N < 128 into a `{i64, i64}` coerce.
  if (origTy)
    if (auto intTy = dyn_cast<cir::IntType>(origTy))
      if (intTy.getIsBitInt() && intTy.getWidth() > 64 &&
          intTy.getWidth() < 128 && info.isDirect()) {
        auto u64 = cir::IntType::get(ctx, 64, /*isSigned=*/false);
        auto coerced =
            cir::StructType::get(ctx, {u64, u64}, /*packed=*/false,
                                 /*padded=*/false, /*is_class=*/false);
        return ArgClassification::getDirect(coerced);
      }

  if (info.isDirect()) {
    mlir::Type coerced = abiTypeToCIR(info.getCoerceToType(), ctx);
    if (coerced && origTy) {
      if (coerced == origTy)
        coerced = nullptr;
      else if (!isa<cir::RecordType, cir::ComplexType, cir::VectorType,
                    cir::MethodType, cir::DataMemberType>(origTy)) {
        if (!isa<cir::RecordType>(coerced))
          coerced = nullptr;
      } else if (!recordCoercionEnabled && isa<cir::RecordType>(origTy)) {
        auto recTy = cast<cir::RecordType>(origTy);
        bool canPass =
            !recTy.getName() || getRecordABIInfo(module, recTy).canPassInRegs;
        if (!canPass)
          coerced = nullptr;
      } else if (auto coercedInt = dyn_cast<cir::IntType>(coerced))
        if (auto origInt = dyn_cast<cir::IntType>(origTy))
          if (coercedInt.getWidth() == origInt.getWidth())
            coerced = nullptr;
      if (coerced)
        if (auto recTy = dyn_cast<cir::RecordType>(origTy))
          if (recTy.getPadded() && recTy.getMembers().size() == 1 &&
              recTy.getMembers()[0] == coerced)
            coerced = nullptr;
    }
    return ArgClassification::getDirect(coerced);
  }
  if (info.isExtend()) {
    mlir::Type coerced = abiTypeToCIR(info.getCoerceToType(), ctx);
    if (origTy && isa<cir::BoolType>(origTy))
      return ArgClassification::getExtend(nullptr, info.isSignExt());
    if (origTy && !isa<cir::IntType>(origTy))
      return ArgClassification::getDirect(nullptr);
    return ArgClassification::getExtend(coerced, info.isSignExt());
  }
  if (info.isIndirect()) {
    if (!recordCoercionEnabled)
      if (!origTy || !isa<cir::RecordType, cir::VectorType, cir::ComplexType,
                          cir::IntType>(origTy))
        return ArgClassification::getDirect(nullptr);
    return ArgClassification::getIndirect(info.getIndirectAlign(),
                                          info.getIndirectByVal());
  }
  // Ignore.
  if (!recordCoercionEnabled && origTy && isa<cir::RecordType>(origTy))
    return ArgClassification::getDirect(nullptr);
  return ArgClassification::getIgnore();
}

/// `_BitInt(N > 128)` is passed/returned indirectly on x86_64 (does not fit in
/// the two integer-class registers).
static bool isBitIntIndirect(mlir::Type ty) {
  auto intTy = dyn_cast_or_null<cir::IntType>(ty);
  return intTy && intTy.getIsBitInt() && intTy.getWidth() > 128;
}

/// Widen an undersized union coercion to the union's true sizeof, matching
/// classic Clang (the ABI library picks the best-aligned member, which can be
/// narrower than the union's data).
static void fixupUnionCoercion(ArgClassification &ac, mlir::Type origTy,
                               const DataLayout &dl, MLIRContext *ctx) {
  if (ac.kind != ArgKind::Direct || !ac.coercedType || !origTy)
    return;
  auto recTy = dyn_cast<cir::RecordType>(origTy);
  if (!recTy || !recTy.isUnion())
    return;
  auto coercedInt = dyn_cast<cir::IntType>(ac.coercedType);
  if (!coercedInt)
    return;
  auto members = recTy.getMembers();
  auto endIt =
      recTy.getPadded() && !members.empty() ? members.end() - 1 : members.end();
  uint64_t maxMemberBits = 0;
  uint64_t maxAlignBytes = 1;
  for (auto it = members.begin(); it != endIt; ++it) {
    maxMemberBits =
        std::max(maxMemberBits, dl.getTypeSizeInBits(*it).getFixedValue());
    if (!recTy.getPacked())
      maxAlignBytes = std::max(maxAlignBytes, dl.getTypeABIAlignment(*it));
  }
  uint64_t unionBits = llvm::alignTo(maxMemberBits, maxAlignBytes * 8);
  if (coercedInt.getWidth() < unionBits && unionBits <= 64)
    ac.coercedType = cir::IntType::get(ctx, unionBits, coercedInt.isSigned());
}

/// Classify an x86_64 SysV signature (return type + argument types) using the
/// LLVM ABI library.  Shared by the cir.func path and the indirect-call path
/// (which classifies from the callee function-pointer's pointee FuncType).
static FunctionClassification
classifyX86_64Sig(mlir::Type retCIR, mlir::TypeRange inputs, MLIRContext *ctx,
                  const DataLayout &dl, mlir::abi::ABITypeMapper &typeMapper,
                  const llvm::abi::TargetInfo &targetInfo, ModuleOp module) {
  FunctionClassification fc;
  const bool recordCoercionEnabled = false;

  bool voidRet = !retCIR || isa<cir::VoidType>(retCIR);
  const llvm::abi::Type *retAbi =
      voidRet
          ? typeMapper.getTypeBuilder().getVoidType()
          : mapCIRType(retCIR, typeMapper, dl, recordCoercionEnabled, module);

  SmallVector<const llvm::abi::Type *> argAbi;
  for (mlir::Type a : inputs)
    argAbi.push_back(
        mapCIRType(a, typeMapper, dl, recordCoercionEnabled, module));

  std::unique_ptr<llvm::abi::FunctionInfo> fi =
      llvm::abi::FunctionInfo::create(llvm::CallingConv::C, retAbi, argAbi);
  targetInfo.computeInfo(*fi);

  mlir::Type origRet = voidRet ? mlir::Type() : retCIR;
  fc.returnInfo = convertABIArgInfo(fi->getReturnInfo(), ctx, origRet,
                                    recordCoercionEnabled, module);
  if (!voidRet)
    fixupUnionCoercion(fc.returnInfo, origRet, dl, ctx);

  for (unsigned i = 0, e = fi->arg_size(); i < e; ++i) {
    mlir::Type origArg = i < inputs.size() ? inputs[i] : mlir::Type();
    ArgClassification ac = convertABIArgInfo(
        fi->getArgInfo(i).Info, ctx, origArg, recordCoercionEnabled, module);
    if (isBitIntIndirect(origArg))
      ac = ArgClassification::getIndirect(llvm::Align(8), /*byVal=*/true);
    fixupUnionCoercion(ac, origArg, dl, ctx);
    fc.argInfos.push_back(ac);
  }
  return fc;
}

/// Classify a cir.func for x86_64 SysV using the LLVM ABI library.
static FunctionClassification
classifyX86_64(cir::FuncOp func, const DataLayout &dl,
               mlir::abi::ABITypeMapper &typeMapper,
               const llvm::abi::TargetInfo &targetInfo, ModuleOp module) {
  cir::FuncType fnTy = func.getFunctionType();
  return classifyX86_64Sig(fnTy.getReturnType(), fnTy.getInputs(),
                           func->getContext(), dl, typeMapper, targetInfo,
                           module);
}

struct CallConvLoweringPass
    : public impl::CallConvLoweringBase<CallConvLoweringPass> {
  using CallConvLoweringBase::CallConvLoweringBase;
  void runOnOperation() override;
};

/// Classify \p func using whichever driver mode is configured.  Returns
/// std::nullopt and emits an error on the function if classification fails
/// (e.g. injection-driver mode but the function is missing the attribute,
/// or the attribute is malformed).
std::optional<FunctionClassification>
classifyFunction(cir::FuncOp func, const DataLayout &dl, StringRef target,
                 StringRef classificationAttrName) {
  ArrayRef<Type> argTypes = func.getFunctionType().getInputs();
  Type returnType = func.getFunctionType().getReturnType();

  if (!classificationAttrName.empty()) {
    auto attr = func->getAttrOfType<DictionaryAttr>(classificationAttrName);
    if (!attr) {
      func.emitOpError()
          << "missing classification attribute '" << classificationAttrName
          << "' (CallConvLowering driver mode 'classification-attr')";
      return std::nullopt;
    }
    return mlir::abi::test::parseClassificationAttr(
        attr, [&]() { return func.emitOpError(); });
  }

  if (target == "test")
    return mlir::abi::test::classify(argTypes, returnType, dl);

  // Note: the "x86_64" target is handled directly in runOnOperation (it needs
  // a shared ABITypeMapper and TargetInfo), so it never reaches here.
  func.emitOpError() << "unknown target '" << target
                     << "' (supported: test, x86_64)";
  return std::nullopt;
}

/// Find the cir.func declaration matching a direct cir.call / cir.try_call
/// callee, if any.  Returns nullptr if the callee is indirect or the symbol
/// cannot be resolved.  Takes a SymbolTable instead of a ModuleOp so the
/// symbol lookup is amortized across all the call sites the driver walks
/// (ModuleOp::lookupSymbol is linear per call).
cir::FuncOp lookupCallee(Operation *callOp, SymbolTable &symbolTable) {
  FlatSymbolRefAttr callee;
  if (auto call = dyn_cast<cir::CallOp>(callOp))
    callee = call.getCalleeAttr();
  else if (auto tryCall = dyn_cast<cir::TryCallOp>(callOp))
    callee = tryCall.getCalleeAttr();
  else
    return nullptr;
  if (!callee)
    return nullptr;
  return symbolTable.lookup<cir::FuncOp>(callee.getValue());
}

void CallConvLoweringPass::runOnOperation() {
  ModuleOp moduleOp = getOperation();
  MLIRContext *ctx = &getContext();

  if (target.empty() == classificationAttr.empty()) {
    moduleOp.emitOpError() << "CallConvLowering requires exactly one of "
                              "'target' or 'classification-attr' pass options";
    signalPassFailure();
    return;
  }

  if (!moduleOp->hasAttr(DLTIDialect::kDataLayoutAttrName)) {
    moduleOp.emitOpError()
        << "CallConvLowering requires a DataLayout (dlti.dl_spec attribute "
           "on the module)";
    signalPassFailure();
    return;
  }

  DataLayout dl(moduleOp);
  CIRABIRewriteContext rewriteCtx(moduleOp, dl);
  SymbolTable symbolTable(moduleOp);

  // For the x86_64 target, build the LLVM ABI library classifier once and
  // reuse it (and its type mapper) across every function.
  std::optional<mlir::abi::ABITypeMapper> x86TypeMapper;
  std::unique_ptr<llvm::abi::TargetInfo> x86Target;
  if (target == "x86_64") {
    x86TypeMapper.emplace(dl);
    auto avx =
        static_cast<llvm::abi::X86AVXABILevel>(x86AvxAbiLevel.getValue());
    x86Target = llvm::abi::createX86_64TargetInfo(
        x86TypeMapper->getTypeBuilder(), avx, /*Has64BitPointers=*/true,
        llvm::abi::ABICompatInfo());
  }

  // Classify every cir.func up front.  No IR mutation happens here, so
  // later walks can consult any function's classification regardless of
  // visitation order.
  llvm::MapVector<cir::FuncOp, FunctionClassification> classifications;
  bool anyFailed = false;
  moduleOp.walk([&](cir::FuncOp f) {
    // Coroutine functions have a synthesized ABI that the classifier must not
    // rewrite.
    if (x86Target && f->hasAttr("coroutine"))
      return;
    std::optional<FunctionClassification> fc;
    if (x86Target)
      fc = classifyX86_64(f, dl, *x86TypeMapper, *x86Target, moduleOp);
    else
      fc = classifyFunction(f, dl, target, classificationAttr);
    if (!fc) {
      anyFailed = true;
      return;
    }
    classifications.insert({f, std::move(*fc)});
  });
  if (anyFailed) {
    signalPassFailure();
    return;
  }

  // Build a callee-to-callers index.  One module walk collects every direct
  // cir.call / cir.try_call to each cir.func; the loop below rewrites a
  // function and all of its call sites together.  Indirect or unresolved
  // callees are skipped here; rewriteCallSite errors on those at the end.
  llvm::DenseMap<cir::FuncOp, SmallVector<Operation *>> callers;
  moduleOp.walk([&](Operation *op) {
    if (!isa<cir::CallOp, cir::TryCallOp>(op))
      return;
    if (cir::FuncOp callee = lookupCallee(op, symbolTable))
      callers[callee].push_back(op);
  });

  // Rewrite each function together with every direct call to it.  By the
  // time we move on to function F+1, F's signature and every direct call to
  // F have already been brought into alignment, and F+1..FN are still in
  // their original (mutually consistent) form, so the IR is verifier-clean
  // at every outer-iteration boundary.
  //
  // There is still a brief inner window where F's signature has been
  // rewritten but its callers have not yet caught up -- we have no way to
  // mutate both sides of a call atomically.  No verifier runs inside the
  // pass, and at pass exit the module is verifier-clean.  Fusing the inner
  // loop here keeps the invalid window per-function rather than module-wide.
  OpBuilder builder(ctx);
  for (auto &kv : classifications) {
    cir::FuncOp func = kv.first;
    const FunctionClassification &fc = kv.second;
    if (failed(rewriteCtx.rewriteFunctionDefinition(func, fc, builder))) {
      signalPassFailure();
      return;
    }
    for (Operation *callOp : callers.lookup(func)) {
      // A variadic call has more operands than the callee has fixed
      // parameters.  Re-classify from the call's actual operand types so each
      // variadic argument (structs included) is coerced or passed byval per
      // the ABI, then rewrite with that complete classification.
      const FunctionClassification *callFc = &fc;
      FunctionClassification variadicFc;
      if (auto c = dyn_cast<cir::CallOp>(callOp);
          c && c.getArgOperands().size() > fc.argInfos.size()) {
        SmallVector<mlir::Type> argTypes =
            llvm::to_vector(c.getArgOperands().getTypes());
        mlir::Type retTy = c.getNumResults() ? c.getResult().getType()
                                             : cir::VoidType::get(ctx);
        if (x86Target)
          variadicFc = classifyX86_64Sig(retTy, argTypes, ctx, dl,
                                         *x86TypeMapper, *x86Target, moduleOp);
        else if (target == "test")
          variadicFc = mlir::abi::test::classify(argTypes, retTy, dl);
        else
          variadicFc = fc;
        callFc = &variadicFc;
      }
      if (failed(rewriteCtx.rewriteCallSite(callOp, *callFc, builder))) {
        signalPassFailure();
        return;
      }
    }
  }

  // Rewrite indirect call sites.  The callee is opaque, so classify from the
  // function-pointer's pointee FuncType and let rewriteCallSite retype the
  // callee pointer to match the coerced signature.  Collect the calls first
  // because rewriteCallSite erases and replaces each one, which would
  // invalidate a live walk.
  SmallVector<cir::CallOp> indirectCalls;
  moduleOp.walk([&](cir::CallOp c) {
    if (c.isIndirect())
      indirectCalls.push_back(c);
  });
  for (cir::CallOp c : indirectCalls) {
    auto ptrTy = dyn_cast<cir::PointerType>(c.getIndirectCall().getType());
    if (!ptrTy)
      continue;
    auto funcTy = dyn_cast<cir::FuncType>(ptrTy.getPointee());
    if (!funcTy)
      continue;
    // Classify from the pointee's fixed inputs.  A variadic indirect call is
    // left to rewriteCallSite's NYI: rebuilding the callee pointer for a
    // variadic signature would need to preserve the varargs marker, which is
    // not yet handled.
    std::optional<FunctionClassification> fc;
    if (x86Target)
      fc = classifyX86_64Sig(funcTy.getReturnType(), funcTy.getInputs(), ctx,
                             dl, *x86TypeMapper, *x86Target, moduleOp);
    else if (target == "test")
      fc = mlir::abi::test::classify(funcTy.getInputs(), funcTy.getReturnType(),
                                     dl);
    else
      continue;
    if (!fc) {
      signalPassFailure();
      return;
    }
    if (failed(rewriteCtx.rewriteCallSite(c, *fc, builder))) {
      signalPassFailure();
      return;
    }
  }
}

} // namespace

std::unique_ptr<Pass> mlir::createCallConvLoweringPass() {
  return std::make_unique<CallConvLoweringPass>();
}

std::unique_ptr<Pass>
mlir::createCallConvLoweringPass(llvm::StringRef target,
                                 unsigned x86AvxAbiLevel) {
  CallConvLoweringOptions options;
  options.target = target.str();
  options.x86AvxAbiLevel = x86AvxAbiLevel;
  return std::make_unique<CallConvLoweringPass>(options);
}
