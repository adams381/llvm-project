// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --input-file=%t.cir %s -check-prefix=CIR
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -Wno-unused-value -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --input-file=%t-cir.ll %s -check-prefix=LLVM
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -Wno-unused-value -emit-llvm %s -o %t.ll
// RUN: FileCheck --input-file=%t.ll %s -check-prefix=OGCG

struct S {
  int x;
  int y;
};

void f1(struct S);
void f2(void) {
  struct S s;
  f1(s);
}

// `S { int x; int y; }` is 8 bytes (single eightbyte INTEGER class on
// x86_64 SysV).  CallConvLowering coerces the by-value argument into a
// single `i64` eightbyte, matching OGCG.  In CIR the caller stores the
// record into a coerce alloca and loads the eightbyte through a bitcast
// before passing it to `@f1`.
// CIR-LABEL: cir.func{{.*}} @f2(){{.*}} {
// CIR:         %[[S:.+]] = cir.load align(4) %{{.+}} : !cir.ptr<!rec_S>, !rec_S
// CIR:         %[[COERCE:.+]] = cir.alloca !rec_S, !cir.ptr<!rec_S>, ["coerce"]
// CIR:         cir.store %[[S]], %[[COERCE]] : !rec_S, !cir.ptr<!rec_S>
// CIR:         %[[CAST:.+]] = cir.cast bitcast %[[COERCE]] : !cir.ptr<!rec_S> -> !cir.ptr<!u64i>
// CIR:         %[[I:.+]] = cir.load %[[CAST]] : !cir.ptr<!u64i>, !u64i
// CIR:         cir.call @f1(%[[I]]) : (!u64i) -> ()

// LLVM-LABEL: define{{.*}} void @f2(){{.*}}
// LLVM:         %[[I:.+]] = load i64, ptr %{{.+}}, align 8
// LLVM:         call void @f1(i64 %[[I]])

// OGCG-LABEL: define{{.*}} void @f2()
// OGCG:         %[[S:.+]] = load i64, ptr %{{.+}}, align 4
// OGCG-NEXT:    call void @f1(i64 %[[S]])

struct S f3(void);
void f4(void) {
  struct S s = f3();
}

// `f3()` also has its return coerced to `i64` matching OGCG.  The
// caller stores the eightbyte into a coerce alloca and reloads the
// record through a bitcast before storing into `s`.
// CIR-LABEL: cir.func{{.*}} @f4(){{.*}} {
// CIR:         %[[I:.+]] = cir.call @f3() : () -> !u64i
// CIR-NEXT:    %[[COERCE:.+]] = cir.alloca !u64i, !cir.ptr<!u64i>, ["coerce"]
// CIR-NEXT:    cir.store %[[I]], %[[COERCE]] : !u64i, !cir.ptr<!u64i>
// CIR-NEXT:    %[[CAST:.+]] = cir.cast bitcast %[[COERCE]] : !cir.ptr<!u64i> -> !cir.ptr<!rec_S>
// CIR-NEXT:    %[[S:.+]] = cir.load %[[CAST]] : !cir.ptr<!rec_S>, !rec_S
// CIR-NEXT:    cir.store align(4) %[[S]], %{{.+}} : !rec_S, !cir.ptr<!rec_S>

// LLVM-LABEL: define{{.*}} void @f4(){{.*}} {
// LLVM:         %[[I:.+]] = call i64 @f3()
// LLVM-NEXT:    %[[COERCE:.+]] = alloca i64
// LLVM-NEXT:    store i64 %[[I]], ptr %[[COERCE]]
// LLVM-NEXT:    %[[S:.+]] = load %struct.S, ptr %[[COERCE]]
// LLVM-NEXT:    store %struct.S %[[S]], ptr %{{.+}}

// OGCG-LABEL: define{{.*}} void @f4() #0 {
// OGCG:         %[[S:.+]] = call i64 @f3()
// OGCG-NEXT:    store i64 %[[S]], ptr %{{.+}}, align 4

struct Big {
  int data[10];
};

void f5(struct Big);
struct Big f6(void);

void f7(void) {
  struct Big b;
  f5(b);
}

// `Big` is 40 bytes — too large to pass in registers, so it goes byval
// (matching classic Clang's `byval(%struct.Big)`).  The CallConvLowering
// pass converts the by-value arg to a `!cir.ptr<!rec_Big>` argument.
// CIR-LABEL: cir.func{{.*}} @f7(){{.*}} {
// CIR:         cir.call @f5(%{{.+}}) : (!cir.ptr<!rec_Big> {{{.*}}llvm.byval = !rec_Big{{.*}}}) -> ()

