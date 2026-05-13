// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

struct D { int x; };

int worker(D d) { return d.x; }

struct Inner { int (*fp)(D); };
struct Outer { Inner inner; };

int caller(Outer o, int x) {
  D d = {x};
  return o.inner.fp(d);
}

// Cascade target: Inner's fp pointee is rewritten and Outer holds the rewritten Inner.
// CIR-DAG: !rec_Inner = !cir.record<struct "Inner" {!cir.ptr<!cir.func<(!s32i) -> !s32i>>}>
// CIR-DAG: !rec_Outer = !cir.record<struct "Outer" {!rec_Inner}>

// CIR-LABEL: cir.func {{.*}}@_Z6worker1D(%arg0: !s32i
// LLVM:      define {{.*}} i32 @_Z6worker1D(i32 {{.*}})
// OGCG:      define {{.*}} i32 @_Z6worker1D(i32 {{.*}})

// CIR-LABEL: cir.func {{.*}}@_Z6caller5Outeri
// LLVM:      define {{.*}} i32 @_Z6caller5Outeri(
// OGCG:      define {{.*}} i32 @_Z6caller5Outeri(
