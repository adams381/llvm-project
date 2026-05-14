// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

extern void use(float f);
float a, b;

void fenv_access(void) {
  #pragma STDC FENV_ACCESS ON
  __builtin_set_flt_rounds(0);
  use(a + b);
}

// CIR: cir.func {{.*}}@fenv_access
// CIR: cir.return

// CIR's LLVM lowering does not yet emit constrained FP intrinsics; the
// fix here closes the assertion crash only.
// LLVM: define {{.*}}@fenv_access
// LLVM: fadd float
// LLVM: ret void

// OGCG: define {{.*}}@fenv_access
// OGCG: call float @llvm.experimental.constrained.fadd.f32(float %{{.+}}, float %{{.+}}, metadata !"round.dynamic", metadata !"fpexcept.strict")
// OGCG: ret void
