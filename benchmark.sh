#!/bin/bash
# Three measurements, all reproducible from a clean checkout:
#
#   1. Compiled x86-64 vs the tree-walking interpreter vs the bytecode VM,
#      on bench/workload.lang.
#   2. Emitted instruction count with the register pool on vs off
#      (--no-regalloc), over the whole .lang corpus.
#   3. Emitted instruction count with the peephole pass on vs off
#      (--no-peephole), over the whole .lang corpus.
#
# Every number quoted in README.md comes from running this script.
set -u
cd "$(dirname "$0")"

BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

REPS=${REPS:-5}

g++ -O2 -std=c++17 -o "$BUILD/compiler" compiler.cpp || exit 1

# --- 1. compiled vs interpreted, over every bench/*.lang program ---------
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

total_interp=0
total_vm=0
total_comp=0

for prog in bench/*.lang; do
    name=$(basename "$prog" .lang)
    "$BUILD/compiler" --compile "$prog" -o "$BUILD/$name.s" 2>/dev/null
    gcc -no-pie -O2 "$BUILD/$name.s" runtime.c -lm -o "$BUILD/$name.bin" || exit 1

    # Sanity check before timing: a fast wrong answer is not a speedup.
    "$BUILD/compiler" --interpret "$prog" > "$BUILD/$name.interp.out"
    "$BUILD/compiler" --vm "$prog" > "$BUILD/$name.vm.out"
    "$BUILD/$name.bin" > "$BUILD/$name.comp.out"
    if ! diff -q "$BUILD/$name.interp.out" "$BUILD/$name.vm.out" > /dev/null; then
        echo "ABORT: interpreter and vm disagree on $prog"
        diff "$BUILD/$name.interp.out" "$BUILD/$name.vm.out"
        exit 1
    fi
    if ! diff -q "$BUILD/$name.interp.out" "$BUILD/$name.comp.out" > /dev/null; then
        echo "ABORT: backends disagree on $prog"
        diff "$BUILD/$name.interp.out" "$BUILD/$name.comp.out"
        exit 1
    fi

    interp_ms=$(best "$BUILD/compiler" --interpret "$prog")
    vm_ms=$(best "$BUILD/compiler" --vm "$prog")
    comp_ms=$(best "$BUILD/$name.bin")
    total_interp=$((total_interp + interp_ms))
    total_vm=$((total_vm + vm_ms))
    total_comp=$((total_comp + comp_ms))

    echo "=== Execution: $prog (best of $REPS) ==="
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
    echo
done

echo "=== Aggregate over all bench/*.lang programs (sum of best-of-$REPS times) ==="
printf "  tree-walking interpreter : %6d ms\n" "$total_interp"
printf "  bytecode vm              : %6d ms\n" "$total_vm"
printf "  compiled x86-64          : %6d ms\n" "$total_comp"
if [ "$total_comp" -gt 0 ]; then
    printf "  speedup (interp/x86)    : %sx\n" \
        "$(echo "scale=2; $total_interp / $total_comp" | bc)"
    printf "  speedup (vm/x86)        : %sx\n" \
        "$(echo "scale=2; $total_vm / $total_comp" | bc)"
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

# --- 3. peephole pass on vs off -------------------------------------------
peep_without=$(count_instructions "--no-peephole")

echo
echo "=== Codegen: emitted instructions, peephole pass on vs off ==="
printf "  before peephole          : %6d\n" "$peep_without"
printf "  after peephole           : %6d\n" "$with"
printf "  reduction                : %s%%\n" \
    "$(echo "scale=2; 100 * ($peep_without - $with) / $peep_without" | bc)"

# --- 4. constant folding on vs off ----------------------------------------
cfold_without=$(count_instructions "--no-constfold")

echo
echo "=== Codegen: emitted instructions, constant folding on vs off ==="
printf "  before constant folding  : %6d\n" "$cfold_without"
printf "  after constant folding   : %6d\n" "$with"
printf "  reduction                : %s%%\n" \
    "$(echo "scale=2; 100 * ($cfold_without - $with) / $cfold_without" | bc)"
