// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

// x86_64 ABI for __bf16 (brain floating point).

// Scalar __bf16: SSE register
void take_bf16(__bf16 x) {}
// CIR: cir.func {{.*}} @take_bf16(%arg0: !cir.bf16
// LLVM: define {{.*}} void @take_bf16(bfloat noundef %{{.*}})
// OGCG: define {{.*}} void @take_bf16(bfloat {{.*}} %{{.*}})

__bf16 ret_bf16(__bf16 *p) { return *p; }
// CIR: cir.func {{.*}} @ret_bf16(%arg0: !cir.ptr<!cir.bf16>
// LLVM: define {{.*}} bfloat @ret_bf16(
// OGCG: define {{.*}} bfloat @ret_bf16(

// Struct with single __bf16: SSE class
struct OneBF16 { __bf16 a; };
void take_one_bf16(struct OneBF16 s) {}
// CIR: cir.func {{.*}} @take_one_bf16(%arg0: !cir.bf16
// LLVM: define {{.*}} void @take_one_bf16(bfloat %{{.*}})
// OGCG: define {{.*}} void @take_one_bf16(bfloat %{{.*}})

// Struct with two __bf16: SSE class, coerced to <2 x bfloat>
struct TwoBF16 { __bf16 a, b; };
void take_two_bf16(struct TwoBF16 s) {}
// CIR: cir.func {{.*}} @take_two_bf16(%arg0: !cir.vector<2 x !cir.bf16>
// LLVM: define {{.*}} void @take_two_bf16(<2 x bfloat> %{{.*}})
// OGCG: define {{.*}} void @take_two_bf16(<2 x bfloat> %{{.*}})

// Struct with four __bf16: SSE class, coerced to <4 x bfloat>
struct FourBF16 { __bf16 a, b, c, d; };
void take_four_bf16(struct FourBF16 s) {}
// CIR: cir.func {{.*}} @take_four_bf16(%arg0: !cir.vector<4 x !cir.bf16>
// LLVM: define {{.*}} void @take_four_bf16(<4 x bfloat> %{{.*}})
// OGCG: define {{.*}} void @take_four_bf16(<4 x bfloat> %{{.*}})

// Mixed: __bf16 + _Float16 in same eightbyte
struct BF16F16 { __bf16 a; _Float16 b; };
void take_bf16_f16(struct BF16F16 s) {}
// CIR: cir.func {{.*}} @take_bf16_f16(%arg0: !cir.vector<2 x !cir.bf16>
// LLVM: define {{.*}} void @take_bf16_f16(<2 x bfloat> %{{.*}})
// OGCG: define {{.*}} void @take_bf16_f16(<2 x bfloat> %{{.*}})

// Mixed: __bf16 + int (INTEGER dominates SSE in merge)
struct BF16Int { __bf16 a; int b; };
void take_bf16_int(struct BF16Int s) {}
// CIR: cir.func {{.*}} @take_bf16_int(%arg0: !u64i
// LLVM: define {{.*}} void @take_bf16_int(i64 %{{.*}})
// OGCG: define {{.*}} void @take_bf16_int(i64 %{{.*}})
