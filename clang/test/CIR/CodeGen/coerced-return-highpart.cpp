// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t.ll %s

// 16-byte struct where the low eightbyte is an empty class (NoClass in
// x86-64 SysV) and the high eightbyte holds the real value.  The
// coerced-return path returns only the high i64 in a single register and
// the caller stores it at byte offset 8 in the destination slot.

struct EmptyLo {};
struct Val { long v; };

struct Ret {
  EmptyLo lo;
  Val hi;
};

Ret make(long x) {
  Ret r;
  r.hi.v = x;
  return r;
}

long take(Ret r) { return r.hi.v; }

long caller() {
  Ret tmp = make(99);
  return take(tmp);
}

// Negative: a 16-byte struct with a double in the low eightbyte is
// classified (SSE, INTEGER) by x86-64 SysV and returned in two
// registers — the coerced-highpart path must not fire.
struct TwoReg { double lo; long hi; };
TwoReg makeTwoReg(long x);
long callTwoReg() {
  TwoReg r = makeTwoReg(42);
  return r.hi;
}

// Coerced return: make() signature uses i64 (the high eightbyte only).
// CIR: cir.func {{.*}} @_Z4makel(%{{.*}}: !s64i
// CIR-SAME: -> !s64i
// CIR: cir.const #cir.int<8> : !u64i
// CIR: cir.ptr_stride
// CIR: cir.load {{.*}} : !cir.ptr<!s64i>, !s64i
// CIR: cir.return {{.*}} : !s64i

// Caller stores the single-register result at offset 8.
// CIR: cir.func {{.*}} @_Z6callerv()
// CIR: cir.call @_Z4makel
// CIR-SAME: -> !s64i
// CIR: cir.const #cir.int<8> : !u64i
// CIR: cir.ptr_stride
// CIR: cir.store {{.*}} : !s64i

// Both CIR and classic codegen agree on the i64 signature and the
// offset-8 store; use a single check prefix for both outputs.
// LLVM: define {{.*}} @_Z4makel(i64
// LLVM: define {{.*}} @_Z6callerv()
// LLVM: call i64 @_Z4makel(i64
// LLVM: getelementptr{{.*}}i8, ptr %{{.*}}, i64 8

// Guard: the double+long struct uses two registers, not a single i64.
// LLVM: call { double, i64 } @_Z10makeTwoRegl(
