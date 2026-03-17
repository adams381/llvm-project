// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

// x86_64 ABI for long double (x87 80-bit).

// Scalar long double: x87 register
long double ret_ld(void) { return 0; }
// LLVM: define {{.*}} x86_fp80 @ret_ld()
// OGCG: define {{.*}} x86_fp80 @ret_ld()

void take_ld(long double x) {}
// LLVM: define {{.*}} void @take_ld(x86_fp80 noundef %{{.*}})
// OGCG: define {{.*}} void @take_ld(x86_fp80 {{.*}} %{{.*}})

// Struct with long double: X87 class -> returned in x87 register
struct HasLD { long double x; };
struct HasLD ret_has_ld(void) { struct HasLD s = {0}; return s; }
// LLVM: define {{.*}} @ret_has_ld()
// OGCG: define {{.*}} x86_fp80 @ret_has_ld()

// Struct with long double arg: MEMORY class -> byval
void take_has_ld(struct HasLD s) {}
// LLVM: define {{.*}} void @take_has_ld(
// OGCG: define {{.*}} void @take_has_ld(ptr {{.*}} byval(%struct.HasLD) align 16 %{{.*}})

// Union with long double: MEMORY class -> byval
union UnionLD { long double ld; int i; };
void take_union_ld(union UnionLD u) {}
// LLVM: define {{.*}} void @take_union_ld(
// OGCG: define {{.*}} void @take_union_ld(ptr {{.*}} byval(%union.UnionLD) align 16 %{{.*}})
