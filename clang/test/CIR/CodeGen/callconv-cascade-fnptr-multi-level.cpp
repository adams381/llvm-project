// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

struct D { int x; };

int worker(D d) { return d.x; }

struct Holder { int (*fp)(D); };

int caller(Holder *table[3], int idx, int x) {
  D d = {x};
  return table[idx]->fp(d);
}

// Cascade target: Holder still holds the rewritten fp pointee even when
// reached through array-of-pointer indirection.
// CIR-DAG: !rec_Holder = !cir.record<struct "Holder" {!cir.ptr<!cir.func<(!s32i) -> !s32i>>}>

// CIR-LABEL: cir.func {{.*}}@_Z6worker1D(%arg0: !s32i
// LLVM:      define {{.*}} i32 @_Z6worker1D(i32 {{.*}})
// OGCG:      define {{.*}} i32 @_Z6worker1D(i32 {{.*}})

// CIR-LABEL: cir.func {{.*}}@_Z6callerPP6Holderii
// LLVM:      define {{.*}} i32 @_Z6callerPP6Holderii(
// OGCG:      define {{.*}} i32 @_Z6callerPP6Holderii(
