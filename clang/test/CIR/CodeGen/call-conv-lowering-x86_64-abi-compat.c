// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -target-feature +avx -fclangir -clangir-enable-call-conv-lowering -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --check-prefixes=LINUX,LINUX-CIR --input-file=%t-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -target-feature +avx -emit-llvm %s -o %t.ll
// RUN: FileCheck --check-prefixes=LINUX,LINUX-OGCG --input-file=%t.ll %s

// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -target-feature +avx -fclang-abi-compat=3.8 -fclangir -clangir-enable-call-conv-lowering -emit-llvm %s -o %t-38-cir.ll
// RUN: FileCheck --check-prefix=LINUX38 --input-file=%t-38-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -target-feature +avx -fclang-abi-compat=3.8 -emit-llvm %s -o %t-38.ll
// RUN: FileCheck --check-prefix=LINUX38 --input-file=%t-38.ll %s

// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -target-feature +avx -fclang-abi-compat=9 -fclangir -clangir-enable-call-conv-lowering -emit-llvm %s -o %t-9-cir.ll
// RUN: FileCheck --check-prefix=LINUX9 --input-file=%t-9-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -target-feature +avx -fclang-abi-compat=9 -emit-llvm %s -o %t-9.ll
// RUN: FileCheck --check-prefix=LINUX9 --input-file=%t-9.ll %s

// RUN: %clang_cc1 -triple x86_64-apple-darwin -target-feature +avx -fclangir -clangir-enable-call-conv-lowering -emit-llvm %s -o %t-darwin-cir.ll
// RUN: FileCheck --check-prefix=DARWIN --input-file=%t-darwin-cir.ll %s
// RUN: %clang_cc1 -triple x86_64-apple-darwin -target-feature +avx -emit-llvm %s -o %t-darwin.ll
// RUN: FileCheck --check-prefix=DARWIN --input-file=%t-darwin.ll %s

typedef long long v1ll __attribute__((vector_size(8)));
typedef __int128 v2i128 __attribute__((vector_size(32)));

// GCC classifies a 64-bit vector of a 64-bit integer as SSE.  Clang 3.8 and
// older did not, and Darwin, FreeBSD and PlayStation still do not.
void mmx(v1ll v) { (void)v; }

// LINUX: define dso_local void @mmx(double noundef %{{.+}})
// LINUX38: define dso_local void @mmx(i64 noundef %{{.+}})
// LINUX9: define dso_local void @mmx(double noundef %{{.+}})
// DARWIN: define void @mmx(i64 noundef %{{.+}})

// GCC classifies a vector of __int128 as memory.  Clang 9 and older did not,
// and only Linux and NetBSD follow it.  AVX is on above so that this vector
// would otherwise reach a register, which is what makes the rule observable.
void wide_int128(v2i128 v) { (void)v; }

// LINUX-CIR: define dso_local void @wide_int128(ptr noalias noundef byval(<2 x i128>) align 32 %{{[^,)]+}})
// LINUX-OGCG: define dso_local void @wide_int128(ptr noundef byval(<2 x i128>) align 32 %{{[^,)]+}})
// LINUX38: define dso_local void @wide_int128(<2 x i128> noundef %{{[^,)]+}})
// LINUX9: define dso_local void @wide_int128(<2 x i128> noundef %{{[^,)]+}})
// DARWIN: define void @wide_int128(<2 x i128> noundef %{{[^,)]+}})