// LLVM-LABEL: define{{.*}} void @f7(){{.*}} {
// LLVM:         call void @f5(ptr noundef byval(%struct.Big) align 8 %{{.+}})

// OGCG-LABEL: define{{.*}} void @f7() #0 {
// OGCG:         %[[B:.+]] = alloca %struct.Big, align 8
// OGCG-NEXT:    call void @f5(ptr noundef byval(%struct.Big) align 8 %[[B]])

void f8(void) {
  struct Big b = f6();
}

// `Big` return goes via sret (matches classic Clang's
// `sret(%struct.Big)`).  The pass rewrites the call to take an sret
// argument; the result is stored directly into the destination by the
// callee, so no return-value handling is needed in the caller.
// CIR-LABEL: cir.func{{.*}} @f8(){{.*}} {
// CIR:         cir.call @f6(%{{.+}}) : (!cir.ptr<!rec_Big>) -> ()

// LLVM-LABEL: define{{.*}} void @f8(){{.*}} {
// LLVM:        call void @f6(ptr %{{.+}})

// OGCG-LABEL: define{{.*}} void @f8() #0 {
// OGCG:         %[[B:.+]] = alloca %struct.Big, align 4
// OGCG-NEXT:    call void @f6(ptr dead_on_unwind writable sret(%struct.Big) align 4 %[[B]])

void f9(void) {
  f1(f3());
}

// f1 takes a single `i64` eightbyte arg (see f2 above) and f3 returns S
// coerced to `i64` (matching OGCG).  CIR forwards the eightbyte value
// straight from `f3()` to `f1(...)`.
// CIR-LABEL: cir.func{{.*}} @f9(){{.*}} {
// CIR:         %[[SLOT:.+]] = cir.alloca !rec_S, !cir.ptr<!rec_S>, ["agg.tmp0"] {alignment = 4 : i64}
// CIR:         %[[RET:.+]] = cir.call @f3() : () -> !u64i
// CIR:         cir.call @f1(%{{.+}}) : (!u64i) -> ()

// LLVM-LABEL: define{{.*}} void @f9(){{.*}} {
// LLVM:         %[[SLOT:.+]] = alloca %struct.S, i64 1, align 4
// LLVM:         %[[RET:.+]] = call i64 @f3()
// LLVM:         call void @f1(i64 %{{.+}})

// OGCG-LABEL: define{{.*}} void @f9() #0 {
// OGCG:         %[[SLOT:.+]] = alloca %struct.S, align 4
// OGCG-NEXT:    %[[RET:.+]] = call i64 @f3()
// OGCG-NEXT:    store i64 %[[RET]], ptr %[[SLOT]], align 4
// OGCG-NEXT:    %[[ARG:.+]] = load i64, ptr %[[SLOT]], align 4
// OGCG-NEXT:    call void @f1(i64 %[[ARG]])

__attribute__((pure)) int f10(int);
__attribute__((const)) int f11(int);
int f12(void) {
  return f10(1) + f11(2);
}

// CIR-LABEL: cir.func{{.*}} @f12() -> !s32i{{.*}} {
// CIR:         %[[A:.+]] = cir.const #cir.int<1> : !s32i
// CIR-NEXT:    %{{.+}} = cir.call @f10(%[[A]]) side_effect(pure) : (!s32i {llvm.noundef}) -> !s32i
// CIR-NEXT:    %[[B:.+]] = cir.const #cir.int<2> : !s32i
// CIR-NEXT:    %{{.+}} = cir.call @f11(%[[B]]) side_effect(const) : (!s32i {llvm.noundef}) -> !s32i

// LLVM-LABEL: define{{.*}} i32 @f12(){{.*}}
// LLVM:         %{{.+}} = call i32 @f10(i32 noundef 1) #[[ATTR0:.+]]
// LLVM-NEXT:    %{{.+}} = call i32 @f11(i32 noundef 2) #[[ATTR1:.+]]

// OGCG-LABEL: define{{.*}} i32 @f12()
// OGCG:         %{{.+}} = call i32 @f10(i32 noundef 1) #[[ATTR0:.+]]
// OGCG-NEXT:    %{{.+}} = call i32 @f11(i32 noundef 2) #[[ATTR1:.+]]

// LLVM: attributes #[[ATTR0]] = { nounwind willreturn memory(read) }
// LLVM: attributes #[[ATTR1]] = { nounwind willreturn memory(none) }

// OGCG: attributes #[[ATTR0]] = { nounwind willreturn memory(read) }
// OGCG: attributes #[[ATTR1]] = { nounwind willreturn memory(none) }
