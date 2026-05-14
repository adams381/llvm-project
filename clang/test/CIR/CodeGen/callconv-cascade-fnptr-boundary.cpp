// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

struct D { int x; };

int worker(D d) { return d.x; }

struct Bystander { int unrelated; };

int caller(int v) {
  D d = {v};
  Bystander b = {v};
  return worker(d) + b.unrelated;
}

// Bystander is outside the rename closure: no fnptr-typed member.
// CIR-DAG: !rec_Bystander = !cir.record<struct "Bystander" {!s32i}>
// CIR-LABEL: cir.func {{.*}}@_Z6worker1D(%arg0: !s32i
// CIR-NOT: unrealized_conversion_cast

// LLVM:      define {{.*}} i32 @_Z6worker1D(i32 {{.*}})
// OGCG:      define {{.*}} i32 @_Z6worker1D(i32 {{.*}})
// LLVM:      define {{.*}} i32 @_Z6calleri(i32 {{.*}})
// OGCG:      define {{.*}} i32 @_Z6calleri(i32 {{.*}})
