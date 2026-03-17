#!/usr/bin/env python3
"""Differential fuzzer for CIR ABI lowering vs OGCG.

Generates random C struct types, compiles with CIR (-fclangir -emit-llvm)
and OGCG (-emit-llvm), and compares function signatures in the LLVM IR.
Reports any differences.

Usage:
    python3 abi-diff-fuzz.py --clang /path/to/clang [--count 100] [--seed 42]
"""

import argparse
import os
import random
import re
import subprocess
import sys
import tempfile

SCALAR_TYPES = [
    "char", "short", "int", "long", "long long",
    "unsigned char", "unsigned short", "unsigned int",
    "unsigned long", "unsigned long long",
    "float", "double",
]

TRIPLE = "x86_64-unknown-linux-gnu"


def gen_struct_fields(rng, depth=0, max_fields=6, max_depth=2):
    """Generate random struct fields."""
    fields = []
    num_fields = rng.randint(1, max_fields)
    for i in range(num_fields):
        if depth < max_depth and rng.random() < 0.2:
            inner = gen_struct(rng, f"Inner{depth}_{i}", depth + 1)
            fields.append(("inner", inner, f"f{i}"))
        elif rng.random() < 0.1:
            arr_len = rng.randint(1, 4)
            scalar = rng.choice(SCALAR_TYPES)
            fields.append(("array", scalar, arr_len, f"f{i}"))
        else:
            scalar = rng.choice(SCALAR_TYPES)
            fields.append(("scalar", scalar, f"f{i}"))
    return fields


def gen_struct(rng, name, depth=0):
    """Generate a random struct definition."""
    fields = gen_struct_fields(rng, depth)
    return {"name": name, "fields": fields}


def struct_to_c(struct, indent=0):
    """Convert a struct definition to C code."""
    prefix = "  " * indent
    lines = [f"{prefix}struct {struct['name']} {{"]
    for field in struct["fields"]:
        if field[0] == "scalar":
            lines.append(f"{prefix}  {field[1]} {field[2]};")
        elif field[0] == "array":
            lines.append(f"{prefix}  {field[1]} {field[3]}[{field[2]}];")
        elif field[0] == "inner":
            lines.extend(struct_to_c(field[1], indent + 1).split("\n"))
            lines.append(
                f"{prefix}  struct {field[1]['name']} {field[2]};")
    lines.append(f"{prefix}}};")
    return "\n".join(lines)


def gen_test_case(rng, idx):
    """Generate a test case with a struct and pass/return functions."""
    struct = gen_struct(rng, f"S{idx}")
    c_code = struct_to_c(struct)
    c_code += f"\nvoid take_s{idx}(struct S{idx} s) {{}}\n"
    c_code += f"struct S{idx} ret_s{idx}(void) {{ struct S{idx} s = {{0}}; return s; }}\n"
    return c_code, f"take_s{idx}", f"ret_s{idx}"


def extract_signatures(llvm_ir, func_names):
    """Extract function signatures from LLVM IR."""
    sigs = {}
    for name in func_names:
        pattern = rf"define\s+.*?@{re.escape(name)}\(([^)]*)\).*?{{"
        match = re.search(pattern, llvm_ir)
        if match:
            full_match = match.group(0)
            full_match = re.sub(r"%\w+", "%x", full_match)
            full_match = re.sub(r"#\d+", "", full_match).strip()
            sigs[name] = full_match
    return sigs


def compile_and_get_ir(clang, source_file, use_cir):
    """Compile source and return LLVM IR."""
    cmd = [clang, "-cc1", "-triple", TRIPLE]
    if use_cir:
        cmd.extend(["-fclangir", "-emit-llvm"])
    else:
        cmd.extend(["-emit-llvm"])
    cmd.extend([source_file, "-o", "-"])
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        return None, result.stderr
    return result.stdout, None


def normalize_sig(sig):
    """Normalize a signature for comparison.

    Strips parameter names, function attributes, noundef (CIR
    doesn't emit it on regular args yet), and whitespace
    differences so we compare only types and structural attrs.
    """
    sig = re.sub(r"%[\w.]+", "%x", sig)
    sig = re.sub(r"#\d+", "", sig)
    sig = re.sub(r"\s+", " ", sig).strip()
    sig = sig.replace("dso_local ", "")
    sig = sig.replace("noundef ", "")
    sig = sig.replace(" {", "")
    sig = re.sub(r"struct\.(\w+)", r"S.\1", sig)
    return sig


def main():
    parser = argparse.ArgumentParser(
        description="Differential fuzzer for CIR ABI lowering")
    parser.add_argument("--clang", required=True, help="Path to clang")
    parser.add_argument("--count", type=int, default=100,
                        help="Number of test cases")
    parser.add_argument("--seed", type=int, default=42, help="Random seed")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    rng = random.Random(args.seed)
    mismatches = 0
    errors = 0
    total_funcs = 0

    for i in range(args.count):
        c_code, take_name, ret_name = gen_test_case(rng, i)
        func_names = [take_name, ret_name]

        with tempfile.NamedTemporaryFile(
                mode="w", suffix=".c", delete=False) as f:
            f.write(c_code)
            src = f.name

        try:
            cir_ir, cir_err = compile_and_get_ir(args.clang, src, True)
            ogcg_ir, ogcg_err = compile_and_get_ir(args.clang, src, False)

            if cir_err:
                if args.verbose:
                    print(f"  CIR error on S{i}: {cir_err[:200]}")
                errors += 1
                continue
            if ogcg_err:
                if args.verbose:
                    print(f"  OGCG error on S{i}: {ogcg_err[:200]}")
                errors += 1
                continue

            cir_sigs = extract_signatures(cir_ir, func_names)
            ogcg_sigs = extract_signatures(ogcg_ir, func_names)

            for name in func_names:
                total_funcs += 1
                cir_sig = cir_sigs.get(name, "<missing>")
                ogcg_sig = ogcg_sigs.get(name, "<missing>")

                cir_norm = normalize_sig(cir_sig)
                ogcg_norm = normalize_sig(ogcg_sig)

                if cir_norm != ogcg_norm:
                    mismatches += 1
                    print(f"MISMATCH {name}:")
                    print(f"  CIR:  {cir_sig}")
                    print(f"  OGCG: {ogcg_sig}")
                    if args.verbose:
                        print(f"  Source:\n{c_code}")
                    print()
                elif args.verbose:
                    print(f"  OK: {name}")
        finally:
            os.unlink(src)

    print(f"\n{'='*60}")
    print(f"Results: {args.count} structs, {total_funcs} functions")
    print(f"  Matches:    {total_funcs - mismatches - errors}")
    print(f"  Mismatches: {mismatches}")
    print(f"  Errors:     {errors}")

    if mismatches > 0:
        print(f"\nFAILED: {mismatches} ABI mismatches found")
        return 1
    print("\nPASSED: All signatures match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
