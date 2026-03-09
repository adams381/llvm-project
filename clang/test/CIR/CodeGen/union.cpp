// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

// Should generate a union type with all members preserved.
union U {
  bool b;
  short s;
  int i;
  float f;
  double d;
};
// CIR: !rec_U = !cir.record<union "U" {!cir.bool, !s16i, !s32i, !cir.float, !cir.double}>
// LLVM: %union.U = type { double }
// OGCG: %union.U = type { double }

void shouldGenerateUnionAccess(union U u) {
  u.b = true;
  u.b;
  u.i = 1;
  u.i;
  u.f = 0.1F;
  u.f;
  u.d = 0.1;
  u.d;
}
// CIR: cir.func {{.*}}shouldGenerateUnionAccess
// CIR:   cir.get_member %{{.*}}[0] {name = "b"} : !cir.ptr<!rec_U> -> !cir.ptr<!cir.bool>
// CIR:   cir.get_member %{{.*}}[2] {name = "i"} : !cir.ptr<!rec_U> -> !cir.ptr<!s32i>
// CIR:   cir.get_member %{{.*}}[3] {name = "f"} : !cir.ptr<!rec_U> -> !cir.ptr<!cir.float>
// CIR:   cir.get_member %{{.*}}[4] {name = "d"} : !cir.ptr<!rec_U> -> !cir.ptr<!cir.double>

// LLVM: define {{.*}}shouldGenerateUnionAccess{{.*}}(i64 %{{.*}})
// LLVM:   %[[BASE:.*]] = alloca %union.U
// LLVM:   store %union.U %{{.*}}, ptr %[[BASE]]
// LLVM:   store i8 1, ptr %[[BASE]]
// LLVM:   store i32 1, ptr %[[BASE]]
// LLVM:   store float 0x3FB99999A0000000, ptr %[[BASE]]
// LLVM:   store double 1.000000e-01, ptr %[[BASE]]

// OGCG: define {{.*}}shouldGenerateUnionAccess
// OGCG:   %[[BASE:.*]] = alloca %union.U
// OGCG:   %[[DIVE:.*]] = getelementptr inbounds nuw %union.U, ptr %[[BASE]], i32 0, i32 0
// OGCG:   store i64 %{{.*}}, ptr %[[DIVE]]
// OGCG:   store i8 1, ptr %[[BASE]]
// OGCG:   store i32 1, ptr %[[BASE]]
// OGCG:   store float 0x3FB99999A0000000, ptr %[[BASE]]
// OGCG:   store double 1.000000e-01, ptr %[[BASE]]
