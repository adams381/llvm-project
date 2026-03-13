// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

// Verify this-pointer attributes on destructors:
// - noundef, nonnull, dereferenceable(N), align on all destructors
// - dead_on_return(N) only on destructors with no base classes

void opaque();
void opaque(char *);

class Foo {
public:
  ~Foo();

private:
  char FooVar[4];
};

Foo::~Foo() { opaque(FooVar); }

// CIR: cir.func {{.*}} @_ZN3FooD2Ev(%arg0: !cir.ptr<!rec_Foo>
// CIR-SAME: {llvm.align = 1 : i64, llvm.dead_on_return = 4 : i64, llvm.dereferenceable = 4 : i64, llvm.nonnull, llvm.noundef}

// LLVM: define {{.*}} void @_ZN3FooD2Ev(ptr noundef nonnull align 1 dead_on_return(4) dereferenceable(4) %{{.*}})
// OGCG: define {{.*}} void @_ZN3FooD2Ev(ptr noundef nonnull align 1 dead_on_return(4) dereferenceable(4) %{{.*}})

class Bar {
public:
  ~Bar();

private:
  char BarVar[8];
};

Bar::~Bar() { opaque(BarVar); }

// CIR: cir.func {{.*}} @_ZN3BarD2Ev(%arg0: !cir.ptr<!rec_Bar>
// CIR-SAME: {llvm.align = 1 : i64, llvm.dead_on_return = 8 : i64, llvm.dereferenceable = 8 : i64, llvm.nonnull, llvm.noundef}

// LLVM: define {{.*}} void @_ZN3BarD2Ev(ptr noundef nonnull align 1 dead_on_return(8) dereferenceable(8) %{{.*}})
// OGCG: define {{.*}} void @_ZN3BarD2Ev(ptr noundef nonnull align 1 dead_on_return(8) dereferenceable(8) %{{.*}})

class BarInheritsFoo : public Foo {
public:
  ~BarInheritsFoo();

private:
  char BarVar[12];
};

// Has bases, so no dead_on_return.
BarInheritsFoo::~BarInheritsFoo() { opaque(BarVar); }

// CIR: cir.func {{.*}} @_ZN14BarInheritsFooD2Ev(%arg0: !cir.ptr<!rec_BarInheritsFoo>
// CIR-SAME: {llvm.align = 1 : i64, llvm.dereferenceable = 16 : i64, llvm.nonnull, llvm.noundef}

// LLVM: define {{.*}} void @_ZN14BarInheritsFooD2Ev(ptr noundef nonnull align 1 dereferenceable(16) %{{.*}})
// OGCG: define {{.*}} void @_ZN14BarInheritsFooD2Ev(ptr noundef nonnull align 1 dereferenceable(16) %{{.*}})

class BarVirtualInheritsFoo : public virtual Foo {
public:
  ~BarVirtualInheritsFoo();

private:
  char BarVar[16];
};

// Has virtual bases, so no dead_on_return.
BarVirtualInheritsFoo::~BarVirtualInheritsFoo() { opaque(BarVar); }

// CIR: cir.func {{.*}} @_ZN21BarVirtualInheritsFooD2Ev(%arg0: !cir.ptr<!rec_BarVirtualInheritsFoo>
// CIR-SAME: {llvm.align = 8 : i64, llvm.dereferenceable = 24 : i64, llvm.nonnull, llvm.noundef}

// LLVM: define {{.*}} void @_ZN21BarVirtualInheritsFooD2Ev(ptr noundef nonnull align 8 dereferenceable(24) %{{.*}})
// OGCG: define {{.*}} void @_ZN21BarVirtualInheritsFooD2Ev(ptr noundef nonnull align 8 dereferenceable(24) %{{.*}})

class Empty {
public:
  ~Empty();
};

Empty::~Empty() { opaque(); }

// CIR: cir.func {{.*}} @_ZN5EmptyD2Ev(%arg0: !cir.ptr<!rec_Empty>
// CIR-SAME: {llvm.align = 1 : i64, llvm.dereferenceable = 1 : i64, llvm.nonnull, llvm.noundef}

// LLVM: define {{.*}} void @_ZN5EmptyD2Ev(ptr noundef nonnull align 1 dereferenceable(1) %{{.*}})
// OGCG: define {{.*}} void @_ZN5EmptyD2Ev(ptr noundef nonnull align 1 dereferenceable(1) %{{.*}})
