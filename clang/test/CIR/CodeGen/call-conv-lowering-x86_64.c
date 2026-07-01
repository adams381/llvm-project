// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-call-conv-lowering -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-call-conv-lowering -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefixes=LLVM,LLVM-CIR --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefixes=LLVM,LLVM-OGCG --input-file=%t.ll %s

typedef struct { int x; int y; } Pair2;
typedef struct { long a; long b; } Pair16;
typedef struct { long a, b, c, d; } Big;
typedef union { int i; float f; } U;

// Two-int struct returned in a single INTEGER eightbyte -> i64.
Pair2 ret_pair2(int a) { Pair2 p = {a, a}; return p; }

// CIR: cir.func {{.*}}@ret_pair2(%arg0: !s32i {{.*}}) -> !u64i
// LLVM: define dso_local i64 @ret_pair2(i32 noundef %{{.+}})

// Large struct returned indirectly via sret.
Big ret_big(void) { Big b = {1, 2, 3, 4}; return b; }

// CIR: cir.func {{.*}}@ret_big(%arg0: !cir.ptr<!rec_Big> {{.*}}llvm.sret = !rec_Big{{.*}})
// LLVM: define dso_local void @ret_big(ptr dead_on_unwind noalias writable sret(%struct.Big) align 8 %{{.+}})

// 16-byte struct flattened into two integer registers.
void take_pair16(Pair16 p) { (void)p; }

// CIR: cir.func {{.*}}@take_pair16(%arg0: !s64i{{.*}}, %arg1: !s64i{{.*}})
// LLVM: define dso_local void @take_pair16(i64 %{{.+}}, i64 %{{.+}})

// Large struct passed byval.  CIR currently also emits noalias on byval;
// OGCG only does so under -fpass-by-value-is-noalias.
void take_big(Big b) { (void)b; }

// CIR: cir.func {{.*}}@take_big(%arg0: !cir.ptr<!rec_Big> {{.*}}llvm.byval = !rec_Big{{.*}})
// LLVM-CIR: define dso_local void @take_big(ptr noalias noundef byval(%struct.Big) align 8 %{{.+}})
// LLVM-OGCG: define dso_local void @take_big(ptr noundef byval(%struct.Big) align 8 %{{.+}})

// Union coerced to its integer eightbyte.
int take_union(U u) { return u.i; }

// CIR: cir.func {{.*}}@take_union(%arg0: !s32i{{.*}}) -> !s32i
// LLVM: define dso_local i32 @take_union(i32 %{{.+}})
