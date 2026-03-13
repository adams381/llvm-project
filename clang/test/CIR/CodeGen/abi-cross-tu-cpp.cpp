// Cross-compilation-unit ABI validation test for C++ types.
//
// Compiles lib.cpp with -fclangir and main.cpp with classic Clang,
// links them together, and runs the result.  If the ABI lowering
// for non-trivially-copyable types is wrong, the cross-TU calls
// will produce incorrect results or crash.
//
// REQUIRES: x86-registered-target
// REQUIRES: host-supports-jit
// RUN: split-file %s %t
// RUN: %clang -target x86_64-unknown-linux-gnu -fclangir -c %t/lib.cpp -o %t/lib.o
// RUN: %clang -target x86_64-unknown-linux-gnu -c %t/main.cpp -o %t/main.o
// RUN: %clang -target x86_64-unknown-linux-gnu %t/lib.o %t/main.o -o %t/a.out -lstdc++
// RUN: %t/a.out | FileCheck %s

//--- lib.cpp

struct NTC {
  NTC() : x(0), y(0) {}
  NTC(int x, int y) : x(x), y(y) {}
  NTC(const NTC &o) : x(o.x), y(o.y) {}
  ~NTC() {}
  int x, y;
};

// Non-trivially-copyable arg: passed indirectly (ptr, no byval)
extern "C" int sum_ntc(NTC n) {
  return n.x + n.y;
}

// Non-trivially-copyable return: via sret pointer
extern "C" NTC make_ntc(int x, int y) {
  return NTC(x, y);
}

// Non-trivially-copyable arg + return
extern "C" NTC add_ntc(NTC a, NTC b) {
  return NTC(a.x + b.x, a.y + b.y);
}

// Large non-trivially-copyable (>16 bytes, also sret)
struct BigNTC {
  BigNTC() : a(0), b(0), c(0), d(0), e(0) {}
  BigNTC(int a, int b, int c, int d, int e)
      : a(a), b(b), c(c), d(d), e(e) {}
  BigNTC(const BigNTC &o)
      : a(o.a), b(o.b), c(o.c), d(o.d), e(o.e) {}
  ~BigNTC() {}
  int a, b, c, d, e;
};

extern "C" BigNTC make_big_ntc(int x) {
  return BigNTC(x, x + 1, x + 2, x + 3, x + 4);
}

extern "C" int sum_big_ntc(BigNTC b) {
  return b.a + b.b + b.c + b.d + b.e;
}

//--- main.cpp

#include <stdio.h>

struct NTC {
  NTC() : x(0), y(0) {}
  NTC(int x, int y) : x(x), y(y) {}
  NTC(const NTC &o) : x(o.x), y(o.y) {}
  ~NTC() {}
  int x, y;
};

struct BigNTC {
  BigNTC() : a(0), b(0), c(0), d(0), e(0) {}
  BigNTC(int a, int b, int c, int d, int e)
      : a(a), b(b), c(c), d(d), e(e) {}
  BigNTC(const BigNTC &o)
      : a(o.a), b(o.b), c(o.c), d(o.d), e(o.e) {}
  ~BigNTC() {}
  int a, b, c, d, e;
};

extern "C" int sum_ntc(NTC n);
extern "C" NTC make_ntc(int x, int y);
extern "C" NTC add_ntc(NTC a, NTC b);
extern "C" BigNTC make_big_ntc(int x);
extern "C" int sum_big_ntc(BigNTC b);

int main() {
  // Non-trivially-copyable arg (indirect, no byval)
  NTC n(3, 4);
  printf("sum_ntc: %d\n", sum_ntc(n));
  // CHECK: sum_ntc: 7

  // Non-trivially-copyable return (sret)
  NTC m = make_ntc(10, 20);
  printf("make_ntc: %d %d\n", m.x, m.y);
  // CHECK: make_ntc: 10 20

  // Non-trivially-copyable arg + return
  NTC a(1, 2);
  NTC b(3, 4);
  NTC c = add_ntc(a, b);
  printf("add_ntc: %d %d\n", c.x, c.y);
  // CHECK: add_ntc: 4 6

  // Large non-trivially-copyable return (sret, >16 bytes)
  BigNTC big = make_big_ntc(10);
  printf("big_ntc: %d %d %d %d %d\n", big.a, big.b, big.c, big.d, big.e);
  // CHECK: big_ntc: 10 11 12 13 14

  // Large non-trivially-copyable arg (indirect, no byval)
  printf("sum_big_ntc: %d\n", sum_big_ntc(big));
  // CHECK: sum_big_ntc: 60

  return 0;
}
