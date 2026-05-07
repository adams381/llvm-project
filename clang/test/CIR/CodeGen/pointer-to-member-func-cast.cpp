// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -emit-cir -mmlir -mlir-print-ir-before=cir-cxxabi-lowering %s -o %t.cir 2> %t-before.cir
// RUN: FileCheck --check-prefix=CIR-BEFORE --input-file=%t-before.cir %s
// RUN: FileCheck --check-prefix=CIR-AFTER --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --input-file=%t-cir.ll --check-prefix=LLVM %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -emit-llvm %s -o %t.ll
// RUN: FileCheck --input-file=%t.ll --check-prefix=OGCG %s

struct Foo {
  void m1(int);
  virtual void m2(int);
  virtual void m3(int);
};

struct Bar {
  void m4();
};

bool memfunc_to_bool(void (Foo::*func)(int)) {
  return func;
}

// CIR-BEFORE: cir.func {{.*}} @_Z15memfunc_to_boolM3FooFviE
// CIR-BEFORE:   %{{.*}} = cir.cast member_ptr_to_bool %{{.*}} : !cir.method<!cir.func<(!cir.ptr<!rec_Foo>, !s32i)> in !rec_Foo> -> !cir.bool

// The calling-convention lowering pass coerces `!rec_anon_struct {i64,i64}`
// arguments into two separate i64 scalars (matching OGCG's coerce0/coerce1
// convention), then reassembles them in the callee via get_member.
// CIR-AFTER: cir.func {{.*}} @_Z15memfunc_to_boolM3FooFviE(%{{.*}}: !s64i {{.*}}, %{{.*}}: !s64i {{.*}}) -> (!cir.bool
// CIR-AFTER:   {{.*}} = cir.load{{.*}} : !cir.ptr<!rec_anon_struct>, !rec_anon_struct
// CIR-AFTER:   %[[NULL_VAL:.*]] = cir.const #cir.int<0> : !s64i
// CIR-AFTER:   %[[FUNC_PTR:.*]] = cir.extract_member {{.*}}[0] : !rec_anon_struct -> !s64i
// CIR-AFTER:   %[[BOOL_VAL:.*]] = cir.cmp ne %[[FUNC_PTR]], %[[NULL_VAL]] : !s64i

// LLVM: define {{.*}} i1 @_Z15memfunc_to_boolM3FooFviE(i64 %{{.*}}, i64 %{{.*}})
// LLVM:   {{.*}} = load { i64, i64 }, ptr %{{.*}}
// LLVM:   {{.*}} = extractvalue { i64, i64 } {{.*}}, 0
// LLVM:   %{{.*}} = icmp ne i64

// OGCG: define {{.*}} i1 @_Z15memfunc_to_boolM3FooFviE(i64 %{{.*}}, i64 %{{.*}})
// OGCG:   {{.*}} = load { i64, i64 }, ptr %{{.*}}
// OGCG:   {{.*}} = extractvalue { i64, i64 } {{.*}}, 0
// OGCG:   %{{.*}} = icmp ne i64

auto memfunc_reinterpret(void (Foo::*func)(int)) -> void (Bar::*)() {
  return reinterpret_cast<void (Bar::*)()>(func);
}

// CIR-BEFORE: cir.func {{.*}} @_Z19memfunc_reinterpretM3FooFviE
// CIR-BEFORE:   %{{.*}} = cir.cast bitcast %{{.*}} : !cir.method<!cir.func<(!cir.ptr<!rec_Foo>, !s32i)> in !rec_Foo> -> !cir.method<!cir.func<(!cir.ptr<!rec_Bar>)> in !rec_Bar>

// CIR-AFTER: cir.func {{.*}} @_Z19memfunc_reinterpretM3FooFviE(%{{.*}}: !s64i {{.*}}, %{{.*}}: !s64i {{.*}}) -> !rec_anon_struct
// CIR-AFTER:   {{.*}} = cir.load{{.*}} : !cir.ptr<!rec_anon_struct>, !rec_anon_struct
// CIR-AFTER:   {{.*}} = cir.load{{.*}} : !cir.ptr<!rec_anon_struct>, !rec_anon_struct
// CIR-AFTER:   cir.store {{.*}} : !rec_anon_struct, !cir.ptr<!rec_anon_struct>
// CIR-AFTER:   %[[RET:.*]] = cir.load{{.*}} : !cir.ptr<!rec_anon_struct>, !rec_anon_struct
// CIR-AFTER:   cir.return %[[RET]] : !rec_anon_struct

// LLVM: define {{.*}} { i64, i64 } @_Z19memfunc_reinterpretM3FooFviE(i64 %{{.*}}, i64 %{{.*}})
// LLVM:   {{.*}} = load { i64, i64 }, ptr %{{.*}}
// LLVM:   ret { i64, i64 }

// OGCG: define {{.*}} { i64, i64 } @_Z19memfunc_reinterpretM3FooFviE(i64 %{{.*}}, i64 %{{.*}})
// OGCG:   {{.*}} = load { i64, i64 }, ptr %{{.*}}
// OGCG:   ret { i64, i64 }

