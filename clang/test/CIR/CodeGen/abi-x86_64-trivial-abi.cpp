// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

// Verify [[clang::trivial_abi]] types are passed in registers
// (Direct) despite having non-trivial copy/move, matching OGCG.

struct [[clang::trivial_abi]] TrivialABI {
  TrivialABI(const TrivialABI &);
  ~TrivialABI();
  int x;
};

void take_trivial_abi(TrivialABI t) {}
// LLVM: define {{.*}} void @_Z16take_trivial_abi10TrivialABI(i32 %{{.*}})
// OGCG: define {{.*}} void @_Z16take_trivial_abi10TrivialABI(i32 %{{.*}})

TrivialABI ret_trivial_abi(void);
TrivialABI call_trivial_abi() { return ret_trivial_abi(); }
// LLVM: define {{.*}} i32 @_Z16call_trivial_abiv()
// OGCG: define {{.*}} i32 @_Z16call_trivial_abiv()

// Two-int trivial_abi: coerced to i64
struct [[clang::trivial_abi]] TwoInt {
  TwoInt(const TwoInt &);
  ~TwoInt();
  int x, y;
};

void take_two(TwoInt t) {}
// LLVM: define {{.*}} void @_Z8take_two6TwoInt(i64 %{{.*}})
// OGCG: define {{.*}} void @_Z8take_two6TwoInt(i64 %{{.*}})

TwoInt ret_two();
TwoInt call_two() { return ret_two(); }
// LLVM: define {{.*}} i64 @_Z8call_twov()
// OGCG: define {{.*}} i64 @_Z8call_twov()

// Three-int trivial_abi: coerced to i64, i32 (multi-register)
struct [[clang::trivial_abi]] ThreeInt {
  ThreeInt(const ThreeInt &);
  ~ThreeInt();
  int x, y, z;
};

void take_three(ThreeInt t) {}
// LLVM: define {{.*}} void @_Z10take_three8ThreeInt(i64 %{{.*}}, i32 %{{.*}})
// OGCG: define {{.*}} void @_Z10take_three8ThreeInt(i64 %{{.*}}, i32 %{{.*}})

// Without trivial_abi: passed indirectly via pointer
struct NonTrivial {
  NonTrivial(const NonTrivial &);
  ~NonTrivial();
  int x;
};

void take_nontrivial(NonTrivial n) {}
// LLVM: define {{.*}} void @_Z15take_nontrivial10NonTrivial(ptr noundef %{{.*}})
// OGCG: define {{.*}} void @_Z15take_nontrivial10NonTrivial(ptr noundef %{{.*}})
