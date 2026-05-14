// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --check-prefix=CIR --input-file=%t.cir %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefix=LLVM --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefix=OGCG --input-file=%t.ll %s

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

// CIR-DAG: !rec_Node = !cir.record<struct "Node" {!cir.ptr<!cir.func<(!s32i, !cir.ptr<!cir.record<struct "Node">>) -> !s32i>>, !cir.ptr<!cir.record<struct "Node">>}>

// CIR-LABEL: cir.func {{.*}}@_Z6worker1DP4Node(%arg0: !s32i
// CIR:        %arg1: !cir.ptr<!rec_Node>
// LLVM:      define {{.*}} i32 @_Z6worker1DP4Node(i32 {{.*}}, ptr {{.*}})
// OGCG:      define {{.*}} i32 @_Z6worker1DP4Node(i32 {{.*}}, ptr {{.*}})

// CIR-LABEL: cir.func {{.*}}@_Z6callerP4Nodei
// CIR:         cir.call %{{.+}}(%{{.+}}, %{{.+}}) : (!cir.ptr<!cir.func<(!s32i, !cir.ptr<!rec_Node>) -> !s32i>>, !s32i {{.*}}, !cir.ptr<!rec_Node> {{.*}}) -> (!s32i {{.*}})
// LLVM:      define {{.*}} i32 @_Z6callerP4Nodei(
// OGCG:      define {{.*}} i32 @_Z6callerP4Nodei(
