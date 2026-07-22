# Axiom

A small custom language with a real lexer, parser, and AST, that runs either
through a tree-walking interpreter or through a genuine x86-64 assembly
codegen backend.

## Language

See `syntax.txt`. Supported: variable assignment (`x = 10`), `prt` for
output, `input` for reading a line into a variable, and `if` / `else if` /
`else` with `==`, `!=`, `>`, `<`, `>=`, `<=` comparisons. Values are numbers
or strings, inferred from the literal. There is no arithmetic in the
language (`syntax.txt` never defines `+ - * /`, and no `.lang` file uses
them), so none is implemented.

## Architecture

`compiler.cpp` is a real pipeline, not string-splitting:

1. **Lexer** (`tokenize`) — turns source text into a token stream
   (identifiers, numbers, strings, operators, braces, parens, newlines).
2. **Parser** (`Parser`) — recursive-descent parser producing an actual AST
   (`Expr` / `Stmt` node structs: `NUMBER`, `STRING`, `IDENT`, `BINOP` exprs;
   `PRT`, `INPUT`, `ASSIGN`, `IF` statements with branch lists for
   `else if` chains).
3. **Interpreter** (`Interpreter`) — walks the AST directly (no re-parsing
   strings at runtime).
4. **Codegen** (`CodeGen`) — walks the same AST and emits x86-64 GAS
   assembly: real registers, the System V calling convention, stack-slot
   spilling for binop operands, and label/jump-based control flow for
   `if`/`else if`/`else`.

The generated assembly links against `runtime.c`, a small C runtime that
implements the tagged `Value` type (string/number/bool), printing, line
input, and comparisons — the same way a compiled language leans on libc
rather than hand-rolling `printf` and `strcmp` in raw asm. The codegen does
the actual compiling (control flow, storage, calling convention); the
runtime is the "libc" it links against.

## Usage

```sh
g++ -O2 -std=c++17 -o compiler compiler.cpp

# Interpret (default, no flag needed):
./compiler program.lang

# Compile to x86-64 assembly, then assemble/link/run it:
./compiler --compile program.lang -o program.s
gcc -no-pie program.s runtime.c -o program
./program
```

## Testing

`run_tests.sh` runs every `*.lang` file through both the interpreter and the
compiled x86-64 binary and diffs their output, failing if they disagree:

```sh
./run_tests.sh
```

## Known limitations / compatibility notes

- The previous version of this interpreter had two long-standing bugs:
  `let` and `print` were never recognized (only bare `x = 10` assignment and
  `prt` for output are real, per `syntax.txt`), and a same-line `} else {`
  broke its block-matching so `if`/`else` chains silently produced no
  output — even though that exact pattern is `syntax.txt`'s own example.
  A real recursive-descent parser does not have the brace bug, so
  `if_test.lang` and `simple_if.lang` now correctly print output where the
  old interpreter printed nothing. `hello.lang` (`print`) and
  `variables.lang` (`let`) still produce no output, unchanged, since `let`/
  `print` aren't part of the documented language and adding them would be
  new syntax, not a bug fix.
- Any statement that doesn't match the grammar above is silently skipped,
  matching the old interpreter's behavior for unrecognized lines.
- The x86 backend targets Linux x86-64 with GAS syntax, assembled via
  `gcc`/`gas` (not manually via `nasm`/`ld`).
- Numbers are represented/printed as doubles with 6 decimal places
  (`%.6f`), matching the interpreter's `std::to_string(double)` formatting.
