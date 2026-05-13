//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef CLANG_CIR_DIALECT_PASSES_TD
#define CLANG_CIR_DIALECT_PASSES_TD

include "mlir/Pass/PassBase.td"

def CIRCanonicalize : Pass<"cir-canonicalize"> {
  let summary = "Performs CIR canonicalization";
  let description = [{
    Perform canonicalizations on CIR and removes some redundant operations.

    This pass performs basic cleanup and canonicalization transformations that
    are not intended to affect CIR-to-source fidelity and high-level code
    analysis passes. Example transformations performed in this pass include
    empty scope cleanup, trivial `try` cleanup, redundant branch cleanup, etc.
    Those more "heavyweight" transformations and those transformations that
    could significantly affect CIR-to-source fidelity are performed in the
    `cir-simplify` pass.
  }];

  let constructor = "mlir::createCIRCanonicalizePass()";
  let dependentDialects = ["cir::CIRDialect"];
}

def CIRSimplify : Pass<"cir-simplify"> {
  let summary = "Performs CIR simplification and code optimization";
  let description = [{
    The pass performs semantics-preserving code simplifications and optimizations
    on CIR while maintaining strict program correctness.

    Unlike the `cir-canonicalize` pass, these transformations may reduce the IR's
    structural similarity to the original source code as a trade-off for improved
    code quality. This can affect debugging fidelity by altering intermediate
    representations of folded expressions, hoisted operations, and other
    optimized constructs.

    Example transformations include ternary expression folding and code hoisting
    while preserving program semantics.
  }];
  let constructor = "mlir::createCIRSimplifyPass()";
  let dependentDialects = ["cir::CIRDialect"];
}

def HoistAllocas : Pass<"cir-hoist-allocas"> {
  let summary = "Hoist allocas to the entry of the function";
  let description = [{
    This pass hoist all non-dynamic allocas to the entry of the function.
    This is helpful for later code generation.
  }];
  let constructor = "mlir::createHoistAllocasPass()";
  let dependentDialects = ["cir::CIRDialect"];
}

def CIRFlattenCFG : Pass<"cir-flatten-cfg"> {
  let summary = "Produces flatten CFG";
  let description = [{
    This pass transforms CIR by inlining all the nested regions. Thus,
    the following conditions are true after the pass applied:
    - there are no nested regions in any function body
    - all the blocks in a function belong to the parent region
    In other words, this pass removes such CIR operations like IfOp, LoopOp,
    ScopeOp and etc. and produces a flat CIR.
  }];
  let constructor = "mlir::createCIRFlattenCFGPass()";
  let dependentDialects = ["cir::CIRDialect"];
}

def GotoSolver : Pass<"cir-goto-solver"> {
  let summary = "Replaces goto operations with branches";
  let description = [{
    This pass transforms CIR and replaces goto-s with branch
    operations to the proper blocks.
  }];
  let constructor = "mlir::createGotoSolverPass()";
  let dependentDialects = ["cir::CIRDialect"];
}

def CXXABILowering : Pass<"cir-cxxabi-lowering", "mlir::ModuleOp"> {
  let summary = "Lower CIR according to C++ ABI requirements";
  let description = [{
    This pass lowers CIR operations and types that represent high-level C/C++
    constructs to a more fundamental form according to the target ABI
    requirements.

    See the description of the `TargetLowering` pass for the difference between
    this pass and the `TargetLowering` pass.
  }];
  let constructor = "mlir::createCXXABILoweringPass()";
  let dependentDialects = ["cir::CIRDialect"];
}

def TargetLowering : Pass<"cir-target-lowering", "mlir::ModuleOp"> {
  let summary = "Lower CIR to a target-specific form";
  let description = [{
    This pass lowers CIR operations from a target-agnostic form to a
    target-specific form without considering ABI requirements.

    CIR has three passes in its lowering pipeline that transform input CIR
    according to target-specific requirements, scheduled in the pipeline by the
    following order:

      1. The `TargetLowering` pass.
      2. The `CXXABILowering` pass.
      3. The `CallConvLowering` pass (not implemented yet).

    The `TargetLowering` pass acts more like a legalization pass. It ensures
    every operation in CIR conforms to the target's constraints. An example
    would be the handling of synchronization scopes of atomic operations. The
    x86 family of targets only support a system-wide synchronization scope, thus
    any atomic operations with a different synchronization scope would be
    transformed to use the system-wide scope in this pass.

    The `CXXABILowering` pass and the (not yet implemented) `CallConvLowering`
    pass transform the CIR according to the target's ABI requirements. The
    former handles all ABI-related lowering except for calling convention
    handling, which is handled specifically in the latter. Example
    transformations that the `CXXABILowering` pass could make include:

      - Replace C/C++ types that have an ABI-defined layout with more
        fundamental types corresponding to the ABI requirements. For example,
        the layout of the pointer-to-data-member type in C++ is ABI-defined,
        thus the `CXXABILowering` pass would replace all occurrences of this
        type according to the ABI requirements. With a typical target ABI, it
        will be replaced by the `ptrdiff_t` type since this is the type most
        ABIs use for the layout of the pointer-to-data-member type.
      - Replace CIR operations that have ABI-dependent implementations with more
        fundamental operations. For example, the `dynamic_cast` operator in C++
        is implemented in an ABI-dependent way, typically by calling into a
        library function provided by the implementation. The `CXXABILowering`
        pass would thus replace all `cir.dyn_cast` operations with corresponding
        library function calls.

    The `CallConvLowering` pass is dedicated to handle calling conventions. It
    rewrites function signatures according to calling convention requirements,
    and updates function body and call sites accordingly. For example, when
    passing a large struct by value in C/C++, most ABIs require passing a
    pointer to a copy of the struct instead. Thus, the `CallConvLowering` pass
    would rewrite function signatures that take a value of a large struct type
    to take a pointer to the struct type instead, and update the function body
    and the call sites accordingly.
  }];
  let constructor = "mlir::createTargetLoweringPass()";
  let dependentDialects = ["cir::CIRDialect"];
}

