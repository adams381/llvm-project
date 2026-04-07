// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

// Multiple inheritance: the B subobject vtable slot for C::fb uses an Itanium
// non-virtual thunk (this adjustment -8) to reach the C object layout.

struct A {
  virtual int fa() { return 1; }
};
struct B {
  virtual int fb() { return 2; }
};
struct C : A, B {
  int fb() override;
};
int C::fb() { return 3; }

void anchor(C *p) { (void)p->fb(); }

// Vtable slot references the thunk before the thunk body is emitted.
// CIR: #cir.global_view<@_ZThn8_N1C2fbEv>
// CIR: cir.func {{.*}} @_ZThn8_N1C2fbEv
// CIR: cir.const #cir.int<-8>
// CIR: cir.ptr_stride
// CIR: cir.call @_ZN1C2fbEv

// LLVM: @_ZThn8_N1C2fbEv
// LLVM: define {{.*}}@_ZThn8_N1C2fbEv
// LLVM: call {{.*}}@_ZN1C2fbEv

// OGCG: @_ZThn8_N1C2fbEv
// OGCG: define {{.*}}@_ZThn8_N1C2fbEv
// OGCG: call {{.*}}@_ZN1C2fbEv
