// XFAIL: *
//
// FIXME: This test currently fails because CallConvLowering flattens
// the callee's struct argument (H -> (i64, ptr) per SysV ABI for a
// 16-byte record fitting in two eightbytes) but does not propagate the
// rewrite to function pointer types stored elsewhere (record fields,
// locals, globals, casts).  Remove the XFAIL once the function-pointer
// cascade lands.
//
// Minimal reproducer for the failure observed in
// llvm-test-suite/MultiSource/Applications/kimwitu++/k.cc.
//
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

struct H {
  unsigned long h;
  const char *s;
};

void worker(H item) { (void)item; }

void caller(unsigned long h, const char *s) {
  void (*fp)(H) = worker;
  H item{h, s};
  fp(item);
}

// After the cascade lands, both `worker` and the function pointer `fp`
// should agree on the SysV ABI flattened signature `(i64, ptr) -> void`.

// CIR-LABEL: cir.func {{.*}} @_Z6worker1H
// LLVM:      define {{.*}} void @_Z6worker1H(i64 {{.*}}, ptr {{.*}})
// OGCG:      define {{.*}} void @_Z6worker1H(i64 {{.*}}, ptr {{.*}})

// CIR-LABEL: cir.func {{.*}} @_Z6callermPKc
// LLVM:      define {{.*}} void @_Z6callermPKc(
// OGCG:      define {{.*}} void @_Z6callermPKc(
