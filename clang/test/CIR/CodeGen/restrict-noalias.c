// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

// Verify restrict -> noalias and __attribute__((nonnull)).

// restrict pointer param gets noalias.
void restrict_param(int * restrict p) {}
// LLVM: define {{.*}} void @restrict_param(ptr noalias noundef %{{.*}})
// OGCG: define {{.*}} void @restrict_param(ptr noalias noundef %{{.*}})

// Non-restrict pointer does NOT get noalias.
void normal_param(int *p) {}
// LLVM: define {{.*}} void @normal_param(ptr noundef %{{.*}})
// LLVM-NOT: noalias
// OGCG: define {{.*}} void @normal_param(ptr noundef %{{.*}})

// __attribute__((nonnull)) gets nonnull.
void nonnull_param(int *p) __attribute__((nonnull(1)));
void nonnull_param(int *p) {}
// LLVM: define {{.*}} void @nonnull_param(ptr noundef nonnull %{{.*}})
// OGCG: define {{.*}} void @nonnull_param(ptr noundef nonnull %{{.*}})

// Both restrict and nonnull.
void both(int * restrict p) __attribute__((nonnull(1)));
void both(int * restrict p) {}
// LLVM: define {{.*}} void @both(ptr noalias noundef nonnull %{{.*}})
// OGCG: define {{.*}} void @both(ptr noalias noundef nonnull %{{.*}})
