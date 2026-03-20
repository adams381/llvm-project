// Differential test: compile the OGCG x86_64-arguments.c through CIR
// and verify function signatures match OGCG.  This test auto-syncs
// with the upstream OGCG test -- no duplicate CHECK lines to maintain.
//
// RUN: %clang_cc1 -triple x86_64-unknown-unknown -Wno-strict-prototypes \
// RUN:   -fclangir -emit-llvm %S/../../CodeGen/X86/x86_64-arguments.c \
// RUN:   -o %t-cir.ll
// RUN: %clang_cc1 -triple x86_64-unknown-unknown -Wno-strict-prototypes \
// RUN:   -emit-llvm %S/../../CodeGen/X86/x86_64-arguments.c \
// RUN:   -o %t-ogcg.ll
//
// Normalize: strip dso_local, param names, attr groups, opening brace.
// RUN: grep '^define' %t-cir.ll \
// RUN:   | sed 's/dso_local //;s/ #[0-9]*//;s/ {$//;s/%[a-zA-Z0-9._]*/%x/g;s/  */ /g' \
// RUN:   | sort > %t-cir-sigs.txt
// RUN: grep '^define' %t-ogcg.ll \
// RUN:   | sed 's/dso_local //;s/ #[0-9]*//;s/ {$//;s/%[a-zA-Z0-9._]*/%x/g;s/  */ /g' \
// RUN:   | sort > %t-ogcg-sigs.txt
//
// All 73 function signatures must match.
// RUN: diff %t-cir-sigs.txt %t-ogcg-sigs.txt
