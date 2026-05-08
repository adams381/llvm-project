//==- CallConvCascade.h - Type cascade for calling-convention rewrites ----==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Function-pointer type cascade for the calling-convention lowering pass.
//
// When CallConvLowering rewrites a function's signature (e.g. flattens a
// struct argument into (i64, i64) per the System V x86_64 ABI), the new
// signature must also propagate to every CIR composite type that embeds the
// rewritten cir::FuncType: function-pointer fields of records, arrays of
// function pointers, function pointers in global initializer attributes, and
// function pointers loaded out of any of those.  This cascade preserves the
// CIR verifier invariant that every reference to a rewritten function -- direct
// call, indirect call through a function pointer, cir.get_global, record field,
// cast operand, and so on -- agrees on the same signature.
//
// See clang/docs/CIR/ABILowering.rst, "Function-Pointer Type Cascade" for the
// design.  The implementation is modeled on
// clang/lib/CIR/Dialect/Transforms/CXXABILowering.cpp.
//
//===----------------------------------------------------------------------===//

#ifndef CLANG_LIB_CIR_DIALECT_TRANSFORMS_CALLCONVCASCADE_H
#define CLANG_LIB_CIR_DIALECT_TRANSFORMS_CALLCONVCASCADE_H

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Support/LogicalResult.h"
#include "clang/CIR/Dialect/IR/CIRTypes.h"
#include "llvm/ADT/DenseMap.h"

namespace cir {
namespace callconv {

/// Map from original cir::FuncType (as it appears in the CIR module before the
/// pass runs) to the ABI-rewritten cir::FuncType (the lowered shape produced
/// by classification).  Populated by CallConvLowering's classification phase.
using FuncTypeMap = llvm::DenseMap<cir::FuncType, cir::FuncType>;

/// Apply the function-pointer type cascade to `module`.
///
/// Walks every CIR composite type in `module` that embeds an entry from
/// `rewrites` and propagates the rewrite through:
///   - function-pointer types nested in records, arrays, and other composites
///     (via mlir::TypeConverter recursion);
///   - cir::ConstRecordAttr / cir::GlobalViewAttr / cir::VTableAttr /
///     cir::TypeInfoAttr / cir::DynamicCastInfoAttr / mlir::TypeAttr
///     attribute contents that embed types;
///   - block argument and SSA value types reachable from CIR operations.
///
/// Returns success when the conversion fully legalizes the module.  Returns
/// failure if applyPartialConversion fails.
///
/// When `rewrites` is empty (the common case in Phase A, before uniform
/// classification populates the map), returns success without walking the
/// module.
mlir::LogicalResult applyCallConvCascade(mlir::ModuleOp module,
                                         const FuncTypeMap &rewrites);

} // namespace callconv
} // namespace cir

#endif // CLANG_LIB_CIR_DIALECT_TRANSFORMS_CALLCONVCASCADE_H
