// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

struct D { int x; };

int worker(D d) { return d.x; }

struct X;
struct Y;
struct X { int (*fn)(int, Y *); Y *yp; };
struct Y { int (*fn)(int, X *); X *xp; };

int use_x(X *x, int v) { return x->fn(v, x->yp); }

int caller(int v) {
  D d = {v};
  return worker(d) + use_x(static_cast<X *>(0), v);
}

// X and Y are not in the rename closure: neither fn member's FuncType
// is in the rewrite map, even though `worker` forces the cascade to run.
// CIR-DAG: !rec_X = !cir.record<struct "X" {!cir.ptr<!cir.func<(!s32i,
// CIR-DAG: !rec_Y = !cir.record<struct "Y" {!cir.ptr<!cir.func<(!s32i,
// CIR-NOT: unrealized_conversion_cast
// CIR-NOT: __post_abi_

// CIR-LABEL: cir.func {{.*}}@_Z6worker1D(%arg0: !s32i

// LLVM:      define {{.*}} i32 @_Z6worker1D(i32 {{.*}})
// OGCG:      define {{.*}} i32 @_Z6worker1D(i32 {{.*}})
// LLVM:      define {{.*}} i32 @_Z5use_xP1Xi(
// OGCG:      define {{.*}} i32 @_Z5use_xP1Xi(
