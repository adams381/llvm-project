// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

// Verify that structs with inheritance whose data size spans
// a partial second eightbyte produce two full i64 registers,
// not (i64, i40).  Regression test for dataSizeInBits rounding.

struct f2_s0 { unsigned a; unsigned b; float c; };
struct f2_s1 : public f2_s0 { char d; };

// CIR: cir.func {{.*}}@_Z2f25f2_s1(%arg0: !u64i {{.*}}, %arg1: !u64i
// LLVM: define {{.*}} void @_Z2f25f2_s1(i64 %{{.*}}, i64 %{{.*}})
// OGCG: define {{.*}} void @_Z2f25f2_s1(i64 %{{.*}}, i64 %{{.*}})
void f2(f2_s1 a0) { }

// Also test the return path: should return {i64, i64}.

// CIR: cir.func {{.*}}@_Z6ret_f2v
// LLVM: define {{.*}} { i64, i64 } @_Z6ret_f2v()
// OGCG: define {{.*}} { i64, i64 } @_Z6ret_f2v()
f2_s1 ret_f2(void) {
  f2_s1 s{};
  return s;
}
