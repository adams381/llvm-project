// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

struct S {
  int x;
  S();
  S(const S &);
  S &operator=(const S &);
};

S make_s();

S agg_invoker() {
  auto *fn = +[](int i) -> S { return make_s(); };
  return fn(3);
}

// CallConvLowering now rewrites the by-value aggregate return into an sret arg
// at the CIR level (matching the x86_64 ABI that OGCG already uses).
// CIR-LABEL: cir.func no_inline internal private dso_local @_ZZ11agg_invokervEN3$_08__invokeEi
// CIR-SAME:    (%[[SRET:.*]]: !cir.ptr<!rec_S> {{.*}}llvm.sret{{.*}}, %[[I_ARG:.*]]: !s32i {{.*}})
// CIR:         %[[I_ALLOCA:.*]] = cir.alloca !s32i, !cir.ptr<!s32i>, ["i", init]
// CIR:         %[[UNUSED:.*]] = cir.alloca !rec_anon{{[^,]*}}, {{.*}} ["unused.capture"]
// CIR-NOT:     cir.alloca {{.*}} ["agg.tmp
// CIR-NOT:     cir.alloca {{.*}} ["__retval"]
// CIR:         cir.store %[[I_ARG]], %[[I_ALLOCA]]
// CIR:         %[[I:.*]] = cir.load{{.*}} %[[I_ALLOCA]]
// CIR:         cir.call @_ZZ11agg_invokervENK3$_0clEi(%[[SRET]], %[[UNUSED]], %[[I]])
// CIR-NOT:     cir.copy
// CIR:         cir.return

// LLVM-LABEL: define internal void @"_ZZ11agg_invokervEN3$_08__invokeEi"
// LLVM-SAME:    (ptr {{.*}} sret(%struct.S) {{[^,]*}} %[[AGG_RESULT:[^,)]+]], i32 {{[^,)]*}} %[[I_ARG:[^,)]+]])
// LLVM:         %[[I_ALLOCA:.*]] = alloca i32
// LLVM:         %[[UNUSED:.*]] = alloca %class.anon
// LLVM:         store i32 %[[I_ARG]], ptr %[[I_ALLOCA]]
// LLVM:         %[[I:.*]] = load i32, ptr %[[I_ALLOCA]]
// LLVM:         call void @"_ZZ11agg_invokervENK3$_0clEi"(ptr {{.*}} sret(%struct.S) {{[^,]*}} %[[AGG_RESULT]], ptr {{.*}} %[[UNUSED]], i32 {{.*}} %[[I]])
// LLVM:         ret void

// OGCG-LABEL: define internal void @"_ZZ11agg_invokervEN3$_08__invokeEi"
// OGCG-SAME:    (ptr {{.*}} sret(%struct.S) {{[^,]*}} %[[AGG_RESULT:[^,]+]], i32 {{[^,)]*}} %[[I_ARG:[^,)]+]])
// OGCG:         call void @"_ZZ11agg_invokervENK3$_0clEi"(ptr {{.*}} sret(%struct.S) {{[^,]*}} %[[AGG_RESULT]], ptr {{.*}} %[[UNUSED:.*]], i32 {{.*}})
// OGCG:         ret void
