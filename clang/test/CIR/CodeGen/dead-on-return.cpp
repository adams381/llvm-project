// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

// Verify dead_on_return attribute on non-byval indirect args.
// Applied when the type is trivially destructible (caller doesn't
// need to destroy it).  NOT applied when the type has a non-trivial
// destructor.

struct Poly {
  virtual void foo();
  int x;
};

struct NTC {
  NTC(const NTC &);
  ~NTC();
  int x;
};

struct PlainDtor {
  ~PlainDtor();
  int x;
};

// Polymorphic, trivially destructible -> dead_on_return
void take_poly(Poly p) {}

// CIR: cir.func {{.*}} @_Z9take_poly4Poly(%{{.*}}: !cir.ptr<!rec_Poly> {llvm.dead_on_return = -1 : i64, llvm.noundef}
// LLVM: define {{.*}} void @_Z9take_poly4Poly(ptr noundef dead_on_return %{{.*}})
// OGCG: define {{.*}} void @_Z9take_poly4Poly(ptr noundef dead_on_return %{{.*}})

// Non-trivial copy + destructor -> no dead_on_return
void take_ntc(NTC n) {}

// CIR: cir.func {{.*}} @_Z8take_ntc3NTC(%{{.*}}: !cir.ptr<!rec_NTC> {llvm.noundef}
// CIR-NOT: dead_on_return
// LLVM: define {{.*}} void @_Z8take_ntc3NTC(ptr noundef %{{.*}})
// LLVM-NOT: dead_on_return
// OGCG: define {{.*}} void @_Z8take_ntc3NTC(ptr noundef %{{.*}})
// OGCG-NOT: dead_on_return

// Explicit destructor -> no dead_on_return
void take_plain_dtor(PlainDtor p) {}

// CIR: cir.func {{.*}} @_Z15take_plain_dtor9PlainDtor(%{{.*}}: !cir.ptr<!rec_PlainDtor> {llvm.noundef}
// CIR-NOT: dead_on_return
// LLVM: define {{.*}} void @_Z15take_plain_dtor9PlainDtor(ptr noundef %{{.*}})
// LLVM-NOT: dead_on_return
// OGCG: define {{.*}} void @_Z15take_plain_dtor9PlainDtor(ptr noundef %{{.*}})
// OGCG-NOT: dead_on_return
