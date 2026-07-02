// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-call-conv-lowering -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-call-conv-lowering -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t.ll %s

struct S { int x; int y; };
struct Big { long a, b, c, d; };
int vf(struct S s, ...);
int vbig(struct S s, ...);

// A variadic call whose fixed struct arg is coerced (to i64) and whose
// variadic scalar arguments pass through unchanged.
int call_scalar(struct S s, int a, double d) { return vf(s, a, d); }

// CIR: cir.func {{.*}}@call_scalar(%arg0: !u64i {{.*}}, %arg1: !s32i {{.*}}, %arg2: !cir.double {{.*}}) -> !s32i
// CIR: cir.call @vf(%{{.+}}, %{{.+}}, %{{.+}}) : (!u64i, !s32i {{.*}}, !cir.double {{.*}}) -> !s32i
// LLVM: define dso_local i32 @call_scalar(i64 %{{.+}}, i32 noundef %{{.+}}, double noundef %{{.+}})
// LLVM: call i32 (i64, ...) @vf(i64 %{{.+}}, i32 noundef %{{.+}}, double noundef %{{.+}})

// A variadic struct argument is classified like any other: this small struct
// coerces to a single integer eightbyte rather than being passed by value.
int call_small(struct S s, struct S t) { return vf(s, t); }

// CIR: cir.func {{.*}}@call_small(%arg0: !u64i {{.*}}, %arg1: !u64i {{.*}}) -> !s32i
// CIR: cir.call @vf(%{{.+}}, %{{.+}}) : (!u64i, !u64i) -> !s32i
// LLVM: define dso_local i32 @call_small(i64 %{{.+}}, i64 %{{.+}})
// LLVM: call i32 (i64, ...) @vf(i64 %{{.+}}, i64 %{{.+}})

// A large struct passed variadically goes byval, not coerced to an integer.
int call_big(struct S s, struct Big b) { return vbig(s, b); }

// CIR: cir.func {{.*}}@call_big(%arg0: !u64i {{.*}}, %arg1: !cir.ptr<!rec_Big> {{.*}}llvm.byval = !rec_Big{{.*}}) -> !s32i
// CIR: cir.call @vbig(%{{.+}}, %{{.+}}) : (!u64i, !cir.ptr<!rec_Big> {{.*}}llvm.byval = !rec_Big{{.*}}) -> !s32i
// LLVM: define dso_local i32 @call_big(i64 %{{.+}}, ptr {{.*}}byval(%struct.Big) align 8 %{{.+}})
// LLVM: call i32 (i64, ...) @vbig(i64 %{{.+}}, ptr {{.*}}byval(%struct.Big) align 8 %{{.+}})
