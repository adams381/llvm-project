// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

// x86_64 ABI for aligned and packed structs.

// aligned(16): 8 bytes of data but 16 bytes total -> one register
struct Aligned16 { int a, b; } __attribute__((aligned(16)));

void take_aligned16(struct Aligned16 s) {}
// LLVM: define {{.*}} void @take_aligned16(
// OGCG: define {{.*}} void @take_aligned16(i64 %{{.*}})

struct Aligned16 ret_aligned16(void) { struct Aligned16 s = {1,2}; return s; }
// LLVM: define {{.*}} @ret_aligned16()
// OGCG: define {{.*}} i64 @ret_aligned16()

// aligned(32): exceeds 16-byte limit -> memory (byval align 32)
struct Aligned32 { int a, b; } __attribute__((aligned(32)));

void take_aligned32(struct Aligned32 s) {}
// LLVM: define {{.*}} void @take_aligned32(
// OGCG: define {{.*}} void @take_aligned32(ptr {{.*}} byval(%struct.Aligned32) align 32 %{{.*}})

// packed: tightly packed, 5 bytes -> coerced to i40
struct Packed { int a; char b; } __attribute__((packed));

void take_packed(struct Packed s) {}
// LLVM: define {{.*}} void @take_packed(i40 %{{.*}})
// OGCG: define {{.*}} void @take_packed(i40 %{{.*}})

struct Packed ret_packed(void) { struct Packed s = {1, 2}; return s; }
// LLVM: define {{.*}} i40 @ret_packed()
// OGCG: define {{.*}} i40 @ret_packed()
