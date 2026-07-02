// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-call-conv-lowering -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-call-conv-lowering -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t.ll %s

typedef struct { long a, b, c, d; } Big;
typedef struct { int x, y; } Pair2;

Big make_big(int);
Pair2 make_pair(int);

typedef Big (*BigFP)(int);
typedef Pair2 (*PairFP)(int);

// Address of an sret-returning function: the get_global result is the rewritten
// function type and is bitcast back to the source pointer type for the return.
BigFP get_big(void) { return make_big; }

// CIR-LABEL: cir.func {{.*}}@get_big() -> !cir.ptr<!cir.func<(!s32i) -> !rec_Big>>
// CIR:         %[[G:.*]] = cir.get_global @make_big : !cir.ptr<!cir.func<(!cir.ptr<!rec_Big>, !s32i)>>
// CIR:         cir.cast bitcast %[[G]] : !cir.ptr<!cir.func<(!cir.ptr<!rec_Big>, !s32i)>> -> !cir.ptr<!cir.func<(!s32i) -> !rec_Big>>

// LLVM-LABEL: define dso_local ptr @get_big()
// LLVM:         ptr @make_big

// Address of a small-struct-returning function: the return is coerced to i64.
PairFP get_pair(void) { return make_pair; }

// CIR-LABEL: cir.func {{.*}}@get_pair() -> !cir.ptr<!cir.func<(!s32i) -> !rec_Pair2>>
// CIR:         %[[G:.*]] = cir.get_global @make_pair : !cir.ptr<!cir.func<(!s32i) -> !u64i>>
// CIR:         cir.cast bitcast %[[G]] : !cir.ptr<!cir.func<(!s32i) -> !u64i>> -> !cir.ptr<!cir.func<(!s32i) -> !rec_Pair2>>

// LLVM-LABEL: define dso_local ptr @get_pair()
// LLVM:         ptr @make_pair
