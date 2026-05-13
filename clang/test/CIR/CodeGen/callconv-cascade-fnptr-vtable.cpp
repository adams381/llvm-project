// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

struct D { int x; };

struct Base {
  virtual int handle(D d) { return d.x; }
  virtual ~Base() {}
};

struct Derived : Base {
  int handle(D d) override { return d.x + 1; }
};

int caller(int x) {
  D d = {x};
  Derived obj;
  Base *b = &obj;
  return b->handle(d);
}

// Vtable references the (unmangled-name-invariant) handle symbol; the
// real cascade assertion is the LABEL pair below pinning %arg1: !s32i.
// CIR-DAG: cir.global {{.*}}@_ZTV7Derived = #cir.vtable<{{.*}}@_ZN7Derived6handleE1D{{.*}}>

// CIR-LABEL: cir.func {{.*}}@_Z6calleri
// LLVM:      define {{.*}} i32 @_Z6calleri(i32 {{.*}})
// OGCG:      define {{.*}} i32 @_Z6calleri(i32 {{.*}})

// Cascade target: the rewritten Base::handle signature carries the i32 param
// (instead of D-by-value) for both Base and Derived overrides.
// CIR-LABEL: cir.func {{.*}}@_ZN7Derived6handleE1D(%arg0: !cir.ptr<!rec_Derived>{{.*}}, %arg1: !s32i
// CIR-LABEL: cir.func {{.*}}@_ZN4Base6handleE1D(%arg0: !cir.ptr<!rec_Base>{{.*}}, %arg1: !s32i
// LLVM:      define {{.*}} i32 @_ZN4Base6handleE1D({{.*}}, i32 {{.*}})
// OGCG:      define {{.*}} i32 @_ZN4Base6handleE1D({{.*}}, i32 {{.*}})
