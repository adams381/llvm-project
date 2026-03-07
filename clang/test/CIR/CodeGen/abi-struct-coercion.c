// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --input-file=%t.cir %s -check-prefix=CIR
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --input-file=%t-cir.ll %s -check-prefix=LLVM
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --input-file=%t.ll %s -check-prefix=OGCG

// Verify that trivially-copyable struct return values are correctly coerced
// to register-width integers per the x86_64 System V ABI.  This is the
// "div_t" pattern: an 8-byte struct (two i32s) must be returned as i64.

typedef struct {
  int quot;
  int rem;
} div_t;

div_t do_div(int a, int b);

div_t call_div(int a, int b) {
  return do_div(a, b);
}

// CIR-LABEL: cir.func{{.*}} @call_div
// CIR:         cir.call @do_div({{.*}}) : (!s32i, !s32i) -> !u64i
// CIR:         cir.return %{{.+}} : !u64i

// LLVM-LABEL: define{{.*}} i64 @call_div(i32 %{{.*}}, i32 %{{.*}})
// LLVM:         %{{.+}} = call i64 @do_div(i32 %{{.*}}, i32 %{{.*}})
// LLVM:         ret i64 %{{.+}}

// OGCG-LABEL: define{{.*}} i64 @call_div(i32 {{.*}}, i32 {{.*}})
// OGCG:         %{{.+}} = call i64 @do_div(i32 {{.*}}, i32 {{.*}})
// OGCG:         ret i64 %{{.+}}

int get_quot(int a, int b) {
  div_t result = do_div(a, b);
  return result.quot;
}

// CIR-LABEL: cir.func{{.*}} @get_quot
// CIR:         cir.call @do_div({{.*}}) : (!s32i, !s32i) -> !u64i
// CIR:         cir.get_member %{{.+}}[0] {name = "quot"}

// LLVM-LABEL: define{{.*}} i32 @get_quot(i32 %{{.*}}, i32 %{{.*}})
// LLVM:         %{{.+}} = call i64 @do_div(i32 %{{.*}}, i32 %{{.*}})
// LLVM:         %{{.+}} = getelementptr %struct.div_t, ptr %{{.*}}, i32 0, i32 0

// OGCG-LABEL: define{{.*}} i32 @get_quot(i32 {{.*}}, i32 {{.*}})
// OGCG:         %{{.+}} = call i64 @do_div(i32 {{.*}}, i32 {{.*}})
// OGCG:         %{{.+}} = getelementptr inbounds nuw %struct.div_t, ptr %{{.*}}, i32 0, i32 0

// Verify that large structs (>16 bytes) are returned via sret pointer
// per the x86_64 System V ABI.

typedef struct {
  int a, b, c, d, e;
} Big;

Big make_big(int x);

Big call_make_big(int x) {
  return make_big(x);
}

// CIR-LABEL: cir.func{{.*}} @call_make_big
// CIR-SAME:    %{{.+}}: !cir.ptr<!rec_Big> {llvm.align = 4 : i64, llvm.sret = !rec_Big}
// CIR:         cir.call @make_big(%{{.+}}, %{{.+}}) : (!cir.ptr<!rec_Big>, !s32i) -> ()
// CIR:         cir.store %{{.+}}, %arg0 : !rec_Big
// CIR:         cir.return

// LLVM-LABEL: define{{.*}} void @call_make_big(ptr sret(%struct.Big) align 4 %{{.*}}, i32 %{{.*}})
// LLVM:         call void @make_big(ptr %{{.*}}, i32 %{{.*}})
// LLVM:         ret void

// OGCG-LABEL: define{{.*}} void @call_make_big(ptr {{.*}} sret(%struct.Big) align 4 %{{.*}}, i32 {{.*}})
// OGCG:         call void @make_big(ptr {{.*}} sret(%struct.Big) align 4 %{{.*}}, i32 {{.*}})
// OGCG:         ret void
