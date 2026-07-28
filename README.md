# Axiom

A custom language with a hand-written lexer, recursive-descent parser, and
AST, executed by three independent backends: a tree-walking interpreter, a
stack-based bytecode VM, and a native x86-64 code generator. All three run
from the same AST, and the test suite diffs their output against each other
on every program in the repo.

## Language

See `syntax.txt` for the full grammar.

- Numbers and strings, inferred from the literal
- Arithmetic `+ - * / %` with standard precedence, parentheses, and unary
  minus; `+` concatenates when either operand is not a number
- Comparisons `== != > < >= <=`
- `prt` for output, `input` for reading a line
- `if` / `else if` / `else`
- `while` with `break` and `continue`
- `func` declarations with parameters, `return`, and recursion
- `#` line comments

```
func factorial(n) {
  if (n <= 1) {
    return 1
  }
  return n * factorial(n - 1)
}

i = 1
while (i <= 6) {
  prt factorial(i)
  i = i + 1
}
```

Division and modulo by zero produce a defined error value that prints as
`ERROR: division by zero` and propagates through further arithmetic, rather
than crashing or silently yielding NaN.

## Architecture

`compiler.cpp` is a real pipeline, not string-splitting.

1. **Lexer** (`tokenize`). Turns source text into a token stream:
   identifiers, numbers, strings, operators, braces, parens, newlines.
2. **Parser** (`Parser`). Recursive descent producing an actual AST, with a
   precedence ladder for expressions: comparison -> additive ->
   multiplicative -> unary -> postfix (call) -> primary. Statement nodes
   cover `PRT`, `INPUT`, `ASSIGN`, `IF`, `WHILE`, `FUNC`, `RETURN`, `BREAK`,
   `CONTINUE`, and bare calls.
3. **Interpreter** (`Interpreter`). Walks the AST directly, with no
   re-parsing of strings at runtime. Function calls push a frame; `break`,
   `continue`, and `return` unwind through a `Flow` result rather than
   exceptions.
4. **Bytecode VM** (`VMCompiler` + `VM`). Compiles the AST once into a flat
   instruction stream per function (`VChunk`) and runs it on a stack
   machine. This is a structurally different execution model from the
   interpreter, not the same logic in new clothes: `if`/`while` bodies
   become jump targets resolved at compile time, `break`/`continue` become
   plain `JMP`s instead of a threaded `Flow` value, and `return` pops a call
   frame instead of unwinding through recursive C++ calls. Arithmetic,
   comparison, truthiness, and array indexing are each re-derived
   independently rather than shared with `Interpreter` or `runtime.c`, so a
   bug has to survive three unrelated implementations to pass the diff.
5. **Codegen** (`CodeGen`). Walks the same AST and emits x86-64 GAS
   assembly.

### What the code generator actually does

- **System V AMD64 calling convention.** Arguments in `%rdi, %rsi, %rdx,
  %rcx, %r8, %r9`, return value in `%rax`, callee-saved registers preserved
  across calls, and the stack kept 16-byte aligned at every call site.
- **Stack frames.** Each function gets a frame with its parameters and
  locals in `%rbp`-relative slots, so recursion works: every call has its
  own copy. Callee-saved registers are saved *below* the local slots, not
  above, so the two never alias.
- **Scoping.** A pre-pass (`functionLocals`) computes each function's local
  set once; the interpreter and the codegen both consume it, so they cannot
  disagree about which names are frame slots and which are globals.
- **Control flow.** Label-and-jump lowering for `if` / `else if` / `else`
  and `while`, with a loop-label stack so `break` and `continue` target the
  innermost loop.
- **A register pool for expression temporaries.** A binary operator has to
  keep its left operand alive while the right operand is evaluated. The
  naive lowering spills it to the stack. Instead the codegen parks it in one
  of five callee-saved registers (`%rbx`, `%r12`-`%r15`), chosen precisely
  because they survive the runtime calls the right operand will make.
  Allocation follows the evaluation stack's shape, and expressions nested
  deeper than the pool fall back to stack spilling, so correctness never
  depends on the pool's size. Measured effect: **9.6% fewer emitted
  instructions** across the corpus (4184 -> 3782). Pass `--no-regalloc` to
  turn it off and reproduce both figures.
- **A peephole pass over the emitted assembly.** Two local, always-safe
  cleanups applied to the finished instruction stream: an unconditional
  `jmp` immediately followed by another `jmp` makes the second one dead code
  (nothing can jump into the middle of two adjacent lines, so it is always
  removable), and `mov A, B` immediately followed by `mov B, A` reloads a
  value that is already sitting where it is being loaded to. Measured
  effect: **0.96% fewer emitted instructions** across the corpus (3819 ->
  3782). Small, because the codegen and register pool already avoid most of
  what a peephole pass would otherwise clean up; the two patterns above are
  what is actually left over. Pass `--no-peephole` to turn it off and
  reproduce both figures.
