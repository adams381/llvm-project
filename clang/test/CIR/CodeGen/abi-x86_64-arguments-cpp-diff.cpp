// Differential test: compile the OGCG x86_64-arguments.cpp through
// CIR and verify that shared function signatures match OGCG.
//
// XFAIL: *
// Blocked on member pointer support (isZeroInitializable NYI).
// Will pass after rebase onto upstream main (which has #187327).
//
// CIR eagerly emits some linkonce_odr copy constructors that OGCG
// defers, so we compare only functions present in both outputs.
//
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++11 \
// RUN:   -fclangir -emit-llvm %S/../../CodeGenCXX/x86_64-arguments.cpp \
// RUN:   -o %t-cir.ll
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++11 \
// RUN:   -emit-llvm %S/../../CodeGenCXX/x86_64-arguments.cpp \
// RUN:   -o %t-ogcg.ll
//
// Extract and normalize function signatures.
// RUN: grep '^define' %t-cir.ll \
// RUN:   | sed 's/dso_local //;s/ #[0-9]*//;s/ {$//;s/%[a-zA-Z0-9._]*/%x/g;s/  */ /g' \
// RUN:   | sort > %t-cir-sigs.txt
// RUN: grep '^define' %t-ogcg.ll \
// RUN:   | sed 's/dso_local //;s/ #[0-9]*//;s/ {$//;s/%[a-zA-Z0-9._]*/%x/g;s/  */ /g' \
// RUN:   | sort > %t-ogcg-sigs.txt
//
// Extract just the mangled names from each output.
// RUN: sed 's/.* @\([^ (]*\).*/\1/' %t-cir-sigs.txt | sort > %t-cir-names.txt
// RUN: sed 's/.* @\([^ (]*\).*/\1/' %t-ogcg-sigs.txt | sort > %t-ogcg-names.txt
//
// Find shared function names, then grep full signatures for those.
// RUN: comm -12 %t-cir-names.txt %t-ogcg-names.txt > %t-shared-names.txt
//
// Extract signatures for shared functions and compare.
// RUN: grep -F -f %t-shared-names.txt %t-cir-sigs.txt | sort > %t-cir-shared.txt
// RUN: grep -F -f %t-shared-names.txt %t-ogcg-sigs.txt | sort > %t-ogcg-shared.txt
// RUN: diff %t-cir-shared.txt %t-ogcg-shared.txt
//
// Verify we matched at least 23 shared functions (guards against
// the test silently becoming vacuous).
// RUN: test $(wc -l < %t-shared-names.txt) -ge 23
