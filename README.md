# Axiom

A custom language with a hand-written lexer, recursive-descent parser, and
AST, executed either by a tree-walking interpreter or by a native x86-64
code generator. Both backends run from the same AST, and the test suite
diffs them against each other on every program in the repo.

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
4. **Codegen** (`CodeGen`). Walks the same AST and emits x86-64 GAS
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
  depends on the pool's size. Measured effect: **10.1% fewer emitted
  instructions** across the corpus (1720 -> 1545). Pass `--no-regalloc` to
  turn it off and reproduce both figures.

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

# Compile to x86-64 assembly, then assemble/link/run it:
./compiler --compile program.lang -o program.s
gcc -no-pie program.s runtime.c -lm -o program
./program
```

## Testing

`run_tests.sh` runs every `*.lang` file through both the interpreter and the
compiled x86-64 binary and diffs their output, failing if the two disagree.
This differential check is the project's main correctness gate: a codegen bug
that the interpreter does not share shows up immediately as a diff.

```sh
./run_tests.sh
```

The corpus covers arithmetic and precedence, expression nesting past the
register pool, loops with `break`/`continue`, recursion (`factorial`, `fib`),
nested calls, six-argument calls, local-vs-global shadowing, FizzBuzz, and
trial-division primes.

## Benchmarks

```sh
./benchmark.sh
```

Measured on `bench/workload.lang` (recursive `fib(21)` plus trial-division
primes below 4000), best of five runs:

| Backend | Time |
|---|---|
| Tree-walking interpreter | 103 ms |
| Compiled x86-64 | 46 ms |
| **Speedup** | **2.2x** |

The compiled path is faster because control flow, variable access, and the
calling convention are all native. It is 2.2x rather than 20x because every
value is still a heap-allocated `Value*` built through a runtime call, so
allocation dominates. Unboxing numbers into registers is the obvious next
step and is not implemented.

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
