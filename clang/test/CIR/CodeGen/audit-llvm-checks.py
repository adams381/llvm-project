#!/usr/bin/env python3
"""Audit CIR test files for tight LLVM CHECK lines.

Compiles each test file with CIR (-fclangir -emit-llvm) and OGCG
(-emit-llvm), compares function signatures, and reports:
  - ABI-attribute mismatches (potential bugs)
  - Loose LLVM CHECK lines that could be tightened

Usage:
    python3 audit-llvm-checks.py --clang /path/to/clang [--files file1 file2]
    python3 audit-llvm-checks.py --clang /path/to/clang --all-modified
"""

import argparse
import os
import re
import subprocess
import sys

ABI_ATTRS = [
    "noundef", "signext", "zeroext", "nonnull", "noalias",
    "byval", "sret", "dead_on_return", "dead_on_unwind", "writable",
    "nofpclass", "inreg",
]

DEREFERENCEABLE_RE = re.compile(r"dereferenceable(?:_or_null)?\(\d+\)")
ALIGN_PARAM_RE = re.compile(r"align \d+")

NON_ABI_TOKENS = [
    "dso_local", "unnamed_addr", "comdat", "mustprogress",
    "noinline", "optnone",
]


def extract_run_info(filepath):
    """Extract triple and extra flags from CIR -emit-llvm RUN line."""
    triple = "x86_64-unknown-linux-gnu"
    extra_flags = []
    with open(filepath) as f:
        for line in f:
            if "-fclangir" in line and "-emit-llvm" in line:
                m = re.search(r"-triple\s+(\S+)", line)
                if m:
                    triple = m.group(1)
                for flag in ["-std=c++20", "-std=c++17", "-std=c++14",
                             "-std=c++11", "-std=c++2a", "-std=c++23",
                             "-mconstructor-aliases",
                             "-fno-delete-null-pointer-checks",
                             "-fno-finite-loops",
                             "-Wno-unused-value",
                             "-ffinite-math-only",
                             "-menable-no-nans", "-menable-no-infs"]:
                    if flag in line:
                        extra_flags.append(flag)
                break
    return triple, extra_flags


def compile_to_ir(clang, filepath, triple, extra_flags, use_cir):
    """Compile source and return LLVM IR string."""
    cmd = [clang, "-cc1", "-triple", triple]
    cmd.extend(extra_flags)
    if use_cir:
        cmd.extend(["-fclangir", "-emit-llvm"])
    else:
        cmd.extend(["-emit-llvm"])
    cmd.extend([filepath, "-o", "-"])
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    if result.returncode != 0:
        return None, result.stderr
    return result.stdout, None


def extract_signatures(llvm_ir):
    """Extract all define/declare signatures from LLVM IR."""
    sigs = {}
    for line in llvm_ir.splitlines():
        line = line.strip()
        if line.startswith("define ") or line.startswith("declare "):
            m = re.match(r"(define|declare)\s+(.+?)@(\S+)\(([^)]*)\)(.*)",
                         line)
            if m:
                kind = m.group(1)
                ret_part = m.group(2).strip()
                name = m.group(3)
                params = m.group(4).strip()
                suffix = m.group(5).strip()
                sigs[name] = {
                    "kind": kind,
                    "ret": ret_part,
                    "params": params,
                    "suffix": suffix,
                    "raw": line,
                }
    return sigs


def normalize_for_abi(sig_str):
    """Normalize a signature string keeping only ABI-relevant parts."""
    s = sig_str
    s = re.sub(r"%[\w.]+", "%x", s)
    s = re.sub(r"#\d+", "", s)
    for tok in NON_ABI_TOKENS:
        s = s.replace(tok + " ", "")
        s = s.replace(tok, "")
    s = re.sub(r"\s+", " ", s).strip()
    return s


def extract_abi_attrs(param_str):
    """Extract ABI-relevant attributes from a parameter string."""
    attrs = set()
    for attr in ABI_ATTRS:
        if attr in param_str:
            attrs.add(attr)
    deref = DEREFERENCEABLE_RE.search(param_str)
    if deref:
        attrs.add(deref.group(0))
    return attrs


