// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

// x86_64 ABI for __int128 types.

__int128 ret_int128(void) { return 0; }
// LLVM: define {{.*}} i128 @ret_int128()
// OGCG: define {{.*}} i128 @ret_int128()

void take_int128(__int128 x) {}
// LLVM: define {{.*}} void @take_int128(i128 noundef %{{.*}})
// OGCG: define {{.*}} void @take_int128(i128 {{.*}} %{{.*}})

unsigned __int128 ret_uint128(void) { return 0; }
// LLVM: define {{.*}} i128 @ret_uint128()
// OGCG: define {{.*}} i128 @ret_uint128()

struct HasInt128 { __int128 x; };
void take_has_int128(struct HasInt128 s) {}
// LLVM: define {{.*}} void @take_has_int128(i128 %{{.*}})
// OGCG: define {{.*}} void @take_has_int128(i128 %{{.*}})
