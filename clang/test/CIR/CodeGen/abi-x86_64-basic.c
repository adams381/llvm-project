// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

// Comprehensive test for x86_64 System V ABI basic patterns.
// Validates that CIR ABI lowering matches OGCG for scalars,
// struct coercion, sret, byval, unions, and nested structs.

//--- Scalar returns ---

int ret_i32(void) { return 0; }
// LLVM: define {{.*}} i32 @ret_i32()
// OGCG: define {{.*}} i32 @ret_i32()

long long ret_i64(void) { return 0; }
// LLVM: define {{.*}} i64 @ret_i64()
// OGCG: define {{.*}} i64 @ret_i64()

float ret_float(void) { return 0; }
// LLVM: define {{.*}} float @ret_float()
// OGCG: define {{.*}} float @ret_float()

double ret_double(void) { return 0; }
// LLVM: define {{.*}} double @ret_double()
// OGCG: define {{.*}} double @ret_double()

//--- Small struct coercion (8 bytes -> i64) ---

struct Small { int a, b; };

struct Small ret_small(void) { return (struct Small){1, 2}; }
// CIR: cir.func {{.*}} @ret_small()
// LLVM: define {{.*}} i64 @ret_small()
// OGCG: define {{.*}} i64 @ret_small()

void take_small(struct Small s) {}
// LLVM: define {{.*}} void @take_small(i64 %{{.*}})
// OGCG: define {{.*}} void @take_small(i64 %{{.*}})

//--- Two-register struct ({i64, i64}) ---

struct TwoReg { long a, b; };

struct TwoReg ret_tworeg(void) { return (struct TwoReg){1, 2}; }
// LLVM: define {{.*}} { i64, i64 } @ret_tworeg()
// OGCG: define {{.*}} { i64, i64 } @ret_tworeg()

void take_tworeg(struct TwoReg s) {}
// LLVM: define {{.*}} void @take_tworeg(i64 %{{.*}}, i64 %{{.*}})
// OGCG: define {{.*}} void @take_tworeg(i64 %{{.*}}, i64 %{{.*}})

//--- Large struct -> sret/byval (>16 bytes) ---

struct Large { long a, b, c; };

struct Large ret_large(void) { struct Large l = {1,2,3}; return l; }
// LLVM: define {{.*}} void @ret_large(ptr dead_on_unwind noalias writable sret(%struct.Large) align 8 %{{.*}})
// OGCG: define {{.*}} void @ret_large(ptr dead_on_unwind noalias writable sret(%struct.Large) align 8 %{{.*}})

void take_large(struct Large s) {}
// LLVM: define {{.*}} void @take_large(ptr noundef byval(%struct.Large) align 8 %{{.*}})
// OGCG: define {{.*}} void @take_large(ptr noundef byval(%struct.Large) align 8 %{{.*}})

//--- Single float struct ---

struct SingleFloat { float x; };

struct SingleFloat ret_singlefloat(void) { return (struct SingleFloat){1.0f}; }
// LLVM: define {{.*}} float @ret_singlefloat()
// OGCG: define {{.*}} float @ret_singlefloat()

void take_singlefloat(struct SingleFloat s) {}
// LLVM: define {{.*}} void @take_singlefloat(float %{{.*}})
// OGCG: define {{.*}} void @take_singlefloat(float %{{.*}})

//--- Mixed int+float struct (coerced to i64) ---

struct MixedIF { int a; float b; };

struct MixedIF ret_mixedif(void) { return (struct MixedIF){1, 2.0f}; }
// LLVM: define {{.*}} i64 @ret_mixedif()
// OGCG: define {{.*}} i64 @ret_mixedif()

void take_mixedif(struct MixedIF s) {}
// LLVM: define {{.*}} void @take_mixedif(i64 %{{.*}})
// OGCG: define {{.*}} void @take_mixedif(i64 %{{.*}})

//--- Double+long struct (two registers: SSE + INTEGER) ---

struct DblLong { double d; long l; };

struct DblLong ret_dbllong(void) { return (struct DblLong){1.0, 2}; }
// LLVM: define {{.*}} { double, i64 } @ret_dbllong()
// OGCG: define {{.*}} { double, i64 } @ret_dbllong()

void take_dbllong(struct DblLong d) {}
// LLVM: define {{.*}} void @take_dbllong(double %{{.*}}, i64 %{{.*}})
// OGCG: define {{.*}} void @take_dbllong(double %{{.*}}, i64 %{{.*}})

//--- Nested struct ---

struct Inner { int x, y; };
struct Outer { struct Inner in; int z; };

void take_outer(struct Outer o) {}
// LLVM: define {{.*}} void @take_outer(i64 %{{.*}}, i32 %{{.*}})
// OGCG: define {{.*}} void @take_outer(i64 %{{.*}}, i32 %{{.*}})

//--- Array member struct ---

struct ArrMember { int arr[2]; };

void take_arrmember(struct ArrMember a) {}
// LLVM: define {{.*}} void @take_arrmember(i64 %{{.*}})
// OGCG: define {{.*}} void @take_arrmember(i64 %{{.*}})

//--- Union coercion ---

union SmallUnion { int i; float f; };

union SmallUnion ret_smallunion(void) { union SmallUnion u; u.i = 1; return u; }
// LLVM: define {{.*}} i32 @ret_smallunion()
// OGCG: define {{.*}} i32 @ret_smallunion()

void take_smallunion(union SmallUnion u) {}
// LLVM: define {{.*}} void @take_smallunion(i32 %{{.*}})
// OGCG: define {{.*}} void @take_smallunion(i32 %{{.*}})

//--- Single-member struct ---

struct SingleInt { int x; };

struct SingleInt ret_singleint(void) { return (struct SingleInt){42}; }
// LLVM: define {{.*}} i32 @ret_singleint()
// OGCG: define {{.*}} i32 @ret_singleint()