def compare_signatures(cir_sigs, ogcg_sigs):
    """Compare CIR vs OGCG signatures, return mismatches."""
    results = []
    all_names = set(cir_sigs.keys()) | set(ogcg_sigs.keys())
    for name in sorted(all_names):
        cir = cir_sigs.get(name)
        ogcg = ogcg_sigs.get(name)
        if not cir or not ogcg:
            continue

        cir_norm = normalize_for_abi(cir["raw"])
        ogcg_norm = normalize_for_abi(ogcg["raw"])

        if cir_norm != ogcg_norm:
            cir_attrs = extract_abi_attrs(cir["raw"])
            ogcg_attrs = extract_abi_attrs(ogcg["raw"])
            if cir_attrs != ogcg_attrs:
                results.append({
                    "name": name,
                    "type": "ABI_MISMATCH",
                    "cir": cir["raw"],
                    "ogcg": ogcg["raw"],
                    "cir_attrs": cir_attrs,
                    "ogcg_attrs": ogcg_attrs,
                    "missing_in_cir": ogcg_attrs - cir_attrs,
                    "extra_in_cir": cir_attrs - ogcg_attrs,
                })
            else:
                results.append({
                    "name": name,
                    "type": "NON_ABI_DIFF",
                    "cir": cir["raw"],
                    "ogcg": ogcg["raw"],
                })
        else:
            results.append({
                "name": name,
                "type": "MATCH",
                "cir": cir["raw"],
                "ogcg": ogcg["raw"],
            })
    return results


def check_llvm_lines(filepath, matching_sigs):
    """Check which matching signatures have loose LLVM CHECK lines."""
    with open(filepath) as f:
        content = f.read()

    loose = []
    for sig in matching_sigs:
        name = sig["name"]
        short_name = name.rstrip("(")
        ogcg_attrs = extract_abi_attrs(sig["ogcg"])
        if not ogcg_attrs:
            continue

        llvm_lines = []
        for line in content.splitlines():
            if "LLVM" in line and short_name in line:
                llvm_lines.append(line)

        if not llvm_lines:
            continue

        for attr in ogcg_attrs:
            found = False
            for ll in llvm_lines:
                if attr in ll:
                    found = True
                    break
            if not found:
                loose.append({
                    "func": name,
                    "missing_attr": attr,
                    "llvm_lines": llvm_lines,
                })
    return loose


def audit_file(clang, filepath, verbose=False):
    """Audit a single test file. Returns results dict."""
    triple, extra_flags = extract_run_info(filepath)

    cir_ir, cir_err = compile_to_ir(clang, filepath, triple,
                                    extra_flags, True)
    if cir_err:
        return {"file": filepath, "error": f"CIR: {cir_err[:200]}"}

    ogcg_ir, ogcg_err = compile_to_ir(clang, filepath, triple,
                                      extra_flags, False)
    if ogcg_err:
        return {"file": filepath, "error": f"OGCG: {ogcg_err[:200]}"}

    cir_sigs = extract_signatures(cir_ir)
    ogcg_sigs = extract_signatures(ogcg_ir)
    comparisons = compare_signatures(cir_sigs, ogcg_sigs)

    matching = [c for c in comparisons if c["type"] == "MATCH"]
    abi_mismatches = [c for c in comparisons
                      if c["type"] == "ABI_MISMATCH"]
    non_abi = [c for c in comparisons if c["type"] == "NON_ABI_DIFF"]

    loose_checks = check_llvm_lines(filepath, matching + non_abi)

    return {
        "file": filepath,
        "total_funcs": len(comparisons),
        "matches": len(matching),
        "abi_mismatches": abi_mismatches,
        "non_abi_diffs": len(non_abi),
        "loose_checks": loose_checks,
    }


