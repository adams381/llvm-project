// Cross-compilation-unit ABI validation test.
//
// Compiles lib.c with -fclangir and main.c with classic Clang,
// links them together, and runs the result.  If the ABI lowering
// is wrong, the cross-TU calls will produce incorrect results or
// crash.
//
// REQUIRES: x86-registered-target
// REQUIRES: host-supports-jit
// RUN: split-file %s %t
// RUN: %clang -target x86_64-unknown-linux-gnu -fclangir -c %t/lib.c -o %t/lib.o
// RUN: %clang -target x86_64-unknown-linux-gnu -c %t/main.c -o %t/main.o
// RUN: %clang -target x86_64-unknown-linux-gnu %t/lib.o %t/main.o -o %t/a.out
// RUN: %t/a.out | FileCheck %s

//--- lib.c

typedef struct {
  int a, b;
} Pair;

typedef struct {
  int a, b, c, d, e;
} Big;

// Direct coercion return: {int,int} -> i64 on x86_64
Pair make_pair(int a, int b) {
  Pair p;
  p.a = a;
  p.b = b;
  return p;
}

// Direct coercion arg: {int,int} passed as i64 on x86_64
int sum_pair(Pair p) {
  return p.a + p.b;
}

// sret return: >16 bytes returned via hidden pointer
Big make_big(int x) {
  Big b;
  b.a = x;
  b.b = x + 1;
  b.c = x + 2;
  b.d = x + 3;
  b.e = x + 4;
  return b;
}

// byval arg: >16 bytes passed via hidden pointer
int sum_big(Big b) {
  return b.a + b.b + b.c + b.d + b.e;
}

// Mixed: coerced struct return + scalar arg
Pair add_to_pair(Pair p, int n) {
  Pair r;
  r.a = p.a + n;
  r.b = p.b + n;
  return r;
}

// Scalar passthrough (baseline sanity check)
int add(int a, int b) {
  return a + b;
}

//--- main.c

#include <stdio.h>

typedef struct {
  int a, b;
} Pair;

typedef struct {
  int a, b, c, d, e;
} Big;

Pair make_pair(int a, int b);
int sum_pair(Pair p);
Big make_big(int x);
int sum_big(Big b);
Pair add_to_pair(Pair p, int n);
int add(int a, int b);

int main(void) {
  // Direct coercion return
  Pair p = make_pair(3, 4);
  printf("pair: %d %d\n", p.a, p.b);
  // CHECK: pair: 3 4

  // Direct coercion arg
  printf("sum_pair: %d\n", sum_pair(p));
  // CHECK: sum_pair: 7

  // sret return
  Big b = make_big(10);
  printf("big: %d %d %d %d %d\n", b.a, b.b, b.c, b.d, b.e);
  // CHECK: big: 10 11 12 13 14

  // byval arg
  printf("sum_big: %d\n", sum_big(b));
  // CHECK: sum_big: 60

  // Mixed coercion + scalar
  Pair q = add_to_pair(p, 10);
  printf("add_to_pair: %d %d\n", q.a, q.b);
  // CHECK: add_to_pair: 13 14

  // Scalar baseline
  printf("add: %d\n", add(17, 25));
  // CHECK: add: 42

  return 0;
}