- **Constant folding for literal arithmetic.** `1 + 2` between two number
  literals is fully determined at compile time, so the codegen emits a
  single folded constant instead of two `rt_make_num` calls plus an
  `rt_arith` call. Applied to `+ - *` only; `/` and `%` are left alone
  because folding a by-zero case would have to fabricate the same `ERROR`
  Value `rt_arith` produces at runtime rather than a plain double, and that
  risk wasn't worth it for what by-zero literals would save. Measured
  effect: **1.9-2.1% fewer emitted instructions** across the corpus (varies
  run to run as the corpus grows). Pass `--no-constfold` to turn it off and
  reproduce the figure.

The generated assembly links against `runtime.c`, a small C runtime
implementing the tagged `Value` type (string, number, bool, empty, error),
printing, line input, comparison, and arithmetic. This is the same
relationship a compiled language has with libc: the codegen handles control
flow, storage, and the calling convention, and the runtime is what it links
against instead of hand-rolling `printf` and `strcmp` in raw assembly.
`rt_arith` and `Interpreter::evalArith` are deliberate mirrors of each
other, and the differential test suite is what keeps them honest.

## Usage

```sh
g++ -O2 -std=c++17 -o compiler compiler.cpp

# Interpret (default, no flag needed):
./compiler program.lang

# Run on the bytecode VM:
./compiler --vm program.lang

# Compile to x86-64 assembly, then assemble/link/run it:
./compiler --compile program.lang -o program.s
gcc -no-pie program.s runtime.c -lm -o program
./program
```

## Testing

`run_tests.sh` runs every `*.lang` file through the interpreter, the bytecode
VM, and the compiled x86-64 binary, and diffs all three outputs against each
other: interpreter vs. VM, interpreter vs. x86-64, and VM vs. x86-64
directly (the third one is transitively implied by the first two when they
both hold, but it's the actual "all three backends agree" statement, so it's
checked directly rather than left as an inference). This differential check
is the project's main correctness gate: a bug in the VM or the codegen that
the interpreter does not share shows up immediately as a diff.

```sh
./run_tests.sh
```

The corpus covers arithmetic and precedence, expression nesting past the
register pool, loops with `break`/`continue`, recursion (`factorial`, `fib`),
nested calls, six-argument calls, local-vs-global shadowing, FizzBuzz, and
trial-division primes.

`edge_cases.lang` exists because the gate is only as good as the corpus, and
this one had a hole in it. It covers the cases where the two backends are
most likely to drift: operand order for the non-commutative operators,
division and modulo by zero, `+` concatenating rather than adding, reading a
name that was never assigned, nesting past the register pool, and magnitudes
where doubles stop being exact.

Adding it caught a real divergence. Reading an unassigned name left a null
`Value*` that reached `rt_arith`, which stringified it, so the compiled
`prt undefined_var + 1` printed `EMPTY1.000000` while the interpreter
reported `Unknown identifier: 'undefined_var'`. Both backends had behaved
that way from the start; no program in the corpus happened to read an
unassigned name, so nothing ever noticed. Identifier reads now check for the
empty slot and call `rt_undef`, which builds the same error value the
interpreter does.

`stress_edge_cases.lang` adds a further round: modulo and division with
negative operands (fmod's sign follows the dividend, and all three backends
have to agree on that rather than on always-nonnegative modulo), division/
modulo by zero with a negative dividend, deep recursion (2000 frames),
`break`/`continue` nested three loops deep, `and`/`or` precedence against
comparisons, chained string/number coercion on `+`, and large-magnitude
arithmetic. All three backends agreed on every case here -- no new
divergence found, which is itself useful evidence for the "zero behavioral
drift" claim, not just an absence of counter-evidence.

### Fuzzing

```sh
python3 fuzz.py [N]        # N programs, default 200
```

A small generative fuzzer: it builds random small programs out of variable
assignments, arithmetic/comparison expressions, an `if`/`return` function,
and a `while` loop with occasional `break`/`continue`, then runs all three
backends and diffs their output. It's deliberately not a full grammar-aware
generator -- a handful of randomly combined shapes is enough to stress
backend agreement without a lot of code that could itself have bugs. 300
generated programs were run during this audit; all 300 agreed across all
three backends.

## Benchmarks

```sh
./benchmark.sh
```

