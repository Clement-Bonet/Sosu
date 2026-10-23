# Sosu

![Sosu: simple, safe, native](docs/Sosu.png)

A native, low-level, statically typed language, and its compiler `sosuc` (C99, libc only).
The specification is in [docs/SPEC.md](docs/SPEC.md).

English is the working language of the project: code, comments, compiler messages and documentation.

## Status

| Step (roadmap §13) | Status |
|---|---|
| 1. Lexer | done |
| 2. Parser -> AST, packages and imports | done (local and git dependencies, `sosu.lock`) |
| 3. Semantic analysis | done for the language of §5 to §7 |
| 4. C code generation | done: classes with virtual tables, interfaces, `as?` |
| 5. C interop, SDL3 window | done: `extern "C"`, linking (`[link]` in sosu.toml), the `platform` package and the window example (Windows, Linux) |
| 6. Arenas, defer, move, unsafe, checks | done (moves of `Arena` and what contains it, `.copy()`) |
| 7. Tooling | done: `run`, `test`, `fmt`, JSON diagnostics, contracts, `--release` |
| 8. `platform` / `render` / `ui` | done on the desktop: window, keyboard, mouse and text events, drawing in a canvas of pixels, text (TrueType fonts read and drawn in Sosu, and a built-in 8 by 8 font), widgets in immediate mode (`ui`), `.ui.sosu` interface files (no styles yet) |

`sosuc run` compiles a package to C, then to a native program, and runs it. The generated C keeps
`#line` directives to the `.sosu` files, and checks integer overflow, division by zero, indexes and
contracts at run time. `sosuc check` checks a package without compiling it. A `.ui.sosu` interface
file is a module like the others: `sosuc` turns its windows and components into functions that call
the widgets of `ui` (spec §10.2).

## Building

Windows (MSVC, located automatically with `vswhere`):

```bat
build.bat        :: builds build\sosuc.exe and the test programs
build.bat test   :: builds, then runs the tests
```

Linux / macOS (gcc or clang; checked on every push by GitHub Actions, see `.github/workflows/ci.yml`):

```sh
make
make test
```

## Usage

```
sosuc run tests/programs/basics     # a package folder (default: the current folder)
sosuc run tests/programs/classes --release   # optimized: no overflow checks nor contracts
sosuc test tests/programs/testing   # runs its `test` blocks
sosuc build tests/programs/basics   # writes build/basics.c and build/basics(.exe)
sosuc check tests/packages/app      # checks without compiling
sosuc fmt packages/platform         # formats in place (spec §7.6); --check only reports
sosuc lex tests/samples/hello.sosu  # tokens of a file (debugging)
sosuc parse tests/samples/hello.sosu --format=json   # syntax tree; JSON diagnostics on stderr
```

The C compiler is MSVC on Windows (found automatically, no Developer Prompt needed), `cc` elsewhere,
or the one named by `SOSU_CC` (gcc or clang style).

Exit codes of `sosuc`: 0 = success, 1 = errors in the source code (or a failed test), 2 = usage, I/O
or C compiler error. `sosuc run` returns the exit code of the program; a run-time error (overflow,
index out of bounds, failed contract) stops it with the code 101.

## Layout

```
src/lib/      libsosu: library shared by all the tools (spec §11.4)
  common      allocation, formatting, dynamic arrays
  utf8        UTF-8 decoding
  source      source files, line / column positions
  diag        diagnostics (text and JSON, spec §7.5)
  fmt         the canonical formatter (spec §7.6)
  token       token kinds
  lexer       lexical analysis
  arena       arena allocator for compiler data
  ast         syntax tree and its printer
  parser      syntax analysis (recursive descent)
  path        file path helpers
  shell       commands run through the system shell (C compiler, git)
  manifest    sosu.toml parser
  git         git dependencies and sosu.lock
  program     loading of packages, dependencies and imported modules
  types       semantic types (interned)
  sema        semantic analysis of declarations
  sema_body   semantic analysis of function bodies
  codegen     C code generation
src/sosuc/    command-line driver
lib/          the `core` and `std` packages, written in Sosu (std.io, std.time, std.random), and the C runtime
packages/     platform (window and events, on SDL3), render (pixels, shapes, text), ui (widgets)
examples/     window (shapes, keyboard and mouse), form (widgets)
tests/        unit tests, .sosu samples from the spec, test packages, programs run end to end,
              and window (a window test run by the CI)
docs/         specification and error codes
```

