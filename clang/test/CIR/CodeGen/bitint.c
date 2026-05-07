// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

// _BitInt types are distinguished from regular integer types via the
// "bitint" keyword in CIR.  Verify that the type alias includes "_bitint"
// and that regular __int128 does not.

// CIR-DAG: !s32i_bitint = !cir.int<s, 32, bitint>
// CIR-DAG: !s128i_bitint = !cir.int<s, 128, bitint>
// CIR-DAG: !u64i_bitint = !cir.int<u, 64, bitint>
// CIR-DAG: !s128i = !cir.int<s, 128>
// CIR-DAG: !s256i_bitint = !cir.int<s, 256, bitint>

// _BitInt(128) has alignment 8 while __int128 has alignment 16.
signed _BitInt(128) bitint128_var;
__int128 int128_var;
signed _BitInt(256) bitint256_var;
signed _BitInt(254) bitint254_var;
signed _BitInt(257) bitint257_var;

// CIR: cir.global external @bitint128_var = #cir.int<0> : !s128i_bitint {alignment = 8 : i64}
// CIR: cir.global external @int128_var = #cir.int<0> : !s128i {alignment = 16 : i64}
// CIR: cir.global external @bitint256_var = #cir.int<0> : !s256i_bitint
// CIR: cir.global external @bitint254_var = #cir.int<0> : !cir.int<s, 254, bitint>
// CIR: cir.global external @bitint257_var = #cir.int<0> : !cir.int<s, 257, bitint>

// LLVM: @bitint128_var = global i128 0, align 8
// LLVM: @int128_var = global i128 0, align 16
// LLVM: @bitint256_var = global i256 0
// LLVM: @bitint254_var = global i254 0, align 8
// LLVM: @bitint257_var = global i257 0, align 8

// OGCG: @bitint128_var = global i128 0, align 8
// OGCG: @int128_var = global i128 0, align 16
// OGCG: @bitint256_var = global i256 0
// OGCG: @bitint254_var = global i256 0, align 8
// OGCG: @bitint257_var = global [40 x i8] zeroinitializer, align 8

void take_bitint_32(_BitInt(32) x) {}
// CIR: cir.func {{.*}} @take_bitint_32(%arg0: !s32i_bitint
// LLVM: define {{.*}} void @take_bitint_32(i32 {{.*}})
// OGCG: define {{.*}} void @take_bitint_32(i32 {{.*}})

// _BitInt(128) is INTEGER class on x86_64 SysV: a 16-byte value passed in
// two registers (RDI/RSI for an arg, RAX/RDX for a return).  CIR matches
// classic Clang's `i128` ABI signature; only _BitInt(>128) needs to go via
// memory as `byval`.
void take_bitint_128(signed _BitInt(128) x) {}
// CIR: cir.func {{.*}} @take_bitint_128(%arg0: !s128i_bitint
// LLVM: define {{.*}} void @take_bitint_128(i128 {{.*}})
// OGCG: define {{.*}} void @take_bitint_128(i128 {{.*}})

void take_unsigned_bitint(unsigned _BitInt(64) x) {}
// CIR: cir.func {{.*}} @take_unsigned_bitint(%arg0: !u64i_bitint
// LLVM: define {{.*}} void @take_unsigned_bitint(i64 {{.*}})
// OGCG: define {{.*}} void @take_unsigned_bitint(i64 {{.*}})

// _BitInt(>128) cannot fit in two integer-class registers, so it is passed
// indirect via a `byval` pointer.  The byval *type* CIR uses preserves the
// original bitwidth (`i254`); classic codegen uses the storage-padded type
// (`i256`).  These are observationally equivalent at the binary ABI level
// because the in-memory size and alignment match (both 32 bytes, align 8);
// the byval type is only a hint to LLVM about the memory layout.
void take_bitint_254(signed _BitInt(254) x) {}
// CIR: cir.func {{.*}} @take_bitint_254(%arg0: !cir.ptr<!cir.int<s, 254, bitint>>
// LLVM: define {{.*}} void @take_bitint_254(ptr noundef byval(i254) align 8 {{.*}})
// OGCG: define {{.*}} void @take_bitint_254(ptr noundef byval(i256) align 8 {{.*}})

void take_bitint_257(signed _BitInt(257) x) {}
// CIR: cir.func {{.*}} @take_bitint_257(%arg0: !cir.ptr<!cir.int<s, 257, bitint>>
// LLVM: define {{.*}} void @take_bitint_257(ptr noundef byval(i257) align 8 {{.*}})
// OGCG: define {{.*}} void @take_bitint_257(ptr noundef byval([40 x i8]) align 8 {{.*}})

// Regular __int128 should NOT have the bitint flag.
void take_int128(__int128 x) {}
// CIR: cir.func {{.*}} @take_int128(%arg0: !s128i
// LLVM: define {{.*}} void @take_int128(i128 {{.*}})
// OGCG: define {{.*}} void @take_int128(i128 {{.*}})