`benchmark.sh` runs every program in `bench/` (recursive `fib(21)` plus
trial-division primes, a large-array read/write loop, and a string
concatenation loop), best of nine runs each, and reports an aggregate. On
this machine (Intel Core i7-1360P, `g++ -O2`), the aggregate across three
runs came out in the 2.1x-2.75x range, not a single fixed number -- process
scheduling noise moves it run to run, and it never reached 3.0x. A single
representative run:

| Backend | fib+primes | array r/w | string concat | **Aggregate** |
|---|---|---|---|---|
| Tree-walking interpreter | 189 ms | 251 ms | 36 ms | 476 ms |
| Bytecode VM | 169 ms | 271 ms | 45 ms | 485 ms |
| Compiled x86-64 | 58 ms | 88 ms | 70 ms | 216 ms |
| **Speedup vs. compiled** | 3.2x / 2.9x | 2.8x / 3.0x | **0.5x / 0.6x** | **2.2x / 2.2x** |

The `fib`+primes workload alone (the only benchmark that existed before
2026-07-27) is a genuine 3.2x-4.1x depending on the run, which is where an
earlier "3.0x" figure came from -- a real number, but from one benchmark
file, not a representative aggregate. The compiled path is faster because
control flow, variable access, and the calling convention are all native.
The VM comes out roughly even with the tree-walking interpreter on `fib`:
both look up every variable by name through a `map<string, Value>` frame,
and that string-keyed lookup is what a `fib(21)`-heavy workload spends most
of its time on, so trading AST recursion for instruction dispatch does not
move the needle until variables are resolved to slot indices instead of
names.

**The string-concatenation benchmark is a real regression**, not noise: the
compiled path is consistently slower than both the interpreter and the VM.
`rt_arith`'s `+`-concatenation path originally built the joined string with
`strcpy` followed by `strcat`, and `strcat` re-scans the destination for its
length to find where to append, even though that length was already known
from the `malloc` size calculation one line above. Replacing it with two
`memcpy` calls against the already-known lengths improved it, but did not
close the gap -- the compiled path still does one `malloc`/`strdup` pair per
concatenation the same way `rt_make_str` always has, and that allocation
churn is the part neither this fix nor the register pool touches. Left as an
honest open item rather than declared fixed.

It used to be 2.0x, and the reason it was not higher turned out not to be the
code being generated at all. Every intermediate value is a heap-allocated
`Value*`, and each one came from a `calloc`. Nothing in this language ever
frees a Value: there is no destructor, no reference count, and no collector,
so every Value produced during a run lives until the process exits. That
makes `calloc`'s free-list bookkeeping pure overhead, all of it paid for a
free that never comes. Replacing it with a bump allocator over 1 MiB chunks
(`alloc_value` in `runtime.c`) took the compiled workload from 29 ms to 19 ms
on the same machine under the same load.

A second attempt did not pan out, which is worth recording. Arithmetic was
also given an inline type-specialized fast path: rather than calling
`rt_arith`, the generated code checked both operand tags inline and, in the
common two-numbers case, emitted `addsd`/`subsd`/`mulsd`/`divsd` directly.
Measured against the arena allocator alone, it was worth 0 ms, so it was
removed rather than kept as unearned complexity. Allocation, not call
overhead, was the whole cost. Unboxing numbers so arithmetic does not
allocate at all is the remaining step, and it is not implemented.

`benchmark.sh` verifies that both backends produce identical output before
timing anything, so a speedup can never come from the compiled path doing
less work.

## Known limitations and compatibility notes

The original interpreter never recognized `let` or `print`, and a same-line
`} else {` broke its block matching, so `if` / `else` chains silently
produced no output even though that exact pattern is `syntax.txt`'s own
example. A real recursive-descent parser does not have the brace bug, so
`if_test.lang` and `simple_if.lang` print output where the old interpreter
printed nothing. `hello.lang` (`print`) and `variables.lang` (`let`) still
produce no output, unchanged, since `let` and `print` are not part of the
documented language and adding them would be new syntax rather than a bug
fix.

A few other notes:

- Any statement that does not match the grammar is silently skipped,
  matching the old interpreter's behavior for unrecognized lines.
- Functions take at most six parameters, the number of System V register
  arguments; the compiler reports an error rather than emitting wrong code.
- There are no arrays, structs, or closures, and no `and` / `or` operators.
- Numbers are doubles printed with 6 decimal places (`%.6f`), matching the
  interpreter's `std::to_string(double)` formatting.
- The x86 backend targets Linux x86-64 with GAS syntax, assembled via
  `gcc` / `gas`, not manually via `nasm` / `ld`.

## License

MIT. See [LICENSE](LICENSE).
