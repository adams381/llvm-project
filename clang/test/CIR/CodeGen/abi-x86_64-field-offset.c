// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

// Verify that struct field offsets account for alignment padding
// during ABI classification.  Mixed-type structs between 9-16
// bytes should be passed in two registers, not via byval/sret.

// float (SSE) + unsigned long (INTEGER) = two registers
struct FloatLong { float f; unsigned long l; };

void take_fl(struct FloatLong s) {}
// LLVM: define {{.*}} void @take_fl(float %{{.*}}, i64 %{{.*}})
// OGCG: define {{.*}} void @take_fl(float %{{.*}}, i64 %{{.*}})

struct FloatLong ret_fl(void) {
  struct FloatLong s = {0};
  return s;
}
// LLVM: define {{.*}} { float, i64 } @ret_fl()
// OGCG: define {{.*}} { float, i64 } @ret_fl()

// int (INTEGER) + double (SSE) = two registers
struct IntDouble { int i; double d; };

void take_id(struct IntDouble s) {}
// LLVM: define {{.*}} void @take_id(i32 %{{.*}}, double %{{.*}})
// OGCG: define {{.*}} void @take_id(i32 %{{.*}}, double %{{.*}})

// short + long long = two INTEGER registers
struct ShortLL { short s; long long l; };

void take_sl(struct ShortLL s) {}
// LLVM: define {{.*}} void @take_sl(i16 %{{.*}}, i64 %{{.*}})
// OGCG: define {{.*}} void @take_sl(i16 %{{.*}}, i64 %{{.*}})