def CallConvLowering : Pass<"cir-call-conv-lowering", "mlir::ModuleOp"> {
  let summary = "Lower CIR calling conventions to ABI-specific form";
  let description = [{
    This pass rewrites CIR function signatures and call sites to match
    the target ABI calling conventions.
  }];
  let constructor = "mlir::createCallConvLoweringPass()";
  let dependentDialects = ["cir::CIRDialect"];
  let options = [
    Option<"recordCoercionEnabled", "record-coercion", "bool",
      /*default=*/"true",
      "Enable ABI coercion for record types.">,
    Option<"passByValueIsNoAlias", "pass-by-value-is-noalias", "bool",
      /*default=*/"false",
      "Add noalias to byval parameters.">,
    Option<"x86AvxAbiLevel", "x86-avx-abi-level", "unsigned",
      /*default=*/"0",
      "X86 AVX ABI level: 0=None, 1=AVX, 2=AVX512.">
  ];
}

def CIRVerifyCallConvInvariant
    : Pass<"cir-verify-callconv-invariant", "mlir::ModuleOp"> {
  let summary = "Verify post-CallConvLowering invariants on the CIR module";
  let description = [{
    Walks the module after `CallConvLowering` and asserts the call-site
    clauses of the function-pointer type cascade invariant from
    `clang/docs/CIR/ABILowering.rst`:

      Clause 2 -- Every direct `cir.call` / `cir.try_call` site's signature
      matches the callee's declared signature (operand and result types and
      arity).  The standard MLIR verifier already enforces this via
      `cir::CallOp::verifySymbolUses`; this clause re-checks it as defensive
      duplication and to cover the calling-convention `MissingFeature` paths
      that bypass some standard checks.

      Clause 3 -- Every indirect `cir.call` / `cir.try_call` site agrees
      with the pointee `cir.func` type of the indirect-callee value (operand
      and result types and arity).  The standard verifier short-circuits
      indirect calls (see `verifyCallCommInSymbolUses` in `CIRDialect.cpp`),
      so this is the unique add-on coverage of the post-cascade indirect-
      call invariant.  An exception is made for the C++ pointer-to-member-
      function lowering shape, where CXXABILowering emits function pointers
      with type `func<(!cir.ptr<!void>, !cir.ptr<RecordType>, ...args)>` but
      the call passes only `(adjusted_this, ...args)` -- dropping the
      typed-this slot, which `LowerToLLVM` later reconciles via a bitcast.

    Clause 1 from the design doc (every `cir.global_view` agrees with its
    referenced `cir.func` declaration) is intentionally NOT enforced here:
    `cir.global_view` explicitly allows the view's type to differ from the
    referenced symbol's declared type, which C-interop callback tables rely
    on.  See the `checkGlobalView` comment in
    `VerifyCallConvInvariant.cpp` for the rationale.

    On a violation the pass calls `op->emitError` with a descriptive message
    and `signalPassFailure`.
  }];
  let constructor = "mlir::createCIRVerifyCallConvInvariantPass()";
  let dependentDialects = ["cir::CIRDialect"];
}

def CIREHABILowering : Pass<"cir-eh-abi-lowering", "mlir::ModuleOp"> {
  let summary = "Lower flattened CIR EH operations to target-specific ABI form";
  let description = [{
    This pass lowers the ABI-agnostic exception handling operations produced by
    CFG flattening into an ABI-specific form. Currently only the Itanium C++
    ABI is implemented.

    For the Itanium ABI, the pass performs the following transformations:
      - Replaces `cir.eh.initiate` with `cir.eh.inflight_exception`
      - Replaces `cir.eh.dispatch` with `cir.eh.typeid` + comparison chains
      - Removes `cir.begin_cleanup` and `cir.end_cleanup` operations
      - Replaces `cir.begin_catch` with a call to `__cxa_begin_catch`
      - Replaces `cir.end_catch` with a call to `__cxa_end_catch`
      - Replaces `cir.resume` with `cir.resume.flat`
      - Sets the personality function attribute on functions that require EH

    If a non-Itanium ABI is specified, the pass emits a diagnostic indicating
    that the target is not yet implemented.
  }];
  let constructor = "mlir::createCIREHABILoweringPass()";
  let dependentDialects = ["cir::CIRDialect"];
}

def LoweringPrepare : Pass<"cir-lowering-prepare"> {
  let summary = "Lower to more fine-grained CIR operations before lowering to "
    "other dialects";
  let description = [{
    This pass does preparation work for lowering to other dialects. For example,
    it may expand the global variable initialziation in a more ABI-friendly form.
  }];
  let constructor = "mlir::createLoweringPreparePass()";
  let dependentDialects = ["cir::CIRDialect"];
}

def IdiomRecognizer : Pass<"cir-idiom-recognizer", "mlir::ModuleOp"> {
  let summary = "Raise calls to C/C++ libraries to CIR operations";
  let description = [{
    This pass recognizes idiomatic C++ usage and captures information about C++
    standard library containers, library functions calls, and types into CIR
    operations, attributes and types.
  }];
  let constructor = "mlir::createIdiomRecognizerPass()";
  let dependentDialects = ["cir::CIRDialect"];
}

#endif // CLANG_CIR_DIALECT_PASSES_TD
