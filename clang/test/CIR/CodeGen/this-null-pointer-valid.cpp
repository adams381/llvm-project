// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fno-delete-null-pointer-checks -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fno-delete-null-pointer-checks -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fno-delete-null-pointer-checks -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

// When -fno-delete-null-pointer-checks is passed, the this pointer
// gets dereferenceable_or_null instead of nonnull + dereferenceable.

struct Struct {
  int many, member, fields;
  void returnsVoid();
};

void Struct::returnsVoid() {}

// CIR: cir.func {{.*}} @_ZN6Struct11returnsVoidEv(%arg0: !cir.ptr<!rec_Struct>
// CIR-SAME: {llvm.align = 4 : i64, llvm.dereferenceable_or_null = 12 : i64, llvm.noundef}
// CIR-NOT: llvm.nonnull

// LLVM: define {{.*}} void @_ZN6Struct11returnsVoidEv(ptr noundef align 4 dereferenceable_or_null(12) %{{.*}})
// LLVM-NOT: nonnull
// OGCG: define {{.*}} void @_ZN6Struct11returnsVoidEv(ptr noundef align 4 dereferenceable_or_null(12) %{{.*}})

struct Small {
  ~Small();
  int x;
};

Small::~Small() {}

// Destructor with no bases: dereferenceable_or_null + dead_on_return.
// CIR: cir.func {{.*}} @_ZN5SmallD2Ev(%arg0: !cir.ptr<!rec_Small>
// CIR-SAME: {llvm.align = 4 : i64, llvm.dead_on_return = 4 : i64, llvm.dereferenceable_or_null = 4 : i64, llvm.noundef}
// CIR-NOT: llvm.nonnull

// LLVM: define {{.*}} void @_ZN5SmallD2Ev(ptr noundef align 4 dead_on_return(4) dereferenceable_or_null(4) %{{.*}})
// LLVM-NOT: nonnull
// OGCG: define {{.*}} void @_ZN5SmallD2Ev(ptr noundef align 4 dead_on_return(4) dereferenceable_or_null(4) %{{.*}})
