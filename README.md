# Sosu

A native, low-level, statically typed language, and its compiler `sosuc` (C99, libc only).
The specification is in [docs/SPEC.md](docs/SPEC.md).

English is the working language of the project: code, comments, compiler messages and documentation.

## Status

| Step (roadmap §13) | Status |
|---|---|
| 1. Lexer | done |
| 2. Parser -> AST, packages and imports | done (local dependencies only, no git yet) |
| 3. Semantic analysis | to do |
| 4. C code generation | to do |

For now, `sosuc check` loads a package, its dependencies and every imported module, and reports
the errors found up to parsing. `sosuc lex <file>` prints the tokens of a file and `sosuc parse <file>`
its syntax tree (as S-expressions). `.ui.sosu` files are not parsed by the compiler: their translator
will belong to the `ui` package (spec §10.2).

## Building

Windows (MSVC, located automatically with `vswhere`):

```bat
build.bat        :: builds build\sosuc.exe and the test programs
build.bat test   :: builds, then runs the tests
```

Linux / macOS (gcc or clang):

```sh
make
make test
```

## Usage

```
sosuc check tests/packages/app      # a package folder (default: the current folder)
sosuc lex tests/samples/hello.sosu
sosuc parse tests/samples/hello.sosu
sosuc parse file.sosu --format=json   # JSON diagnostics on stderr
```

Exit codes: 0 = success, 1 = errors in the source code, 2 = usage or I/O error.

## Layout

```
src/lib/      libsosu: library shared by all the tools (spec §11.4)
  common      allocation, formatting, dynamic arrays
  utf8        UTF-8 decoding
  source      source files, line / column positions
  diag        diagnostics (text and JSON, spec §7.5)
  token       token kinds
  lexer       lexical analysis
  arena       arena allocator for compiler data
  ast         syntax tree and its printer
  parser      syntax analysis (recursive descent)
  path        file path helpers
  manifest    sosu.toml parser
  program     loading of packages, dependencies and imported modules
src/sosuc/    command-line driver
lib/          the `core` and `std` packages (empty modules for now)
tests/        unit tests, .sosu samples taken from the spec, test packages
docs/         specification and error codes
```

`sosuc` looks for `core` and `std` in `$SOSU_HOME/lib`, or else in `../lib` next to the executable
(which is `lib/` for `build/sosuc`).

## Lexer decisions

Points the spec left open, settled here (see [docs/errors.md](docs/errors.md)):

- `++`, `--` and `::` are recognized only to report an explicit error with the fix.
- No leading zero (`007`): in C it means octal, a source of confusion.
- No type suffix on literals (`300u8`): the error suggests `300 as u8`.
- Float exponents: `1e5`, `2.5e-3` (lowercase only).
- Escapes: `\n \t \r \0 \\ \' \"`, `\xNN` limited to ASCII (so that strings stay valid UTF-8) and `\u{...}`.
- Strings cannot span lines; `/* */` comments do not nest.
- `as?` is lexed as `as` followed by `?`; the parser combines them.
- `fn` (function types, §5.1) is not in the keyword list: the lexer treats it as an identifier.

## Parser decisions

The grammar in §12 is a subset; the rest was settled as follows:

- A statement starting with `Type name` is a declaration (§5); a type can be `a.b.C`, `T*`, `T?`,
  `T[N]`, `T[]`, `T!E` or `fn(A, B) -> R uses io`.
- A variable always gets a value when declared (`i32 x;` is an error), as in the grammar.
- Precedence follows §5.1. `as` has the precedence of the unary operators and applies to their
  result: `-x as f32` is `(-x) as f32`. After `as`, `*` and `?` are type suffixes unless an operand
  follows: `x as i32 * 2` multiplies, `p as u8*` converts.
- `..` and `..=` sit between `||` and `?:`, and do not chain: `0..n + 1` is `0..(n + 1)`.
- Struct literal: `Point{ x: 1.0, y: 2.0 }`, trailing comma allowed. It is not allowed in
  `requires` / `ensures`, where the brace opens the function body.
- `match` is an expression; as a statement it ends with its `}`. Arms are separated by commas.
  Patterns: `_`, `Variant`, `Variant(a, b)` (no literals yet).
- In a function type, the `uses` list only continues after a comma with a predefined effect
  (`io`, `time`, `random`, `threads`), so that it is not confused with the next parameter.
- `defer` takes a simple statement or a block.
- After an error, the parser resynchronizes (end of statement, member or declaration, or closing
  bracket): one mistake gives one message.

## Package decisions

Spec §8.1 describes packages; these points were settled here:

- `sosu.toml` accepts comments, strings and one-line inline tables; `[package]` requires `name` and
  `version`. Package and dependency names are identifiers, since `import` uses them.
- The entry file is `src/main.sosu` for an application, or `src/lib.sosu` for a library. Only the
  modules reachable by `import` from it are loaded (libc cannot list a folder).
- `import pkg.a.b;` loads `src/a/b.sosu` of `pkg`, which must be the current package, one of its
  dependencies, or `core` / `std`. `import pkg;` alone is an error.
- Modules of a package may import each other; packages may not depend on each other in a cycle.
- Git dependencies are parsed but not fetched yet (E0405), so `sosu.lock` does not exist yet.
