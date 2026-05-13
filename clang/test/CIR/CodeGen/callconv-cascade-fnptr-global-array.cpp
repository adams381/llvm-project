// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

struct D { int x; };

int worker(D d) { return d.x; }
int worker2(D d) { return d.x + 1; }

using Fn = int (*)(D);
static Fn gTable[2] = {worker, worker2};

int caller(int idx, int x) {
  D d = {x};
  return gTable[idx](d);
}

// Cascade target: gTable's element type and its const_array initializer
// both reference the rewritten FuncType.
// CIR-DAG: cir.global {{.*}}@_ZL6gTable = #cir.const_array<{{.*}}> : !cir.array<!cir.ptr<!cir.func<(!s32i) -> !s32i>> x 2>

// CIR-LABEL: cir.func {{.*}}@_Z6worker1D(%arg0: !s32i
// LLVM:      define {{.*}} i32 @_Z6worker1D(i32 {{.*}})
// OGCG:      define {{.*}} i32 @_Z6worker1D(i32 {{.*}})

// CIR-LABEL: cir.func {{.*}}@_Z6callerii
// LLVM:      define {{.*}} i32 @_Z6callerii(i32 {{.*}}, i32 {{.*}})
// OGCG:      define {{.*}} i32 @_Z6callerii(i32 {{.*}}, i32 {{.*}})