struct Base1 {
  int x;
  virtual void m1(int);
};

struct Base2 {
  int y;
  virtual void m2(int);
};

struct Derived : Base1, Base2 {
  virtual void m3(int);
};

using Base1MemFunc = void (Base1::*)(int);
using Base2MemFunc = void (Base2::*)(int);
using DerivedMemFunc = void (Derived::*)(int);

DerivedMemFunc base_to_derived_zero_offset(Base1MemFunc ptr) {
  return static_cast<DerivedMemFunc>(ptr);
}

// CIR-BEFORE: cir.func {{.*}} @_Z27base_to_derived_zero_offsetM5Base1FviE
// CIR-BEFORE:   %[[PTR:.*]] = cir.load{{.*}} %{{.*}} : !cir.ptr<!cir.method<!cir.func<(!cir.ptr<!rec_Base1>, !s32i)> in !rec_Base1>>, !cir.method<!cir.func<(!cir.ptr<!rec_Base1>, !s32i)> in !rec_Base1>
// CIR-BEFORE:   %{{.*}} = cir.derived_method %[[PTR]][0] : !cir.method<!cir.func<(!cir.ptr<!rec_Base1>, !s32i)> in !rec_Base1> -> !cir.method<!cir.func<(!cir.ptr<!rec_Derived>, !s32i)> in !rec_Derived>

// CIR-AFTER: cir.func {{.*}} @_Z27base_to_derived_zero_offsetM5Base1FviE(%{{.*}}: !s64i {{.*}}, %{{.*}}: !s64i {{.*}}) -> !rec_anon_struct
// CIR-AFTER:   {{.*}} = cir.load{{.*}} : !cir.ptr<!rec_anon_struct>, !rec_anon_struct
// CIR-AFTER:   {{.*}} = cir.load{{.*}} : !cir.ptr<!rec_anon_struct>, !rec_anon_struct
// CIR-AFTER:   cir.store {{.*}} : !rec_anon_struct, !cir.ptr<!rec_anon_struct>
// CIR-AFTER:   cir.return {{.*}} : !rec_anon_struct

// LLVM: define {{.*}} { i64, i64 } @_Z27base_to_derived_zero_offsetM5Base1FviE(i64 %{{.*}}, i64 %{{.*}})
// LLVM:   {{.*}} = load { i64, i64 }, ptr %{{.*}}
// LLVM:   ret { i64, i64 }

// OGCG: define {{.*}} { i64, i64 } @_Z27base_to_derived_zero_offsetM5Base1FviE(i64 %{{.*}}, i64 %{{.*}})
// OGCG:   {{.*}} = load { i64, i64 }, ptr %{{.*}}
// OGCG:   ret { i64, i64 }

DerivedMemFunc base_to_derived(Base2MemFunc ptr) {
  return static_cast<DerivedMemFunc>(ptr);
}

// CIR-BEFORE: cir.func {{.*}} @_Z15base_to_derivedM5Base2FviE
// CIR-BEFORE:   %[[PTR:.*]] = cir.load{{.*}} %{{.*}} : !cir.ptr<!cir.method<!cir.func<(!cir.ptr<!rec_Base2>, !s32i)> in !rec_Base2>>, !cir.method<!cir.func<(!cir.ptr<!rec_Base2>, !s32i)> in !rec_Base2>
// CIR-BEFORE:   %{{.*}} = cir.derived_method %[[PTR]][16] : !cir.method<!cir.func<(!cir.ptr<!rec_Base2>, !s32i)> in !rec_Base2> -> !cir.method<!cir.func<(!cir.ptr<!rec_Derived>, !s32i)> in !rec_Derived>

// CIR-AFTER: cir.func {{.*}} @_Z15base_to_derivedM5Base2FviE(%{{.*}}: !s64i {{.*}}, %{{.*}}: !s64i {{.*}}) -> !rec_anon_struct
// CIR-AFTER:   {{.*}} = cir.extract_member {{.*}}[1] : !rec_anon_struct -> !s64i
// CIR-AFTER:   %[[OFFSET_ADJ:.*]] = cir.const #cir.int<16> : !s64i
// CIR-AFTER:   %[[BINOP_KIND:.*]] = cir.add nsw {{.*}}, %[[OFFSET_ADJ]] : !s64i
// CIR-AFTER:   {{.*}} = cir.insert_member {{.*}}[1], %[[BINOP_KIND]] : !rec_anon_struct, !s64i

// LLVM: define {{.*}} { i64, i64 } @_Z15base_to_derivedM5Base2FviE(i64 %{{.*}}, i64 %{{.*}})
// LLVM:   {{.*}} = extractvalue { i64, i64 } {{.*}}, 1
// LLVM:   {{.*}} = add nsw i64 {{.*}}, 16
// LLVM:   {{.*}} = insertvalue { i64, i64 } {{.*}}, i64 {{.*}}, 1

