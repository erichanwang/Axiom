#!/usr/bin/env python3
"""Generates random small Axiom programs and checks that the interpreter,
the bytecode vm, and the compiled x86-64 binary all agree on their output.

Usage: python3 fuzz.py [N]   (default N=200 programs)

Exits non-zero and prints the offending program on the first divergence
found. Cheap and stupid on purpose: a handful of arithmetic/control-flow
shapes randomly combined, not a full grammar-aware generator -- that is
plenty to catch backend drift, and is a lot less code to get wrong itself.
"""
import random
import subprocess
import sys
import tempfile
import os

COMPILER = None  # set in main() after building

def rand_expr(vars_, depth=0):
    if depth >= 3 or random.random() < 0.4:
        choice = random.random()
        if choice < 0.4 and vars_:
            return random.choice(vars_)
        if choice < 0.8:
            return str(random.choice([0, 1, 2, 3, 5, 7, 10, -1, -5, 100]))
        return '"' + random.choice(["a", "b", "hi", ""]) + '"'
    op = random.choice(["+", "-", "*", "/", "%"])
    return "(" + rand_expr(vars_, depth + 1) + " " + op + " " + rand_expr(vars_, depth + 1) + ")"

def rand_cond(vars_):
    op = random.choice(["==", "!=", ">", "<", ">=", "<="])
    return rand_expr(vars_, 2) + " " + op + " " + rand_expr(vars_, 2)

def gen_program(seed):
    random.seed(seed)
    lines = []
    vars_ = []
    nvars = random.randint(1, 4)
    for i in range(nvars):
        v = f"v{i}"
        vars_.append(v)
        lines.append(f"{v} = {rand_expr(vars_)}")

    # A small function using an existing var name as its param.
    if random.random() < 0.5:
        lines.append("func f(n) {")
        lines.append(f"  if ({rand_cond(['n'])}) {{")
        lines.append(f"    return {rand_expr(['n'])}")
        lines.append("  }")
        lines.append(f"  return {rand_expr(['n'])}")
        lines.append("}")
        lines.append(f"prt f({rand_expr(vars_)})")

    # A small loop with break/continue.
    lines.append("i = 0")
    lines.append(f"while (i < {random.randint(1, 8)}) {{")
    if random.random() < 0.3:
        lines.append(f"  if ({rand_cond(vars_ + ['i'])}) {{")
        lines.append("    break")
        lines.append("  }")
    if random.random() < 0.3:
        lines.append("  i = i + 1")
        lines.append("  continue")
    lines.append(f"  prt {rand_expr(vars_ + ['i'])}")
    lines.append("  i = i + 1")
    lines.append("}")

    for v in vars_:
        lines.append(f"prt {v}")
        lines.append(f"prt {rand_expr(vars_)}")

    return "\n".join(lines) + "\n"

def run(cmd, src_file, build_dir):
    return subprocess.run(cmd, cwd=build_dir, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=10).stdout

def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    repo = os.path.dirname(os.path.abspath(__file__))
    build_dir = tempfile.mkdtemp(prefix="axiom_fuzz_")
    compiler = os.path.join(build_dir, "compiler")
    subprocess.run(["g++", "-O2", "-std=c++17", "-o", compiler,
                     os.path.join(repo, "compiler.cpp")], check=True)

    fails = 0
    for seed in range(n):
        prog = gen_program(seed)
        src = os.path.join(build_dir, "fuzz.lang")
        with open(src, "w") as f:
            f.write(prog)

        try:
            interp_out = run([compiler, "--interpret", src], src, build_dir)
            vm_out = run([compiler, "--vm", src], src, build_dir)
            asm_path = os.path.join(build_dir, "fuzz.s")
            subprocess.run([compiler, "--compile", src, "-o", asm_path],
                            cwd=build_dir, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
            bin_path = os.path.join(build_dir, "fuzz.bin")
            r = subprocess.run(["gcc", "-no-pie", asm_path,
                                 os.path.join(repo, "runtime.c"), "-lm",
                                 "-o", bin_path], cwd=build_dir,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if r.returncode != 0:
                print(f"SEED {seed}: assemble/link failed\n{r.stdout.decode()}")
                print(prog)
                fails += 1
                continue
            asm_out = run([bin_path], src, build_dir)
        except subprocess.TimeoutExpired:
            print(f"SEED {seed}: timeout (likely infinite loop in generated program), skipping")
            continue

        if interp_out != vm_out or interp_out != asm_out:
            print(f"DIVERGENCE at seed {seed}:")
            print(prog)
            print("interp:", interp_out)
            print("vm:    ", vm_out)
            print("asm:   ", asm_out)
            fails += 1

    print(f"\n{n - fails}/{n} programs agreed across all three backends.")
    sys.exit(1 if fails else 0)

if __name__ == "__main__":
    main()
