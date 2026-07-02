// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-call-conv-lowering -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-call-conv-lowering -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t.ll %s

typedef struct { int x; int y; } Pair2;
typedef struct { long a, b, c, d; } Big;

// Indirect call whose callee returns a coerced small struct: the callee
// pointer is bitcast to the coerced function type before the call.
Pair2 call_ret(Pair2 (*fp)(int), int a) { return fp(a); }

// CIR: cir.func {{.*}}@call_ret(%arg0: !cir.ptr<!cir.func<(!s32i) -> !rec_Pair2>> {{.*}}, %arg1: !s32i {{.*}}) -> !u64i
// CIR: cir.cast bitcast %{{.+}} : !cir.ptr<!cir.func<(!s32i) -> !rec_Pair2>> -> !cir.ptr<!cir.func<(!s32i) -> !u64i>>
// CIR: cir.call %{{.+}}(%{{.+}}) : (!cir.ptr<!cir.func<(!s32i) -> !u64i>>, !s32i {{.*}}) -> !u64i
// LLVM: define dso_local i64 @call_ret(ptr noundef %{{.+}}, i32 noundef %{{.+}})
// LLVM: call i64 %{{.+}}(i32 noundef %{{.+}})

// Indirect call whose callee returns a large struct via sret.
Big call_sret(Big (*fp)(void)) { return fp(); }

// CIR: cir.func {{.*}}@call_sret(%arg0: !cir.ptr<!rec_Big> {{.*}}llvm.sret = !rec_Big{{.*}}, %arg1: !cir.ptr<!cir.func<() -> !rec_Big>> {{.*}})
// LLVM: define dso_local void @call_sret(ptr dead_on_unwind noalias writable sret(%struct.Big) align 8 %{{.+}}, ptr noundef %{{.+}})
// LLVM: call void %{{.+}}(ptr dead_on_unwind writable sret(%struct.Big) align 8 %{{.+}})

// Indirect call whose callee takes a struct flattened into a register.
void call_void(void (*fp)(Pair2), Pair2 p) { fp(p); }

// CIR: cir.func {{.*}}@call_void(%arg0: !cir.ptr<!cir.func<(!rec_Pair2)>> {{.*}}, %arg1: !u64i {{.*}})
// LLVM: define dso_local void @call_void(ptr noundef %{{.+}}, i64 %{{.+}})
// LLVM: call void %{{.+}}(i64 %{{.+}})