// OGCG: define {{.*}} { i64, i64 } @_Z15base_to_derivedM5Base2FviE(i64 %{{.*}}, i64 %{{.*}})
// OGCG:   {{.*}} = extractvalue { i64, i64 } {{.*}}, 1
// OGCG:   {{.*}} = add nsw i64 {{.*}}, 16
// OGCG:   {{.*}} = insertvalue { i64, i64 } {{.*}}, i64 {{.*}}, 1

Base1MemFunc derived_to_base_zero_offset(DerivedMemFunc ptr) {
  return static_cast<Base1MemFunc>(ptr);
}

// CIR-BEFORE: cir.func {{.*}} @_Z27derived_to_base_zero_offsetM7DerivedFviE
// CIR-BEFORE:   %[[PTR:.*]] = cir.load{{.*}} %{{.*}} : !cir.ptr<!cir.method<!cir.func<(!cir.ptr<!rec_Derived>, !s32i)> in !rec_Derived>>, !cir.method<!cir.func<(!cir.ptr<!rec_Derived>, !s32i)> in !rec_Derived>
// CIR-BEFORE:   %{{.*}} = cir.base_method %[[PTR]][0] : !cir.method<!cir.func<(!cir.ptr<!rec_Derived>, !s32i)> in !rec_Derived> -> !cir.method<!cir.func<(!cir.ptr<!rec_Base1>, !s32i)> in !rec_Base1>

// CIR-AFTER: cir.func {{.*}} @_Z27derived_to_base_zero_offsetM7DerivedFviE(%{{.*}}: !s64i {{.*}}, %{{.*}}: !s64i {{.*}}) -> !rec_anon_struct
// CIR-AFTER:   {{.*}} = cir.load{{.*}} : !cir.ptr<!rec_anon_struct>, !rec_anon_struct
// CIR-AFTER:   cir.return {{.*}} : !rec_anon_struct

// LLVM: define {{.*}} { i64, i64 } @_Z27derived_to_base_zero_offsetM7DerivedFviE(i64 %{{.*}}, i64 %{{.*}})
// LLVM:   {{.*}} = load { i64, i64 }, ptr %{{.*}}
// LLVM:   ret { i64, i64 }

// OGCG: define {{.*}} { i64, i64 } @_Z27derived_to_base_zero_offsetM7DerivedFviE(i64 %{{.*}}, i64 %{{.*}})
// OGCG:   {{.*}} = load { i64, i64 }, ptr %{{.*}}
// OGCG:   ret { i64, i64 }

Base2MemFunc derived_to_base(DerivedMemFunc ptr) {
  return static_cast<Base2MemFunc>(ptr);
}

// CIR-BEFORE: cir.func {{.*}} @_Z15derived_to_baseM7DerivedFviE
// CIR-BEFORE:   %[[PTR:.*]] = cir.load{{.*}} %{{.*}} : !cir.ptr<!cir.method<!cir.func<(!cir.ptr<!rec_Derived>, !s32i)> in !rec_Derived>>, !cir.method<!cir.func<(!cir.ptr<!rec_Derived>, !s32i)> in !rec_Derived>
// CIR-BEFORE:   %{{.*}} = cir.base_method %[[PTR]][16] : !cir.method<!cir.func<(!cir.ptr<!rec_Derived>, !s32i)> in !rec_Derived> -> !cir.method<!cir.func<(!cir.ptr<!rec_Base2>, !s32i)> in !rec_Base2>

// CIR-AFTER: cir.func {{.*}} @_Z15derived_to_baseM7DerivedFviE(%{{.*}}: !s64i {{.*}}, %{{.*}}: !s64i {{.*}}) -> !rec_anon_struct
// CIR-AFTER:   {{.*}} = cir.extract_member {{.*}}[1] : !rec_anon_struct -> !s64i
// CIR-AFTER:   %[[OFFSET_ADJ:.*]] = cir.const #cir.int<16> : !s64i
// CIR-AFTER:   %[[BINOP_KIND:.*]] = cir.sub nsw {{.*}}, %[[OFFSET_ADJ]] : !s64i
// CIR-AFTER:   {{.*}} = cir.insert_member {{.*}}[1], %[[BINOP_KIND]] : !rec_anon_struct, !s64i

// LLVM: define {{.*}} { i64, i64 } @_Z15derived_to_baseM7DerivedFviE(i64 %{{.*}}, i64 %{{.*}})
// LLVM:   {{.*}} = extractvalue { i64, i64 } {{.*}}, 1
// LLVM:   {{.*}} = sub nsw i64 {{.*}}, 16
// LLVM:   {{.*}} = insertvalue { i64, i64 } {{.*}}, i64 {{.*}}, 1

// OGCG: define {{.*}} { i64, i64 } @_Z15derived_to_baseM7DerivedFviE(i64 %{{.*}}, i64 %{{.*}})
// OGCG:   {{.*}} = extractvalue { i64, i64 } {{.*}}, 1
// OGCG:   {{.*}} = sub nsw i64 {{.*}}, 16
// OGCG:   {{.*}} = insertvalue { i64, i64 } {{.*}}, i64 {{.*}}, 1
