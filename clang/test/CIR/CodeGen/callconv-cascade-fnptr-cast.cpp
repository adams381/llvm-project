// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

struct D { int x; };

int worker(D d) { return d.x; }

int caller(int x) {
  using GenericFn = void (*)();
  using TypedFn = int (*)(D);
  GenericFn g = reinterpret_cast<GenericFn>(worker);
  TypedFn fp = reinterpret_cast<TypedFn>(g);
  D d = {x};
  return fp(d);
}

// CIR-LABEL: cir.func {{.*}}@_Z6worker1D(%arg0: !s32i
// LLVM:      define {{.*}} i32 @_Z6worker1D(i32 {{.*}})
// OGCG:      define {{.*}} i32 @_Z6worker1D(i32 {{.*}})

// CIR-LABEL: cir.func {{.*}}@_Z6calleri
// Cascade target: the bitcasts in caller's body carry the rewritten
// FuncType pointees.
// CIR-DAG: cir.cast bitcast {{.*}} : !cir.ptr<!cir.func<(!s32i) -> !s32i>> -> !cir.ptr<!cir.func<()>>
// CIR-DAG: cir.cast bitcast {{.*}} : !cir.ptr<!cir.func<()>> -> !cir.ptr<!cir.func<(!s32i) -> !s32i>>
// LLVM:      define {{.*}} i32 @_Z6calleri(i32 {{.*}})
// OGCG:      define {{.*}} i32 @_Z6calleri(i32 {{.*}})
