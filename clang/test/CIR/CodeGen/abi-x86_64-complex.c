// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

// x86_64 ABI for _Complex types.

// _Complex float: returned/passed as <2 x float>
float _Complex ret_cf(void) { float _Complex c = 1.0f; return c; }
// LLVM: define {{.*}} @ret_cf()
// OGCG: define {{.*}} <2 x float> @ret_cf()

void take_cf(float _Complex c) {}
// LLVM: define {{.*}} void @take_cf(
// OGCG: define {{.*}} void @take_cf(<2 x float> {{.*}} %{{.*}})

// _Complex double: returned as {double, double}, passed as two doubles
double _Complex ret_cd(void) { double _Complex c = 1.0; return c; }
// LLVM: define {{.*}} { double, double } @ret_cd()
// OGCG: define {{.*}} { double, double } @ret_cd()

void take_cd(double _Complex c) {}
// LLVM: define {{.*}} void @take_cd(
// OGCG: define {{.*}} void @take_cd(double {{.*}} %{{.*}}, double {{.*}} %{{.*}})
