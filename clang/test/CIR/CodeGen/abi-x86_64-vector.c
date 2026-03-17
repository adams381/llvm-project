// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

// x86_64 ABI for vector types (SSE/AVX).

typedef float __m128 __attribute__((__vector_size__(16)));
typedef float __m256 __attribute__((__vector_size__(32)));
typedef int v4si __attribute__((vector_size(16)));
typedef int v2si __attribute__((vector_size(8)));

// __m128: SSE register (direct)
__m128 ret_m128(void) { __m128 v = {0}; return v; }
// LLVM: define {{.*}} <4 x float> @ret_m128()
// OGCG: define {{.*}} <4 x float> @ret_m128()

void take_m128(__m128 v) {}
// LLVM: define {{.*}} void @take_m128(<4 x float> noundef %{{.*}})
// OGCG: define {{.*}} void @take_m128(<4 x float> {{.*}} %{{.*}})

// __m256: larger than SSE -> byval on default target
__m256 ret_m256(void) { __m256 v = {0}; return v; }
// LLVM: define {{.*}} @ret_m256()
// OGCG: define {{.*}} <8 x float> @ret_m256()

void take_m256(__m256 v) {}
// LLVM: define {{.*}} void @take_m256(
// OGCG: define {{.*}} void @take_m256(ptr {{.*}} byval(<8 x float>) align 32 %{{.*}})

// v4si (128-bit int vector): SSE register
v4si ret_v4si(void) { v4si v = {0}; return v; }
// LLVM: define {{.*}} <4 x i32> @ret_v4si()
// OGCG: define {{.*}} <4 x i32> @ret_v4si()

void take_v4si(v4si v) {}
// LLVM: define {{.*}} void @take_v4si(<4 x i32> noundef %{{.*}})
// OGCG: define {{.*}} void @take_v4si(<4 x i32> {{.*}} %{{.*}})

// v2si (64-bit int vector): coerced to double per ABI
v2si ret_v2si(void) { v2si v = {0}; return v; }
// LLVM: define {{.*}} @ret_v2si()
// OGCG: define {{.*}} double @ret_v2si()

void take_v2si(v2si v) {}
// LLVM: define {{.*}} void @take_v2si(
// OGCG: define {{.*}} void @take_v2si(double {{.*}} %{{.*}})
