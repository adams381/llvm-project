// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

typedef long long __attribute__((vector_size(16))) v2i64;

struct Wrap { v2i64 v; };

Wrap make();
int sink(Wrap w);

int caller() {
  Wrap w = make();
  return sink(w);
}

// CIR-LABEL: cir.func {{.*}}@_Z6callerv
// CIR:         cir.alloca !cir.vector<2 x !s64i>, !cir.ptr<!cir.vector<2 x !s64i>>, ["coerce"] {alignment = 16 : i64}
// CIR:         cir.alloca !rec_Wrap, !cir.ptr<!rec_Wrap>, ["coerce"] {alignment = 16 : i64}

// LLVM-LABEL: define {{.*}} i32 @_Z6callerv()
// LLVM:         alloca <2 x i64>{{.*}}align 16

// OGCG-LABEL: define {{.*}} i32 @_Z6callerv()
// OGCG:         alloca %struct.Wrap, {{.*}}align 16
