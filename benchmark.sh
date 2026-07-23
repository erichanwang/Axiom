#!/bin/bash
# Three measurements, all reproducible from a clean checkout:
#
#   1. Compiled x86-64 vs the tree-walking interpreter vs the bytecode VM,
#      on bench/workload.lang.
#   2. Emitted instruction count with the register pool on vs off
#      (--no-regalloc), over the whole .lang corpus.
#
# Every number quoted in README.md comes from running this script.
set -u
cd "$(dirname "$0")"

BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

REPS=${REPS:-5}

g++ -O2 -std=c++17 -o "$BUILD/compiler" compiler.cpp || exit 1

# --- 1. compiled vs interpreted ------------------------------------------
"$BUILD/compiler" --compile bench/workload.lang -o "$BUILD/workload.s" 2>/dev/null
gcc -no-pie -O2 "$BUILD/workload.s" runtime.c -lm -o "$BUILD/workload.bin" || exit 1

# Sanity check before timing: a fast wrong answer is not a speedup.
"$BUILD/compiler" --interpret bench/workload.lang > "$BUILD/interp.out"
"$BUILD/compiler" --vm bench/workload.lang > "$BUILD/vm.out"
"$BUILD/workload.bin" > "$BUILD/comp.out"
if ! diff -q "$BUILD/interp.out" "$BUILD/vm.out" > /dev/null; then
    echo "ABORT: interpreter and vm disagree on the benchmark workload"
    diff "$BUILD/interp.out" "$BUILD/vm.out"
    exit 1
fi
if ! diff -q "$BUILD/interp.out" "$BUILD/comp.out" > /dev/null; then
    echo "ABORT: backends disagree on the benchmark workload"
    diff "$BUILD/interp.out" "$BUILD/comp.out"
    exit 1
fi

# Best of N: the minimum is the least noise-contaminated sample.
best() {
    local best=""
    for _ in $(seq "$REPS"); do
        local start elapsed
        start=$(date +%s%N)
        "$@" > /dev/null 2>&1
        elapsed=$(( ($(date +%s%N) - start) / 1000000 ))
        if [ -z "$best" ] || [ "$elapsed" -lt "$best" ]; then best=$elapsed; fi
    done
    echo "$best"
}

interp_ms=$(best "$BUILD/compiler" --interpret bench/workload.lang)
vm_ms=$(best "$BUILD/compiler" --vm bench/workload.lang)
comp_ms=$(best "$BUILD/workload.bin")

echo "=== Execution: bench/workload.lang (best of $REPS) ==="
printf "  tree-walking interpreter : %6d ms\n" "$interp_ms"
printf "  bytecode vm              : %6d ms\n" "$vm_ms"
printf "  compiled x86-64          : %6d ms\n" "$comp_ms"
if [ "$comp_ms" -gt 0 ]; then
    printf "  speedup (interp/x86)    : %sx\n" \
        "$(echo "scale=1; $interp_ms / $comp_ms" | bc)"
    printf "  speedup (vm/x86)        : %sx\n" \
        "$(echo "scale=1; $vm_ms / $comp_ms" | bc)"
else
    echo "  speedup                  : compiled run too fast to time at ms resolution"
fi

# --- 2. register pool on vs off ------------------------------------------
count_instructions() {
    local flag="$1" total=0
    for f in *.lang bench/*.lang; do
        "$BUILD/compiler" --compile $flag "$f" -o "$BUILD/count.s" 2>/dev/null
        total=$(( total + $(grep -cE '^[[:space:]]+[a-z]' "$BUILD/count.s") ))
    done
    echo "$total"
}

with=$(count_instructions "")
without=$(count_instructions "--no-regalloc")

echo
echo "=== Codegen: emitted instructions over the whole .lang corpus ==="
printf "  stack-spill temporaries  : %6d\n" "$without"
printf "  register-pool temporaries: %6d\n" "$with"
printf "  reduction                : %s%%\n" \
    "$(echo "scale=1; 100 * ($without - $with) / $without" | bc)"
