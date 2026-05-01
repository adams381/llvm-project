//===- CIRABIRewriteContext.h - CIR-specific ABI rewriting ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines CIRABIRewriteContext, the CIR dialect's implementation of
// the shared ABIRewriteContext interface.  It rewrites CIR function definitions
// and call sites to match ABI-lowered signatures.
//
//===----------------------------------------------------------------------===//

#ifndef CLANG_LIB_CIR_DIALECT_TRANSFORMS_TARGETLOWERING_CIRABIREWRITECONTEXT_H
#define CLANG_LIB_CIR_DIALECT_TRANSFORMS_TARGETLOWERING_CIRABIREWRITECONTEXT_H

#include "mlir/ABI/ABIRewriteContext.h"
#include "mlir/IR/BuiltinOps.h"
#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Dialect/IR/CIRTypes.h"

namespace cir {

/// ABI metadata for a record type, queried from the module-level
/// cir.record_layouts attribute.  For anonymous or missing records,
/// safe defaults are used (cannot pass in regs, not empty, not
/// trivially destructible).
struct RecordABIInfo {
  bool canPassInRegs = false;
  bool hasTrivialDtor = true;
  bool isEmpty = false;
  uint64_t dataSizeInBits = 0;
  uint64_t recordAlignInBytes = 0;
};

/// Query ABI metadata for a RecordType from the module's
/// cir.record_layouts attribute.  Returns safe defaults for
/// anonymous records or records without a layout entry.
inline RecordABIInfo getRecordABIInfo(mlir::ModuleOp module,
                                      cir::RecordType recTy) {
  RecordABIInfo info;
  auto name = recTy.getName();
  if (!name)
    return info;
  auto dict = module->getAttrOfType<mlir::DictionaryAttr>(
      CIRDialect::getRecordLayoutsAttrName());
  if (!dict)
    return info;
  auto layout = dict.getAs<RecordLayoutAttr>(name);
  if (!layout)
    return info;
  info.canPassInRegs =
      layout.getArgPassingKind() == ArgPassingKind::CanPassInRegs;
  info.hasTrivialDtor = layout.getHasTrivialDtor();
  info.isEmpty = layout.getIsEmpty();
  info.dataSizeInBits = layout.getDataSize();
  info.recordAlignInBytes = layout.getRecordAlign();
  return info;
}

/// CIR-specific implementation of the ABIRewriteContext interface.
///
/// This class knows how to rewrite CIR FuncOps and CallOps to match
/// ABI-lowered signatures, using CIR operations for coercion (alloca,
/// load, store, cast, etc.).
class CIRABIRewriteContext : public mlir::abi::ABIRewriteContext {
  mlir::ModuleOp module;
  bool passByValueIsNoAlias = false;

public:
  explicit CIRABIRewriteContext(mlir::ModuleOp module,
                                bool passByValueIsNoAlias = false)
      : module(module), passByValueIsNoAlias(passByValueIsNoAlias) {}

  mlir::LogicalResult
  rewriteFunctionDefinition(mlir::FunctionOpInterface funcOp,
                            const mlir::abi::FunctionClassification &fc,
                            mlir::OpBuilder &rewriter) override;

  mlir::LogicalResult
  rewriteCallSite(mlir::Operation *callOp,
                  const mlir::abi::FunctionClassification &fc,
                  mlir::OpBuilder &rewriter) override;

  mlir::StringRef getDialectNamespace() const override { return "cir"; }
};

} // namespace cir

#endif // CLANG_LIB_CIR_DIALECT_TRANSFORMS_TARGETLOWERING_CIRABIREWRITECONTEXT_H
