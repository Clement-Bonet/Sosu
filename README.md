# Sosu

![Sosu: simple, safe, native](docs/Sosu.png)

A native, low-level, statically typed language, and its compiler `sosuc` (C99, libc only).
The specification is in [docs/SPEC.md](docs/SPEC.md).

English is the working language of the project: code, comments, compiler messages and documentation.

## Status

| Step (roadmap §13) | Status |
|---|---|
| 1. Lexer | done |
| 2. Parser -> AST, packages and imports | done (local dependencies only, no git yet) |
| 3. Semantic analysis | done for the language of §5 to §7, except arenas (`Arena`, `arena.new`), which come with step 6 |
| 4. C code generation | to do |

For now, `sosuc check` loads a package, its dependencies and every imported module, then checks
declarations and function bodies: types, mutability, effects, `unsafe`, `match` exhaustiveness, `try`,
contracts (semantic analysis runs once the program parses without errors). `sosuc lex <file>` prints the tokens of a file and `sosuc parse <file>`
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
  types       semantic types (interned)
  sema        semantic analysis of declarations
  sema_body   semantic analysis of function bodies
src/sosuc/    command-line driver
lib/          the `core` and `std` packages (empty modules for now)
tests/        unit tests, .sosu samples taken from the spec, test packages
docs/         specification and error codes
```

`sosuc` looks for `core` and `std` in `$SOSU_HOME/lib`, or else in `../lib` next to the executable
(which is `lib/` for `build/sosuc`).

## Documentation

- [docs/SPEC.md](docs/SPEC.md): the language. Points the first draft left open were settled while
  writing the compiler and are part of it now: literals (§3.1), operators and struct literals (§5.1),
  `match` (§7.1), packages and imports (§8.1), the full grammar (§12), decisions and open
  questions (§14).
- [docs/errors.md](docs/errors.md): every error code, with an example.

## Compiler notes

- Diagnostics: one stable code each, a position, an explanation and, when possible, the fix. After
  an error, the lexer and the parser resume (end of statement, member or declaration, or closing
  bracket), so one mistake gives one message.
- The syntax trees live in an arena freed at once; names point into the source text.
- Only libc is used: the compiler cannot list a folder, so it loads the modules reachable by
  `import` from the entry file.