def find_modified_files(repo_root):
    """Find test files modified on the branch vs origin/main."""
    cmd = ["git", "diff", "--name-only", "origin/main..HEAD",
           "--", "clang/test/CIR/CodeGen/*.c",
           "clang/test/CIR/CodeGen/*.cpp"]
    result = subprocess.run(cmd, capture_output=True, text=True,
                            cwd=repo_root)
    files = []
    for line in result.stdout.strip().splitlines():
        path = os.path.join(repo_root, line)
        if os.path.exists(path):
            with open(path) as f:
                content = f.read()
            if "check-prefix=LLVM" in content or "--check-prefix=LLVM" in content:
                files.append(path)
    return sorted(files)


def main():
    parser = argparse.ArgumentParser(
        description="Audit CIR test files for tight LLVM CHECK lines")
    parser.add_argument("--clang", required=True, help="Path to clang")
    parser.add_argument("--files", nargs="*",
                        help="Specific test files to audit")
    parser.add_argument("--all-modified", action="store_true",
                        help="Audit all modified files on branch")
    parser.add_argument("--repo-root", default=".",
                        help="Repository root directory")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--summary-only", action="store_true",
                        help="Only print summary, skip per-file details")
    args = parser.parse_args()

    if args.all_modified:
        files = find_modified_files(args.repo_root)
    elif args.files:
        files = args.files
    else:
        parser.error("Specify --files or --all-modified")

    total_files = len(files)
    total_abi_mismatches = 0
    total_loose = 0
    total_funcs = 0
    total_matches = 0
    errors = 0
    files_with_mismatches = []
    files_with_loose = []

    for filepath in files:
        result = audit_file(args.clang, filepath, args.verbose)

        if "error" in result:
            errors += 1
            if not args.summary_only:
                print(f"ERROR: {filepath}")
                print(f"  {result['error']}")
            continue

        total_funcs += result["total_funcs"]
        total_matches += result["matches"]

        if result["abi_mismatches"]:
            total_abi_mismatches += len(result["abi_mismatches"])
            files_with_mismatches.append(filepath)
            if not args.summary_only:
                print(f"\nABI MISMATCHES in {filepath}:")
                for mm in result["abi_mismatches"]:
                    print(f"  {mm['name']}:")
                    print(f"    CIR:  {mm['cir']}")
                    print(f"    OGCG: {mm['ogcg']}")
                    if mm["missing_in_cir"]:
                        print(f"    Missing in CIR: "
                              f"{', '.join(mm['missing_in_cir'])}")
                    if mm["extra_in_cir"]:
                        print(f"    Extra in CIR: "
                              f"{', '.join(mm['extra_in_cir'])}")

        if result["loose_checks"]:
            total_loose += len(result["loose_checks"])
            files_with_loose.append((filepath, result["loose_checks"]))
            if not args.summary_only:
                print(f"\nLOOSE CHECKS in {filepath}:")
                for lc in result["loose_checks"]:
                    print(f"  {lc['func']}: missing '{lc['missing_attr']}'")

    print(f"\n{'='*60}")
    print(f"AUDIT SUMMARY")
    print(f"{'='*60}")
    print(f"Files audited:       {total_files}")
    print(f"Files with errors:   {errors}")
    print(f"Functions compared:  {total_funcs}")
    print(f"  ABI matches:       {total_matches}")
    print(f"  ABI mismatches:    {total_abi_mismatches}")
    print(f"  Loose checks:      {total_loose}")
    print()

    if files_with_mismatches:
        print(f"Files with ABI mismatches ({len(files_with_mismatches)}):")
        for f in files_with_mismatches:
            print(f"  {f}")
        print()

    if files_with_loose:
        print(f"Files with loose LLVM checks ({len(files_with_loose)}):")
        for f, checks in files_with_loose:
            attrs = set(lc["missing_attr"] for lc in checks)
            print(f"  {os.path.basename(f)}: {', '.join(sorted(attrs))}")
        print()

    if total_abi_mismatches > 0:
        print(f"ATTENTION: {total_abi_mismatches} ABI mismatches need "
              f"investigation")
        return 1

    if total_loose > 0:
        print(f"INFO: {total_loose} LLVM CHECK lines could be tightened")

    return 0


if __name__ == "__main__":
    sys.exit(main())
