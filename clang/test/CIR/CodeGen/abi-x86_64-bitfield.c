// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

// x86_64 ABI for structs with bit-fields.

// Small bit-field struct: fits in one register
struct BF1 { int a:1; int b:1; int c:1; };
void take_bf1(struct BF1 s) {}
// LLVM: define {{.*}} void @take_bf1(i32 %{{.*}})
// OGCG: define {{.*}} void @take_bf1(i32 %{{.*}})

struct BF1 ret_bf1(void) { struct BF1 s = {0}; return s; }
// LLVM: define {{.*}} i32 @ret_bf1()
// OGCG: define {{.*}} i32 @ret_bf1()

// Zero-width bit-field forces eightbyte boundary
struct ZeroWidth { int a; int :0; int b; };
void take_zerowidth(struct ZeroWidth s) {}
// LLVM: define {{.*}} void @take_zerowidth(i64 %{{.*}})
// OGCG: define {{.*}} void @take_zerowidth(i64 %{{.*}})

// Bit-fields spanning eightbyte boundary: two registers
struct BFSpan { long long a:40; long long b:40; };
void take_bfspan(struct BFSpan s) {}
// LLVM: define {{.*}} void @take_bfspan(i64 %{{.*}}, i64 %{{.*}})
// OGCG: define {{.*}} void @take_bfspan(i64 %{{.*}}, i64 %{{.*}})
