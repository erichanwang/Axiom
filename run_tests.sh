#!/bin/bash
# Runs every .lang test file through both the interpreter and the x86-64
# codegen path, and diffs the two outputs. Exits non-zero on any mismatch.
set -u
cd "$(dirname "$0")"

BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

g++ -O2 -std=c++17 -o "$BUILD/compiler" compiler.cpp || exit 1

fail=0

for f in *.lang; do
    name="${f%.lang}"
    stdin_file=/dev/null
    [ "$name" = "input" ] && { echo "Eric" > "$BUILD/stdin.txt"; stdin_file="$BUILD/stdin.txt"; }

    "$BUILD/compiler" --interpret "$f" < "$stdin_file" > "$BUILD/$name.interp.out" 2>&1

    "$BUILD/compiler" --compile "$f" -o "$BUILD/$name.s" 2>/dev/null
    gcc -no-pie "$BUILD/$name.s" runtime.c -lm -o "$BUILD/$name.bin" 2>"$BUILD/$name.cc.err"
    if [ $? -ne 0 ]; then
        echo "FAIL (assemble/link): $f"
        cat "$BUILD/$name.cc.err"
        fail=1
        continue
    fi
    "$BUILD/$name.bin" < "$stdin_file" > "$BUILD/$name.asm.out" 2>&1

    if diff -q "$BUILD/$name.interp.out" "$BUILD/$name.asm.out" > /dev/null; then
        echo "OK:   $f"
    else
        echo "FAIL (output mismatch): $f"
        diff "$BUILD/$name.interp.out" "$BUILD/$name.asm.out"
        fail=1
    fi
done

exit $fail
