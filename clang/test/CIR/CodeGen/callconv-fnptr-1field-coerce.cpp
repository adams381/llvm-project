// XFAIL: *
//
// FIXME: This test currently fails because CallConvLowering rewrites the
// callee's signature (D -> i32 per SysV ABI for a 1-field record) but
// does not propagate the rewrite to function pointer types stored
// elsewhere (record fields, locals, globals, casts).  Remove the XFAIL
// once the function-pointer cascade lands.
//
// Minimal reproducer for the failure observed in
// llvm-test-suite/MultiSource/Benchmarks/Prolangs-C++/city/intersection.cpp.
//
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

struct D { int x; };

int worker(D d) { return d.x; }

int caller(int x) {
  int (*fp)(D) = worker;
  D d = {x};
  return fp(d);
}

// After the cascade lands, both `worker` and the function pointer `fp`
// should agree on the SysV ABI signature `(i32) -> i32`.

// CIR-LABEL: cir.func {{.*}} @_Z6worker1D
// LLVM:      define {{.*}} i32 @_Z6worker1D(i32 {{.*}})
// OGCG:      define {{.*}} i32 @_Z6worker1D(i32 {{.*}})

// CIR-LABEL: cir.func {{.*}} @_Z6calleri
// LLVM:      define {{.*}} i32 @_Z6calleri(i32 {{.*}})
// OGCG:      define {{.*}} i32 @_Z6calleri(i32 {{.*}})
