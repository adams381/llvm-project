// XFAIL: *
// (cascade's conditional-rename strategy produces inconsistent rename
// verdicts on cycle paths > 1 level deep; the rename-closure analysis
// approach is the planned fix path)

// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s

struct D { int x; };

struct Node;
struct Node {
  int (*next_fn)(D, Node *);
  Node *next;
};

int worker(D d, Node *n) { return d.x + (n ? 1 : 0); }

int caller(Node *n, int x) {
  D d = {x};
  return n->next_fn(d, n->next);
}

// CIR-LABEL: cir.func {{.*}}@_Z6worker1DPS_
