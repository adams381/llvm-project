// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --input-file=%t.cir %s -check-prefix=CIR
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -Wno-unused-value -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --input-file=%t-cir.ll %s -check-prefix=LLVM
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -Wno-unused-value -emit-llvm %s -o %t.ll
// RUN: FileCheck --input-file=%t-cir.ll %s -check-prefix=LLVM

union Union { int i; float f; };

float getF();

void toUnion() {
  // CIR-LABEL: toUnion()
  // LLVM-LABEL: toUnion()
  //
  (void)(union Union) getF();
  // CIR: %[[F_FUNC:.*]] = cir.get_global @getF : !cir.ptr<!cir.func<(...) -> !cir.float>>
  // CIR:  %[[F_CAST:.*]] = cir.cast bitcast %[[F_FUNC]] : !cir.ptr<!cir.func<(...) -> !cir.float>> -> !cir.ptr<!cir.func<() -> !cir.float>>
  // CIR: cir.call %[[F_CAST:.*]]() : (!cir.ptr<!cir.func<() -> !cir.float>>) -> !cir.float
  // CIR: cir.return

  // LLVM: call float @getF()
  // LLVM: ret void
}

// GCC assign-from-cast extension:
union Union toUnionAssign() {
  // CIR-LABEL: toUnionAssign()
  // LLVM-LABEL: toUnionAssign()
  //
  // The calling-convention lowering pass coerces the trivially-copyable
  // single-eightbyte union return into `i32` (matching OGCG's ABI shape
  // for unions sized <= 8 bytes).  CIR therefore returns `i32` and
  // performs the load/return through the `i32` coerce slot.
  // CIR: %[[RET_ALLOCA:.*]] = cir.alloca !rec_Union, !cir.ptr<!rec_Union>, ["__retval"]
  // LLVM: %[[RET_ALLOCA:.*]] = alloca %union.Union
  union Union u;
  u = (union Union)42;
  // CIR: %[[UNION_TO_INT:.*]] = cir.cast bitcast %[[RET_ALLOCA]] : !cir.ptr<!rec_Union> -> !cir.ptr<!s32i>
  // CIR: %[[INT42:.*]] = cir.const #cir.int<42>
  // CIR: cir.store align(4) %[[INT42]], %[[UNION_TO_INT]] : !s32i, !cir.ptr<!s32i>
  // LLVM: store i32 42, ptr %[[RET_ALLOCA]]
  return u;
  // CIR: cir.return {{.*}} : !s32i
  // LLVM: ret i32 {{.*}}
}
