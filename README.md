# Sosu

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
| 7. Tooling | done: `run`, `test`, `fmt`, JSON diagnostics, contracts, `--release`, a language server (`sosuc lsp`) |
| 8. `platform` / `render` / `ui` / `i18n` | done on the desktop: window, keyboard, mouse, touch and text events, drawing in a canvas of pixels, text (TrueType fonts read and drawn in Sosu, and a built-in 8 by 8 font with the Latin accented letters), widgets in immediate mode (`ui`), `.ui.sosu` interface files with themes, styles and translations; touch and mobile events written, Android and iOS builds are step 9 |

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
sosuc i18n extract examples/workspace -o examples/workspace/locales/messages.locale.default.sosu   # the texts to translate (spec §10.7)
sosuc lsp                           # the language server of editors (spec §11.4), on stdin and stdout
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
  json        JSON reader and writer (for the language server)
  lsp         the language server: diagnostics while typing, hover, definition, completion
  i18n        the texts to translate of a package, for its catalogs (`sosuc i18n extract`)
src/sosuc/    command-line driver
lib/          the `core` and `std` packages, written in Sosu (std.io, std.time, std.random), and the C runtime
packages/     platform (window and events, on SDL3), render (pixels, shapes, text, PNG images), ui (widgets),
              i18n (catalogs of translations)
examples/     window (shapes, keyboard and mouse), form (widgets), editor and workspace (title bars, a text
              area, tables, translations), themes (themes and styles), containers (grids, groups, tabs,
              trees, inputs)
tests/        unit tests, .sosu samples from the spec, test packages, programs run end to end,
              and window (a window test run by the CI)
docs/         specification and error codes
```

`sosuc` looks for `core` and `std` in `$SOSU_HOME/lib`, or else in `../lib` next to the executable
(which is `lib/` for `build/sosuc`), also when it is found in the `PATH`.

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

Four packages, written in Sosu:

- `packages/platform` opens a window with SDL3 and hands the program its events (keyboard, mouse,
  typed text, input methods, touch screens, the safe area, suspend and resume) and a canvas of pixels
  each frame. The program keeps its state in a class that implements `app.Handler`. A window can
  draw its own title bar (spec §10.5), and a program linked with `--release` has no console on
  Windows.
- `packages/render` draws in that canvas: rectangles (with rounded corners), borders, lines,
  transparency, PNG images, text in a TrueType font or a built-in 8 by 8 font.
- `packages/ui` adds widgets in immediate mode: at each frame, the program describes its interface
  from its state (`if (f.button("Greet")) { ... }`). Most programs describe it in `.ui.sosu`
  files, which `sosuc` turns into those calls.
- `packages/i18n` reads the catalogs of translations, `messages.locale.fr.sosu` (the content of
  gettext); `sosuc i18n extract` writes the texts to translate.

The widgets of `ui`:

| Kind | Widgets |
|---|---|
| Text and buttons | `Text`, `Title`, `Image`, `Button`, `Checkbox`, `Radio`, `Space` |
| Text input | `TextField` (masks, formats, placeholders), `TextArea` (lines, undo), `NumberField`, `MoneyField`, `DateField` (a calendar) |
| Choices and values | `Dropdown`, `Slider`, `Progress` |
| Lists | `List` (and long lists, only the items in view described), `Table` (sorting, resizable columns), `Tree` |
| Layout | `Column`, `Row`, `Grid` (pixels, `Auto`, `Fill`), `Group`, `Tabs`, `Scroll` |
| Windows | `TitleBar`, `MenuBar`, `Menu`, `ContextMenu`, `Dialog`, tooltips |

They have identities that survive an `if` or an insertion in a loop, the keyboard (Tab, the keys
of each widget, Alt and a letter), themes and styles written in `.ui.sosu` files (spec §10.7), and
their literal texts are translated (spec §10.7).

The examples:

- `examples/window`: a rectangle moved with the arrows and the mouse;
- `examples/form`: a small form with a text field, a checkbox and buttons;
- `examples/editor`: its own title bar (a search field, a dropdown, a button), over a text area;
- `examples/workspace`: in the manner of VS Code: its icon, menus, a search field and two dropdowns
  in the title bar, over a tree of files, the details of the file chosen in tabs, a table of all the
  files and an output pane; in English or French (`SOSU_LANG=fr`), light or dark as the system
  (`SOSU_THEME`). With `SOSU_SYSTEM_TITLE_BAR` set, it keeps the title bar of the system, and its
  own becomes a tool bar;
- `examples/themes`: themes and styles of a `.ui.sosu` file, following the system;
- `examples/containers`: a grid, groups, a tree of folders, and every input widget.

On Windows:

```bat
powershell -ExecutionPolicy Bypass -File packages\platform\fetch-sdl3.ps1   :: once: SDL3 3.4.18, checked by SHA-256
build\sosuc run examples\window
build\sosuc run examples\containers
build\sosuc build --release examples\workspace   :: optimized, without a console
```

The script downloads SDL3 into `packages/platform/vendor/` (ignored by Git). On Linux,
`platform` links the SDL3 installed on the system (3.2 or later):

```sh
sudo apt install libsdl3-dev    # Ubuntu 25.04 or later, Debian 13 or later
./build/sosuc run examples/window
./build/sosuc run examples/containers
./build/sosuc run examples/workspace
```

Programs are run from the root of the repository, where they find their font and their catalogs.
`sosuc run` builds without optimizations: build with `--release` to measure the speed of a
program.

The tests of `render`, `ui` and `i18n` need no screen (`sosuc test packages/ui`), and
`tests/programs` drives screens of `.ui.sosu` files without a window. `tests/window` opens a window,
draws one frame and closes itself: the CI runs it on Linux and Windows with
`SDL_VIDEO_DRIVER=dummy`, which needs no screen. macOS is not checked yet.

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
  interface of `platform`, `render` and `ui` (§10.4), title bars (§10.5), the foundations of the
  widgets (§10.6), translations, themes and styles (§10.7), containers (§10.8), the rest of the
  widgets, input methods and touch screens (§10.9), the full grammar (§12), decisions and open
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
