// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

// Verify that array-of-float/double struct members preserve
// their float type during ABI coercion, not falling through
// to an integer type.

// struct { double[1] } should coerce to double, not i64.
struct DoubleArr1 { double f0[1]; };

void take_da1(struct DoubleArr1 s) {}
// LLVM: define {{.*}} void @take_da1(double %{{.*}})
// OGCG: define {{.*}} void @take_da1(double %{{.*}})

struct DoubleArr1 ret_da1(void) {
  struct DoubleArr1 s = {0};
  return s;
}
// LLVM: define {{.*}} double @ret_da1()
// OGCG: define {{.*}} double @ret_da1()

// struct { float[1]; unsigned long long } should coerce to
// { float, i64 }, not { i32, i64 }.
struct FloatArr1Long { float f0[1]; unsigned long long f1; };

void take_fa1l(struct FloatArr1Long s) {}
// LLVM: define {{.*}} void @take_fa1l(float %{{.*}}, i64 %{{.*}})
// OGCG: define {{.*}} void @take_fa1l(float %{{.*}}, i64 %{{.*}})

struct FloatArr1Long ret_fa1l(void) {
  struct FloatArr1Long s = {0};
  return s;
}
// LLVM: define {{.*}} { float, i64 } @ret_fa1l()
// OGCG: define {{.*}} { float, i64 } @ret_fa1l()

// struct { float[2] } should coerce to <2 x float> (SSE).
struct FloatArr2 { float f0[2]; };

void take_fa2(struct FloatArr2 s) {}
// LLVM: define {{.*}} void @take_fa2({{.*}} %{{.*}})
// OGCG: define {{.*}} void @take_fa2(<2 x float> %{{.*}})
