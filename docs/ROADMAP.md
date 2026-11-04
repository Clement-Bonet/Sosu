# Sosu roadmap: estimates

The content of each work of version 1.0, and what is done, is in the specification ([SPEC.md](SPEC.md), §13.1). This file keeps what does not belong to the reference of the language: how long each work may take, and how that was measured.

## How the estimates were made

The estimates are measured on steps 1 to 11, written with Claude Code: they took 6 days (October 21 to 26, 2026), about 49,000 lines, 3.2 million output tokens and 1.6 billion tokens read from the cache. A week of the Pro plan's quota held about 2.2 million output tokens. The effort is relative to steps 1 to 11 (100); the days and tokens add a quarter, since a larger code base and specification make each call heavier:

| # | Work | Effort | Days | Output tokens | Weekly quota |
|---|---|---|---|---|---|
| 1 | Language | 12 | 1 to 1.5 | 400 to 600 k | 18 to 27 % |
| 2 | Concurrency | 8 | 0.5 to 1 | 250 to 400 k | 11 to 18 % |
| 3 | C sources in a package | 2 | 0.2 | 60 to 100 k | 3 to 4 % |
| 4 | Standard library | 25 | 1.5 to 2.5 | 0.8 to 1.2 M | 36 to 54 % |
| 5 | Quality | 10 | 0.5 to 1 | 300 to 500 k | 13 to 22 % |
| 6 | Tooling | 8 | 0.5 to 1 | 250 to 400 k | 11 to 18 % |
| 7 | Documentation | 8 | 0.5 to 1 | 200 to 400 k | 9 to 18 % |
| 8 | Interface | 5 | 0.5 | 150 to 250 k | 7 to 11 % |
| 9 | Distribution | 4 | 0.5 | 120 to 250 k | 5 to 11 % |

In all, an effort of about 82, 6 to 9 days of work and 2.5 to 4.5 million output tokens: 1.1 to 1.8 weeks of the Pro quota, so 2 to 3 weeks of calendar, the quota and not the work setting the pace. The standard library is the least certain line: it depends on how many modules are written in Sosu. Most of the tokens are context read again at each call (about 490,000 per call on steps 1 to 11, whose sessions lasted up to 3 days): a new session for each work spends much less.

## Measured since

- Works 1 to 3, then the modules of `std` of work 4 (§8.0), were written on October 27 and 28, 2026.
- The session of October 28 wrote the modules of `std` (about 5,300 lines), then the hardening that a review asked for (the depth of the parser, the values of `sosu.toml`, the pointers returned into a function, §7.7, §8.1, §12) and its smaller points (about 420 lines). Its context reached about 490,000 tokens per call, and the weekly quota of the Pro plan reached 86 %. Its output tokens are not known exactly: the tools show the share of the quota and the size of the context, not the tokens written.
- The packages `crypto` and `compress` (about 1,600 lines of Sosu, and a test program each) were written on the evening of October 28, in a session of their own: about 2 % of the weekly quota (86 % to 88 %), much less than estimated, since the session started fresh.
- `math.linear` (about 660 lines) followed in the same session, on October 28.
- October 29 to 31, in one long session and a few short ones, with the weekly quota between 93 % and 96 %: the releases (`v0.1.0` to `v0.1.3`, built and published from a tag), the playground (the compiler in WebAssembly, on GitHub Pages), the reference of `core` and `std` generated from their comments (`make doc`), `CONTRIBUTING.md` and `SECURITY.md`, the poster drawn by Sosu (`examples/poster`, with `render.png` writing PNG files), and the points of two external reviews: links never followed when a folder is removed, the size of a canvas and of a PNG bounded, the size guard of the compiler's arena tested, `parser.c` and `sema_body.c` split by domain.
- November 1, in a session of its own: the package `testing` (comparisons that show what differs, benchmarks measured by `sosuc test --bench`, data files, golden files and temporary folders), and `assert(a == b)` showing both sides.
- November 1, in another session of its own: the package `image` (about 2,400 lines of Sosu, and the test program `images`): the PNG reader moved out of `render`, a PNG writer, JPEG read and written (baseline, checked against the Windows Imaging Component both ways), crop, resize (bilinear, Lanczos), rotate and flip; `render` uses it.
- November 1, in a third session of its own: the package `audio` (about 1,200 lines of Sosu, the test program `sounds` and `examples/sound`): the mixer, resampling, WAV read and written, Ogg Vorbis read by stb_vorbis (given as C sources, five lines changed against two faults of its comment header), playing and recording on SDL3 without a callback. Nothing to hear in the tests: their files are made by a script of Python, the Ogg file by a minimal encoder checked against the Vorbis I specification.
- November 1, in a fourth session of its own: the packages `db` (the interface, prepared statements, transactions, a pool of connections between threads) and `sqlite` (`db.sqlite` of §8.2, on the amalgamation of SQLite 3.54.1 given as C sources): about 850 lines of Sosu and 190 of C around the vendored sources, and the test program `databases` (about 450 lines).
- November 1, in a fifth session of its own: `platform.dialogs` (about 400 lines of Sosu and 120 of tests in the package, and `examples/dialogs`): open one file or several, save, choose a folder, message boxes, on SDL3; the answer of a file dialog, given by SDL on a thread of its own, comes back to the loop of `app.run` as an event. Work 4 is done; the dialogs are still to be tried by hand.
- November 1, in a sixth session of its own: work 5. `git` and the C compiler run with their arguments, never through a shell (`Command` in `src/lib/shell.c`: CreateProcess, or fork and execvp; the environment of MSVC read once from `vcvarsall.bat`; Gradle run by `java` from its wrapper, the clang of the NDK with `--target`); `void!E` a value, a type such as `ui8*` given to a call; the fuzzing (`tests/fuzz.c`: 40 000 runs of libFuzzer under ASan, with MSVC, found no crash); the time of `sosuc` measured (`tests/bench.c`), which found four lookups that grew with the program, made constant by hash tables: 318 000 lines took 6.0 s, now 1.1 s; the grammar and the error codes frozen, the compatibility policy (§13.2).

