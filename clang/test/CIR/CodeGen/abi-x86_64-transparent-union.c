// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

typedef union {
  int *ip;
  float *fp;
} TU __attribute__((transparent_union));

void take_tu(TU u);

void test_tu(int *p) {
  take_tu(p);
}

// CIR: cir.func private @take_tu(!cir.ptr<!void>)
// CIR: cir.func {{.*}} @test_tu(%arg0: !cir.ptr<!s32i>
// CIR:   %[[P_ADDR:.*]] = cir.alloca !cir.ptr<!s32i>
// CIR:   %[[AGG_TMP:.*]] = cir.alloca !rec_TU
// CIR:   cir.store %arg0, %[[P_ADDR]]
// CIR:   %[[FIELD:.*]] = cir.get_member %[[AGG_TMP]][0] {name = "ip"}
// CIR:   %[[P_VAL:.*]] = cir.load {{.*}} %[[P_ADDR]]
// CIR:   cir.store {{.*}} %[[P_VAL]], %[[FIELD]]
// CIR:   cir.call @take_tu

// LLVM: declare void @take_tu(ptr)
// LLVM: define dso_local void @test_tu(ptr {{.*}}%[[ARG:.*]])
// LLVM:   store ptr %[[ARG]], ptr %[[P_ADDR:.*]], align 8
// LLVM:   %[[P_VAL:.*]] = load ptr, ptr %[[P_ADDR]], align 8
// LLVM:   store ptr %[[P_VAL]], ptr %[[AGG:.*]], align 8
// LLVM:   call void @take_tu(ptr %{{.*}})

// OGCG: define dso_local void @test_tu(ptr {{.*}}%[[ARG:.*]])
// OGCG:   store ptr %[[ARG]], ptr %[[P_ADDR:.*]], align 8
// OGCG:   %[[P_VAL:.*]] = load ptr, ptr %[[P_ADDR]], align 8
// OGCG:   store ptr %[[P_VAL]], ptr %[[AGG:.*]], align 8
// OGCG:   call void @take_tu(ptr %{{.*}})
// OGCG: declare void @take_tu(ptr)
