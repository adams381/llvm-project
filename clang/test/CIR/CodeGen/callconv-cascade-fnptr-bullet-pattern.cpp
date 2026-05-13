// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

struct D { int x; };

int worker(D d) { return d.x; }

struct Inner {
  int (*fp)(D);
  int extra;
};

struct Base {
  Inner inner;
  virtual int handle(Inner *p, D extra) {
    return inner.extra + extra.x + p->fp(extra);
  }
  virtual ~Base() {}
};

struct Derived : Base {
  int handle(Inner *p, D extra) override {
    return p->extra + extra.x + p->fp(extra);
  }
};

int caller(Base *b, Inner *i, int x) {
  D d = {x};
  return b->handle(i, d);
}

// instantiator anchors Derived::handle into the rewrite map so the cascade
// has a record-rename hit to exercise (do not delete).
int instantiator(int x) {
  Derived d;
  Inner i = {nullptr, x};
  Base *b = &d;
  return caller(b, &i, x);
}

// Cascade target: Inner's fp pointee carries the rewritten signature.
// Reaching this state is the regression assertion: without the FuncType
// recursive-conversion fix, the cascade fails to legalize
// cir.vtable.get_virtual_fn_addr above and compilation aborts.
// CIR-DAG: !rec_Inner = !cir.record<struct "Inner" {!cir.ptr<!cir.func<(!s32i) -> !s32i>>, !s32i}>

// CIR-LABEL: cir.func {{.*}}@_Z6worker1D(%arg0: !s32i
// CIR-LABEL: cir.func {{.*}}@_Z6caller
// LLVM:      define {{.*}} i32 @_Z6caller
// OGCG:      define {{.*}} i32 @_Z6caller
