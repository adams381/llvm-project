// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-call-conv-lowering -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-call-conv-lowering -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefixes=LLVM,LLVM-CIR --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefixes=LLVM,OGCG --input-file=%t.ll %s

struct S { int a, b, c; };
S makeS();

// Namespace-scope variable initialized by a struct-returning call.
S g = makeS();

// CIR-LABEL: cir.func {{.*}}@__cxx_global_var_init
// CIR:         %[[CALL:.*]] = cir.call @_Z5makeSv() : () -> !rec_anon_struct
// CIR:         %[[SLOT:.*]] = cir.alloca "coerce" {{.*}} : !cir.ptr<!rec_anon_struct>
// CIR:         cir.store %[[CALL]], %[[SLOT]] : !rec_anon_struct, !cir.ptr<!rec_anon_struct>
// CIR:         %[[CAST:.*]] = cir.cast bitcast %[[SLOT]] : !cir.ptr<!rec_anon_struct> -> !cir.ptr<!rec_S>
// CIR:         %[[VAL:.*]] = cir.load %[[CAST]] : !cir.ptr<!rec_S>, !rec_S
// CIR:         cir.store {{.*}} %[[VAL]], {{.*}} : !rec_S, !cir.ptr<!rec_S>

// LLVM-LABEL: define internal void @__cxx_global_var_init
// LLVM:         %[[CALL:.*]] = call { i64, i32 } @_Z5makeSv()
// LLVM:         store { i64, i32 } %[[CALL]], ptr %[[SLOT:[0-9a-z._]+]], align
// LLVM-CIR:     %[[VAL:.*]] = load %struct.S, ptr %[[SLOT]], align
// LLVM-CIR:     store %struct.S %[[VAL]], ptr @g
// OGCG:         call void @llvm.memcpy.p0.p0.i64(ptr align 4 @g, ptr align 8 %[[SLOT]], i64 12, i1 false)
