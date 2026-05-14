// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

struct D { int x; };

struct A;
struct B;
struct C;

struct A { int (*fn)(D, B *); B *bp; };
struct B { int (*fn)(D, C *); C *cp; };
struct C { int (*fn)(D, A *); A *ap; };

int worker_a(D d, B *) { return d.x; }
int worker_b(D d, C *) { return d.x; }
int worker_c(D d, A *) { return d.x; }

int caller(A *a, int v) {
  D d = {v};
  return a->fn(d, a->bp);
}

// In mutually recursive records MLIR's alias printer expands one of them
// inline; the others appear as !rec_X aliases.
// CIR-DAG: !rec_C = !cir.record<struct "C" {!cir.ptr<!cir.func<(!s32i, !cir.ptr<!rec_A>) -> !s32i>>, !cir.ptr<!rec_A>}>
// CIR-DAG: !rec_B = !cir.record<struct "B" {!cir.ptr<!cir.func<(!s32i, !cir.ptr<!rec_C>) -> !s32i>>, !cir.ptr<!rec_C>}>

// CIR-LABEL: cir.func {{.*}}@_Z8worker_a1DP1B(%arg0: !s32i
// CIR-LABEL: cir.func {{.*}}@_Z8worker_b1DP1C(%arg0: !s32i
// CIR-LABEL: cir.func {{.*}}@_Z8worker_c1DP1A(%arg0: !s32i

// CIR-LABEL: cir.func {{.*}}@_Z6callerP1Ai
// CIR:         cir.call %{{.+}}(%{{.+}}, %{{.+}}) : (!cir.ptr<!cir.func<(!s32i, !cir.ptr<!rec_B>) -> !s32i>>, !s32i {{.*}}, !cir.ptr<!rec_B> {{.*}}) -> (!s32i {{.*}})

// LLVM:      define {{.*}} i32 @_Z8worker_a1DP1B(i32 {{.*}}, ptr {{.*}})
// OGCG:      define {{.*}} i32 @_Z8worker_a1DP1B(i32 {{.*}}, ptr {{.*}})
// LLVM:      define {{.*}} i32 @_Z6callerP1Ai(
// OGCG:      define {{.*}} i32 @_Z6callerP1Ai(
