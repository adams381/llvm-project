// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t.ll %s

// Every member of the nested union is a float pair, so the eightbyte stays a
// float vector.
struct FF {
  float a, b;
};
struct GG {
  float c, d;
};
union AllFloat {
  struct FF f;
  struct GG g;
};
struct SFloat {
  union AllFloat u;
  int i;
};
void take_all_float(struct SFloat s);

// Here the double is the storage member, so the same eightbyte is a double
// rather than the float pair one of the members would give on its own.
union Mixed {
  struct {
    float a, b;
  } s;
  double d;
};
struct SLow {
  union Mixed u;
  int i;
};
void take_low(struct SLow s);

// The reduction applies at whichever eightbyte the union lands on, not only
// the first.
struct SHigh {
  double lead;
  union Mixed u;
};
void take_high(struct SHigh s);

// It also applies through an array of the union.
struct SArr {
  union Mixed u[1];
  int i;
};
void take_arr(struct SArr s);

// And on the return path.
struct SLow ret_low(void);

// The reduction is reached the same way when the union is the argument itself
// rather than a member.
void take_mixed(union Mixed u);
union Mixed ret_mixed(void);

void *refs[] = {(void *)take_all_float,  (void *)take_low,
                (void *)take_high,       (void *)take_arr,
                (void *)ret_low,         (void *)take_mixed,
                (void *)ret_mixed};

// The pair ret_low comes back in is the reduced double and the trailing int.
// CIR: !rec_anon_struct = !cir.struct<{data !cir.double, data !s32i}>

// CIR: cir.func private @take_all_float(!cir.vector<2 x !cir.float>, !s32i)
// CIR: cir.func private @take_low(!cir.double, !s32i)
// CIR: cir.func private @take_high(!cir.double, !cir.double)
// CIR: cir.func private @take_arr(!cir.double, !s32i)
// CIR: cir.func private @ret_low() -> !rec_anon_struct
// CIR: cir.func private @take_mixed(!cir.double)
// CIR: cir.func private @ret_mixed() -> !cir.double

// LLVM: declare void @take_all_float(<2 x float>, i32)
// LLVM: declare void @take_low(double, i32)
// LLVM: declare void @take_high(double, double)
// LLVM: declare void @take_arr(double, i32)
// LLVM: declare { double, i32 } @ret_low()
// LLVM: declare void @take_mixed(double)
// LLVM: declare double @ret_mixed()
