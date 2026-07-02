// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-call-conv-lowering -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-call-conv-lowering -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t.ll %s

union U3 { char c[3]; };
struct S1 { char c[1]; };

// A 3-byte union is real data: it coerces to i24 with a power-of-two-aligned
// coerce slot (an odd width must not trip a non-power-of-two alignment).
void take_u3(union U3 u) { (void)u; }

// CIR: cir.func {{.*}}@take_u3(%arg0: !cir.int<u, 24> {{.*}})
// LLVM: define dso_local void @take_u3(i24 %{{.+}})

// A one-byte char array field is real data, not empty-class padding.
void take_s1(struct S1 s) { (void)s; }

// CIR: cir.func {{.*}}@take_s1(%arg0: !s8i {{.*}})
// LLVM: define dso_local void @take_s1(i8 %{{.+}})

// A narrow integer already carries zeroext/noundef; Extend must not duplicate.
int take_uchar(unsigned char c) { return c; }

// CIR: cir.func {{.*}}@take_uchar(%arg0: !u8i {{.*}}llvm.zeroext{{.*}}) -> !s32i
// LLVM: define dso_local i32 @take_uchar(i8 noundef zeroext %{{.+}})
