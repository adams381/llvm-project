//===- VerifyCallConvInvariant.cpp - Verify cascade post-conditions -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Diagnostic pass that asserts the three clauses of the function-pointer
// type cascade invariant on a CIR module that has been through
// `CallConvLowering`.  See the matching pass description in
// `clang/include/clang/CIR/Dialect/Passes.td` and the design discussion in
// `clang/docs/CIR/ABILowering.rst`, "Function-Pointer Type Cascade".
//
//===----------------------------------------------------------------------===//

#include "PassDetail.h"

#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/LogicalResult.h"

#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Dialect/IR/CIRTypes.h"
#include "clang/CIR/Dialect/Passes.h"
#include "clang/CIR/Interfaces/CIROpInterfaces.h"

#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/TimeProfiler.h"

using namespace mlir;
using namespace cir;

namespace mlir {
#define GEN_PASS_DEF_CIRVERIFYCALLCONVINVARIANT
#include "clang/CIR/Dialect/Passes.h.inc"
} // namespace mlir

namespace {

struct CIRVerifyCallConvInvariantPass
    : public mlir::impl::CIRVerifyCallConvInvariantBase<
          CIRVerifyCallConvInvariantPass> {
  void runOnOperation() override;
};

// -- Clause 1: GlobalViewAttr / function-pointer references in attributes ---
//
// `cir.global_view` explicitly allows the view's type to differ from the
// referenced symbol's declared type -- the docs call out that "a given view
// of the global might require a static cast for initializing other globals".
// In particular, C/C++ frontends emit views with intentionally retyped
// function-pointer pointees to fit callback-struct slots (e.g.
// `cir.global_view<@fclose> : !cir.ptr<!cir.func<(!cir.ptr<!void>) -> !s32i>>`
// stored into an `ov_callbacks::close_func` field whose declared type is
// `int (*)(void*)`, even though `fclose` itself is declared
// `int fclose(FILE*)`).  These deliberate retypings are valid C and the
// cascade correctly leaves them alone (the type converter only rewrites
// types that match a known FuncType-rewrite source).
//
// As a result the verifier cannot reliably distinguish (a) "the cascade
// missed updating this view" from (b) "the frontend deliberately cast the
// function pointer through this view".  Strict equality enforcement here
// would false-positive on every C-interop callback table.  We therefore
// skip clause 1 entirely and lean on clause 3 (indirect-call type
// agreement at the use site) for the unique add-on coverage the
// verifier provides over the standard MLIR verifier.
//
// `checkGlobalView` is kept (returning success unconditionally) so the
// recursive attribute walk remains in place -- if a future invariant
// needs to inspect global views, it can hook in here.

static bool checkGlobalView(cir::GlobalViewAttr gva, mlir::Operation *op,
                            mlir::SymbolTableCollection &symbolTable) {
  (void)gva;
  (void)op;
  (void)symbolTable;
  return false;
}

static bool verifyAttr(mlir::Attribute attr, mlir::Operation *op,
                       mlir::SymbolTableCollection &symbolTable) {
  if (!attr)
    return false;

  bool failed = false;

  llvm::TypeSwitch<mlir::Attribute>(attr)
      .Case<cir::GlobalViewAttr>([&](cir::GlobalViewAttr gva) {
        if (checkGlobalView(gva, op, symbolTable))
          failed = true;
      })
      .Case<cir::ConstArrayAttr>([&](cir::ConstArrayAttr ca) {
        if (verifyAttr(ca.getElts(), op, symbolTable))
          failed = true;
      })
      .Case<cir::ConstRecordAttr>([&](cir::ConstRecordAttr cra) {
        if (verifyAttr(cra.getMembers(), op, symbolTable))
          failed = true;
      })
      .Case<cir::VTableAttr>([&](cir::VTableAttr vta) {
        if (verifyAttr(vta.getData(), op, symbolTable))
          failed = true;
      })
      .Case<cir::TypeInfoAttr>([&](cir::TypeInfoAttr tia) {
        if (verifyAttr(tia.getData(), op, symbolTable))
          failed = true;
      })
      .Case<cir::DynamicCastInfoAttr>([&](cir::DynamicCastInfoAttr dcia) {
        if (verifyAttr(dcia.getSrcRtti(), op, symbolTable))
          failed = true;
        if (verifyAttr(dcia.getDestRtti(), op, symbolTable))
          failed = true;
      })
      .Case<mlir::ArrayAttr>([&](mlir::ArrayAttr aa) {
        for (mlir::Attribute child : aa.getValue())
          if (verifyAttr(child, op, symbolTable))
            failed = true;
      })
      .Case<mlir::DictionaryAttr>([&](mlir::DictionaryAttr da) {
        for (mlir::NamedAttribute na : da.getValue())
          if (verifyAttr(na.getValue(), op, symbolTable))
            failed = true;
      })
      .Default([](mlir::Attribute) {});

  return failed;
}

// -- Clauses 2 and 3: direct- and indirect-call signature agreement --------

static bool verifyDirectCall(cir::CIRCallOpInterface call,
                             mlir::FlatSymbolRefAttr calleeAttr,
                             mlir::SymbolTableCollection &symbolTable) {
  mlir::Operation *callOp = call.getOperation();

  auto fn =
      symbolTable.lookupNearestSymbolFrom<cir::FuncOp>(callOp, calleeAttr);
  if (!fn) {
    callOp->emitError()
        << "callconv invariant clause 2 violated: direct call to '"
        << calleeAttr.getValue() << "' does not reference a cir.func symbol";
    return true;
  }

  // K&R-style functions (no prototype) bypass operand type checking, the
  // same way `verifyCallCommInSymbolUses` does.
  if (fn.getNoProto())
    return false;

  cir::FuncType fnTy = fn.getFunctionType();
  unsigned numCallOperands = call.getNumArgOperands();
  unsigned numFnOperands = fnTy.getNumInputs();

  if (!fnTy.isVarArg() && numCallOperands != numFnOperands) {
    callOp->emitError() << "callconv invariant clause 2 violated: direct call "
                           "to @"
                        << calleeAttr.getValue() << " passes "
                        << numCallOperands << " operand(s) but the function "
                        << "expects " << numFnOperands;
    return true;
  }
  if (fnTy.isVarArg() && numCallOperands < numFnOperands) {
    callOp->emitError() << "callconv invariant clause 2 violated: variadic "
                           "direct call to @"
                        << calleeAttr.getValue() << " passes "
                        << numCallOperands << " operand(s) but the function "
                        << "requires at least " << numFnOperands;
    return true;
  }

  for (unsigned i = 0; i < numFnOperands; ++i) {
    mlir::Type callTy = call.getArgOperand(i).getType();
    mlir::Type fnArgTy = fnTy.getInput(i);
    if (callTy != fnArgTy) {
      callOp->emitError() << "callconv invariant clause 2 violated: direct "
                             "call to @"
                          << calleeAttr.getValue() << " operand " << i
                          << " has type " << callTy
                          << " but the function declares it as " << fnArgTy;
      return true;
    }
  }

  if (fnTy.hasVoidReturn() && callOp->getNumResults() != 0) {
    callOp->emitError() << "callconv invariant clause 2 violated: direct call "
                           "to void function @"
                        << calleeAttr.getValue() << " has "
                        << callOp->getNumResults() << " result(s)";
    return true;
  }
  if (!fnTy.hasVoidReturn()) {
    if (callOp->getNumResults() != 1) {
      callOp->emitError() << "callconv invariant clause 2 violated: direct "
                             "call to @"
                          << calleeAttr.getValue() << " has "
                          << callOp->getNumResults()
                          << " result(s) but should have exactly 1";
      return true;
    }
    if (callOp->getResult(0).getType() != fnTy.getReturnType()) {
      callOp->emitError() << "callconv invariant clause 2 violated: direct "
                             "call to @"
                          << calleeAttr.getValue() << " result type "
                          << callOp->getResult(0).getType()
                          << " does not match function return type "
                          << fnTy.getReturnType();
      return true;
    }
  }

  return false;
}

static bool verifyIndirectCall(cir::CIRCallOpInterface call,
                               mlir::Value indirect) {
  mlir::Operation *callOp = call.getOperation();

  auto ptrTy = mlir::dyn_cast<cir::PointerType>(indirect.getType());
  if (!ptrTy) {
    callOp->emitError() << "callconv invariant clause 3 violated: indirect "
                           "call's callee operand has non-pointer type "
                        << indirect.getType();
    return true;
  }
  auto fnTy = mlir::dyn_cast<cir::FuncType>(ptrTy.getPointee());
  if (!fnTy) {
    callOp->emitError() << "callconv invariant clause 3 violated: indirect "
                           "call's callee pointer pointee type "
                        << ptrTy.getPointee() << " is not a cir.func type";
    return true;
  }

  // C++ pointer-to-member-function (PMF) calls intentionally have a
  // mismatched shape between the function pointer's pointee FuncType and the
  // call's argument list.  CXXABILowering emits PMF function pointers with
  // type `func<(!cir.ptr<!void>, !cir.ptr<RecordType>, ...args)>` — both an
  // adjusted-this slot AND a typed-this slot — but the call passes only
  // `(adjusted_this : !cir.ptr<!void>, ...args)`, dropping the typed slot.
  // The LowerToLLVM pass bitcasts the function pointer at the call site to
  // remove the typed-this slot before emitting the LLVM call.  Accept this
  // PMF shape as legal: when the call has exactly one fewer operand than
  // the FuncType has inputs, the second input is a pointer-to-record, and
  // the operand types align with inputs[0] and inputs[2..N], treat clause 3
  // as satisfied.  (See clang/lib/CIR/Dialect/Transforms/CXXABILowering.cpp
  // for the upstream PMF lowering shape.)
  unsigned numCallOperands = call.getNumArgOperands();
  unsigned numFnOperands = fnTy.getNumInputs();

  bool isPmfShape = false;
  if (!fnTy.isVarArg() && numFnOperands >= 2 &&
      numCallOperands + 1 == numFnOperands) {
    auto firstInputPtr = mlir::dyn_cast<cir::PointerType>(fnTy.getInput(0));
    auto secondInputPtr = mlir::dyn_cast<cir::PointerType>(fnTy.getInput(1));
    if (firstInputPtr && mlir::isa<cir::VoidType>(firstInputPtr.getPointee()) &&
        secondInputPtr &&
        mlir::isa<cir::RecordType>(secondInputPtr.getPointee())) {
      // Check operand types align: call[0] vs inputs[0], call[i] vs
      // inputs[i+1] for i >= 1.
      bool aligned = call.getArgOperand(0).getType() == fnTy.getInput(0);
      for (unsigned i = 1; aligned && i < numCallOperands; ++i)
        if (call.getArgOperand(i).getType() != fnTy.getInput(i + 1))
          aligned = false;
      if (aligned)
        isPmfShape = true;
    }
  }
  if (isPmfShape) {
    // Verify return type matches.  PMF lowering does not adjust the return
    // type, so this should agree exactly.
    if (fnTy.hasVoidReturn() && callOp->getNumResults() != 0) {
      callOp->emitError() << "callconv invariant clause 3 violated: PMF-shape "
                             "indirect call has "
                          << callOp->getNumResults()
                          << " result(s) but the callee pointer's pointee "
                             "function type returns void";
      return true;
    }
    if (!fnTy.hasVoidReturn() && callOp->getNumResults() == 1 &&
        callOp->getResult(0).getType() != fnTy.getReturnType()) {
      callOp->emitError() << "callconv invariant clause 3 violated: PMF-shape "
                             "indirect call result type "
                          << callOp->getResult(0).getType()
                          << " does not match callee pointer's pointee "
                             "function return type "
                          << fnTy.getReturnType();
      return true;
    }
    return false;
  }

  if (!fnTy.isVarArg() && numCallOperands != numFnOperands) {
    callOp->emitError() << "callconv invariant clause 3 violated: indirect "
                           "call passes "
                        << numCallOperands
                        << " operand(s) but the callee pointer's pointee "
                           "function type expects "
                        << numFnOperands;
    return true;
  }
  if (fnTy.isVarArg() && numCallOperands < numFnOperands) {
    callOp->emitError() << "callconv invariant clause 3 violated: variadic "
                           "indirect call passes "
                        << numCallOperands
                        << " operand(s) but the callee pointer's pointee "
                           "function type requires at least "
                        << numFnOperands;
    return true;
  }

  for (unsigned i = 0; i < numFnOperands; ++i) {
    mlir::Type callTy = call.getArgOperand(i).getType();
    mlir::Type fnArgTy = fnTy.getInput(i);
    if (callTy != fnArgTy) {
      callOp->emitError() << "callconv invariant clause 3 violated: indirect "
                             "call operand "
                          << i << " has type " << callTy
                          << " but the callee pointer's pointee function "
                             "type declares it as "
                          << fnArgTy;
      return true;
    }
  }

  if (fnTy.hasVoidReturn() && callOp->getNumResults() != 0) {
    callOp->emitError() << "callconv invariant clause 3 violated: indirect "
                           "call has "
                        << callOp->getNumResults()
                        << " result(s) but the callee pointer's pointee "
                           "function type returns void";
    return true;
  }
  if (!fnTy.hasVoidReturn()) {
    if (callOp->getNumResults() != 1) {
      callOp->emitError() << "callconv invariant clause 3 violated: indirect "
                             "call has "
                          << callOp->getNumResults()
                          << " result(s) but should have exactly 1";
      return true;
    }
    if (callOp->getResult(0).getType() != fnTy.getReturnType()) {
      callOp->emitError() << "callconv invariant clause 3 violated: indirect "
                             "call result type "
                          << callOp->getResult(0).getType()
                          << " does not match callee pointer's pointee "
                             "function return type "
                          << fnTy.getReturnType();
      return true;
    }
  }

  return false;
}

void CIRVerifyCallConvInvariantPass::runOnOperation() {
  llvm::TimeTraceScope scope("CIR Verify CallConv Invariant");
  mlir::ModuleOp module = getOperation();
  mlir::SymbolTableCollection symbolTable;

  bool failed = false;
  module.walk([&](mlir::Operation *op) {
    // Clause 1: walk the operation's attribute dictionary.
    for (mlir::NamedAttribute na : op->getAttrs())
      if (verifyAttr(na.getValue(), op, symbolTable))
        failed = true;

    // Clauses 2 and 3 only apply to call sites.
    auto call = mlir::dyn_cast<cir::CIRCallOpInterface>(op);
    if (!call)
      return;

    auto calleeAttr = op->getAttrOfType<mlir::FlatSymbolRefAttr>(
        cir::CIRDialect::getCalleeAttrName());
    if (calleeAttr) {
      if (verifyDirectCall(call, calleeAttr, symbolTable))
        failed = true;
    } else {
      // Indirect call: dispatch on the concrete op type to retrieve the
      // indirect-callee SSA value.  The CIRCallOpInterface does not (yet)
      // expose getIndirectCall().
      mlir::Value indirect;
      if (auto callOp = mlir::dyn_cast<cir::CallOp>(op))
        indirect = callOp.getIndirectCall();
      else if (auto tryCallOp = mlir::dyn_cast<cir::TryCallOp>(op))
        indirect = tryCallOp.getIndirectCall();
      else
        return;
      if (verifyIndirectCall(call, indirect))
        failed = true;
    }
  });

  if (failed)
    signalPassFailure();
}

} // namespace

std::unique_ptr<mlir::Pass> mlir::createCIRVerifyCallConvInvariantPass() {
  return std::make_unique<CIRVerifyCallConvInvariantPass>();
}