## What remains (November 1, 2026)

Estimated again from the pace above. The versions: `0.2.0` with the packages of work 4, which was done first (`render.image` moved to the package `image`); `0.3.0` when work 5 is done (it changes the language: the two limits below); `0.2.x` for fixes in between.

| # | Work | What remains | Output tokens |
|---|---|---|---|
| 4 | Standard library | Done (§8.0, §8.3, §10.11), the dialogs and the sound tried by hand on Windows 11 | 0 |
| 5 | Quality | Done (§13.1, §13.2): the CI of 0.3.0 passes on Linux (gcc, clang), macOS (with the iOS simulator), Windows and Android, with the fuzzing (`make fuzz`, and `make fuzz-ogg` for stb_vorbis) and `make bench` | 0 |
| 6 | Tooling | Begun: the reference of `core`, `std` and the packages of `packages/` is generated (`src/docgen`, not yet `sosuc doc` for any package); the rest unchanged | 200 to 350 k |
| 7 | Documentation | Done: the reference of `std` and of the packages (`docs/REFERENCE.md`). Remains: the guide from the first program to a window, the examples of the packages of §8.2 | 150 to 300 k |
| 8 | Interface | unchanged | 150 to 250 k |
| 9 | Distribution | Done: version numbers checked against the tag, release notes, archives for Windows x64, Linux x64 and ARM64, macOS universal, with their SHA-256. Remains: installers, signed binaries | 60 to 150 k |
| 10 | Memory safety | Added on November 4, 2026 (§13.1). The regions are done (§6.1); what they do not follow remains, and how to close it without lifetimes in the types is open | not estimated |

In all, about 0.75 to 1.4 million output tokens: 0.35 to 0.65 weeks of the Pro quota. With the quota renewed each week, the works would end in the second half of November, a new session for each work. This total and this date do not count work 10: its design has to be settled first.

What tokens do not buy, and sets the pace as much:

- The tests by hand: Beatbox runs on an Android phone (built with `--release`: the sound, the dialog of its lists, landscape); iOS on an iPhone remains (a Mac is needed). The dialogs and the sound are tried on Windows 11, not yet on Linux nor macOS.
- The accounts and certificates of signed binaries (Apple developer program, a code-signing certificate for Windows), which cost money.
- The 0.x versions tried by others than their authors, for a few weeks, before 1.0 (§13.1).

A new session for each package or work spends much less than one long session, whose context is read again at each call.

## After 1.0: moving away from C (November 4, 2026)

Steps 12 and 14 of the specification (§13) come after 1.0 (§13.1). Their estimates are in weeks of full-time work with Claude Code, and they are not measured: nothing of this kind was written yet, and the pace of steps 1 to 11 does not apply, since a native backend is debugged at the level of instructions, for each target.

| Work | Today | Weeks |
|---|---|---|
| Intermediate representation | `src/lib/codegen.c` (2,733 lines) goes to C with no IR of its own, although §11.3 names one | 1 to 2 |
| x86-64 backend, for one system | Nothing: instruction selection, registers, calling conventions, object file | 3 to 4 |
| ARM64 and the two other systems | ELF, PE and Mach-O, with linking | 3 to 4 |
| Debug information | `#line` and the C compiler give it; DWARF and PDB would have to be written | 1 to 2 |
| Runtime and `std` | 1,778 lines of C in `lib/runtime`, 10 modules of `std` with `extern "C"` | 1 |
| Self-hosting | About 25,700 lines of C rewritten in Sosu, then the bootstrap | 1 to 2 |

In all, 10 to 15 weeks, so 3 to 4 months, for a compiler without C on the desktop.

What stays in C or depends on it:

- SQLite (about 271,000 lines) stays as it is. SDL3 could only be replaced by a native layer for each system, several months more. `stb_vorbis` (about 5,700 lines) is replaced only if there is a need.
- The C calling convention stays, to call these libraries. Without libc, Linux can use system calls directly; Windows goes through its system DLLs, and macOS through `libSystem`.
- On mobile, the NDK, Xcode and signing remain mandatory (§11.2).

The main cost is not in the table: the programs of a new backend will run slower than those of gcc, clang or MSVC with optimizations, for a long time. Self-hosting is the cheapest work and does not depend on the backend: `sosuc` would be written in Sosu and would still generate C.

If the need is only to install no C compiler, shipping a small one with `sosuc` costs much less. TinyCC is the obvious candidate, not tried yet: it is under the LGPL, and covers macOS and ARM64 poorly.
