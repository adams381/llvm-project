// RUN: %clang_cc1 -std=c++11 -triple aarch64-none-linux-android21 -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s

struct Flub {
  int a = 123;
};

struct Foo {
  int a;

  Foo() : a(123) {}
  Foo(const Foo &other) : a(other.a) {}
  Foo(Foo &&other) noexcept : a(other.a) { other.a = 0; }

  Foo &operator=(const Foo &other) {
    if (this != &other) {
      a = other.a;
    }
    return *this;
  }

  Foo &operator=(Foo &&other) noexcept {
    if (this != &other) {
      a = other.a;
      other.a = 0;
    }
    return *this;
  }

  ~Foo();
};

void trivial_func() {
  Flub f1{};

  Flub f2 = f1;
  // Trivial copy constructors/assignments are replaced with cir.copy
  // CIR: cir.copy {{.*}} : !cir.ptr<!rec_Flub>

  Flub f3 = static_cast<Flub&&>(f1);
  // CIR: @_ZN4FlubC1EOS_(%arg0: !cir.ptr<!rec_Flub> {llvm.align = 4 : i64, llvm.dereferenceable = 4 : i64, llvm.nonnull, llvm.noundef} loc({{.*}}), %arg1: !cir.ptr<!rec_Flub> {llvm.align = 4 : i64, llvm.dereferenceable = 4 : i64, llvm.nonnull, llvm.noundef} loc({{.*}})) special_member<#cir.cxx_ctor<!rec_Flub, move, trivial true>

  f2 = f1;
  // CIR: @_ZN4FlubaSERKS_(%arg0: !cir.ptr<!rec_Flub> {llvm.align = 4 : i64, llvm.dereferenceable = 4 : i64, llvm.nonnull, llvm.noundef} loc({{.*}}), %arg1: !cir.ptr<!rec_Flub> {llvm.align = 4 : i64, llvm.dereferenceable = 4 : i64, llvm.nonnull, llvm.noundef} loc({{.*}})) -> (!cir.ptr<!rec_Flub> {llvm.align = 4 : i64, llvm.dereferenceable = 4 : i64, llvm.nonnull, llvm.noundef}) special_member<#cir.cxx_assign<!rec_Flub, copy, trivial true>>

  f1 = static_cast<Flub&&>(f3);
  // CIR: @_ZN4FlubaSEOS_(%arg0: !cir.ptr<!rec_Flub> {llvm.align = 4 : i64, llvm.dereferenceable = 4 : i64, llvm.nonnull, llvm.noundef} loc({{.*}}), %arg1: !cir.ptr<!rec_Flub> {llvm.align = 4 : i64, llvm.dereferenceable = 4 : i64, llvm.nonnull, llvm.noundef} loc({{.*}})) -> (!cir.ptr<!rec_Flub> {llvm.align = 4 : i64, llvm.dereferenceable = 4 : i64, llvm.nonnull, llvm.noundef}) special_member<#cir.cxx_assign<!rec_Flub, move, trivial true>>
}

void non_trivial_func() {
  Foo f1{};
  // CIR: @_ZN3FooC2Ev(%arg0: !cir.ptr<!rec_Foo> {llvm.align = 4 : i64, llvm.dereferenceable = 4 : i64, llvm.nonnull, llvm.noundef} loc({{.*}})) special_member<#cir.cxx_ctor<!rec_Foo, default>>

  Foo f2 = f1;
  // CIR: @_ZN3FooC2ERKS_(%arg0: !cir.ptr<!rec_Foo> {llvm.align = 4 : i64, llvm.dereferenceable = 4 : i64, llvm.nonnull, llvm.noundef} loc({{.*}}), %arg1: !cir.ptr<!rec_Foo> {llvm.align = 4 : i64, llvm.dereferenceable = 4 : i64, llvm.nonnull, llvm.noundef} loc({{.*}})) special_member<#cir.cxx_ctor<!rec_Foo, copy>>

  Foo f3 = static_cast<Foo&&>(f1);
  // CIR: @_ZN3FooC2EOS_(%arg0: !cir.ptr<!rec_Foo> {llvm.align = 4 : i64, llvm.dereferenceable = 4 : i64, llvm.nonnull, llvm.noundef} loc({{.*}}), %arg1: !cir.ptr<!rec_Foo> {llvm.align = 4 : i64, llvm.dereferenceable = 4 : i64, llvm.nonnull, llvm.noundef} loc({{.*}})) special_member<#cir.cxx_ctor<!rec_Foo, move>>

  f2 = f1;
  // CIR: @_ZN3FooaSERKS_(%arg0: !cir.ptr<!rec_Foo> {llvm.align = 4 : i64, llvm.dereferenceable = 4 : i64, llvm.nonnull, llvm.noundef} loc({{.*}}), %arg1: !cir.ptr<!rec_Foo> {llvm.align = 4 : i64, llvm.dereferenceable = 4 : i64, llvm.nonnull, llvm.noundef} loc({{.*}})) -> (!cir.ptr<!rec_Foo> {llvm.align = 4 : i64, llvm.dereferenceable = 4 : i64, llvm.nonnull, llvm.noundef}) special_member<#cir.cxx_assign<!rec_Foo, copy>>

  f1 = static_cast<Foo&&>(f3);
  // CIR: @_ZN3FooaSEOS_(%arg0: !cir.ptr<!rec_Foo> {llvm.align = 4 : i64, llvm.dereferenceable = 4 : i64, llvm.nonnull, llvm.noundef} loc({{.*}}), %arg1: !cir.ptr<!rec_Foo> {llvm.align = 4 : i64, llvm.dereferenceable = 4 : i64, llvm.nonnull, llvm.noundef} loc({{.*}})) -> (!cir.ptr<!rec_Foo> {llvm.align = 4 : i64, llvm.dereferenceable = 4 : i64, llvm.nonnull, llvm.noundef}) special_member<#cir.cxx_assign<!rec_Foo, move>>
  // CIR: @_ZN3FooD1Ev(!cir.ptr<!rec_Foo> {llvm.align = 4 : i64, llvm.dead_on_return = 4 : i64, llvm.dereferenceable = 4 : i64, llvm.nonnull, llvm.noundef}) special_member<#cir.cxx_dtor<!rec_Foo>>
}