`sosuc` looks for `core` and `std` in `$SOSU_HOME/lib`, or else in `../lib` next to the executable
(which is `lib/` for `build/sosuc`).

## Dependencies

A package lists its dependencies in `sosu.toml` (spec §8.1), from a folder or a Git repository:

```toml
[dependencies]
shared = { path = "../shared" }
ui = { git = "https://github.com/someone/sosu-ui", tag = "v0.1.0" }
```

Git dependencies are cloned with `git` into `build/deps/` of the package, and `sosu.lock` keeps
their exact commit: keep it in version control.

## Windows and widgets (spec §10)

Three packages, written in Sosu:

- `packages/platform` opens a window with SDL3 and hands the program its events (keyboard, mouse,
  typed text) and a canvas of pixels each frame. The program keeps its state in a class that
  implements `app.Handler`.
- `packages/render` draws in that canvas: rectangles, borders, lines, transparency, and text with
  a built-in 8 by 8 font.
- `packages/ui` adds widgets in immediate mode: at each frame, the program describes its interface
  from its state (`if (f.button("Greet")) { ... }`).

`examples/window` moves a rectangle with the arrows and the mouse; `examples/form` is a small form
with a text field, a checkbox and buttons; `examples/editor` draws its own title bar, with a
search field, a dropdown and a button in it, which still moves, maximizes and closes the window.
On Windows:

```bat
powershell -ExecutionPolicy Bypass -File packages\platform\fetch-sdl3.ps1   :: once: SDL3 3.4.18, checked by SHA-256
build\sosuc run examples\window
build\sosuc run examples\form
```

The script downloads SDL3 into `packages/platform/vendor/` (ignored by Git). On Linux,
`platform` links the SDL3 installed on the system (3.2 or later):

```sh
sudo apt install libsdl3-dev    # Ubuntu 25.04 or later, Debian 13 or later
./build/sosuc run examples/window
./build/sosuc run examples/form
./build/sosuc run examples/editor
```

The tests of `render` and `ui` need no screen (`sosuc test packages/render`, `sosuc test
packages/ui`). `tests/window` opens a window, draws one frame and closes itself: the CI runs it on
Linux and Windows with `SDL_VIDEO_DRIVER=dummy`, which needs no screen. macOS is not checked yet.

## Branches

- `main`: stable. It only receives `dev`, through a pull request.
- `dev`: integration of the finished features.
- `feature/<name>`: one branch per feature, created from `dev` and merged back into `dev`
  through a pull request.

The CI (`.github/workflows/ci.yml`) checks every pull request into `dev` or `main` on Linux
(gcc and clang) and Windows (MSVC), so that a branch is merged only once it builds and passes the
tests everywhere.

## Documentation

- [docs/SPEC.md](docs/SPEC.md): the language. Points the first draft left open were settled while
  writing the compiler and are part of it now: literals (§3.1), operators and struct literals (§5.1),
  moves and `.copy()` (§6), `match` (§7.1), packages, imports and `sosu.lock` (§8.1), the v0
  interface of `platform`, `render` and `ui` (§10.4), the full grammar (§12), decisions and open
  questions (§14).
- [docs/errors.md](docs/errors.md): every error code, with an example.

## Compiler notes

- Diagnostics: one stable code each, a position, an explanation and, when possible, the fix. After
  an error, the lexer and the parser resume (end of statement, member or declaration, or closing
  bracket), so one mistake gives one message.
- The syntax trees live in an arena freed at once; names point into the source text.
- Only libc is used: the compiler cannot list a folder, so it loads the modules reachable by
  `import` from the entry file. It calls the C compiler and `git` through the system shell, like a
  user would.

## License

Licensed under either of

- the Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE)), or
- the MIT license ([LICENSE-MIT](LICENSE-MIT)),

at your option. The font and the other works that the repository contains or uses keep their own
licenses: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Unless you explicitly state otherwise, any contribution intentionally submitted for inclusion in
the work by you, as defined in the Apache-2.0 license, shall be dual licensed as above, without
any additional terms or conditions.
