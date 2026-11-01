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

## What remains (November 1, 2026)

Estimated again from the pace above. The planned versions: `0.2.0` when work 5 is done (it changes the language: the two limits below), `0.3.0` with the packages of work 4; `0.1.x` for fixes in between.

| # | Work | What remains | Output tokens |
|---|---|---|---|
| 4 | Standard library | The packages of §8.2 marked 1.0 not written yet (`crypto`, `compress`, `math.linear`, `testing` and `image` are done, §8.3): `audio` (SDL3, mixer, WAV, Ogg), `db` and `db.sqlite`, `platform.dialogs` | 250 to 450 k |
| 5 | Quality | Begun (E0219, E0416, E0371, the review fixes above); the fuzzing of the parser and of the semantic analysis, in the CI; the time of `sosuc` on large programs, measured in the CI; the two limits found while writing `std` (`void!E` as the type of a parameter or a variable, a pointer as a type argument); `git` and the C compiler run without the shell; the grammar and the error codes frozen, a compatibility policy | 200 to 350 k |
| 6 | Tooling | Begun: the reference of `core` and `std` is generated (`src/docgen`, not yet `sosuc doc` for any package); the rest unchanged | 200 to 350 k |
| 7 | Documentation | Done: the reference of `std` (`docs/REFERENCE.md`). Remains: the guide from the first program to a window, the examples of the packages of §8.2 | 150 to 300 k |
| 8 | Interface | unchanged | 150 to 250 k |
| 9 | Distribution | Done: version numbers checked against the tag, release notes, archives for Windows x64, Linux x64 and ARM64, macOS universal, with their SHA-256. Remains: installers, signed binaries | 60 to 150 k |

In all, about 1.0 to 1.85 million output tokens: 0.5 to 0.9 weeks of the Pro quota. With the quota renewed each week, the works would end in the second half of November, a new session for each work.

What tokens do not buy, and sets the pace as much:

- The tests by hand: the dialogs and the sound of `platform`, the examples run on an Android phone, iOS on an iPhone (a Mac is needed).
- The accounts and certificates of signed binaries (Apple developer program, a code-signing certificate for Windows), which cost money.
- The 0.x versions tried by others than their authors, for a few weeks, before 1.0 (§13.1).

A new session for each package or work spends much less than one long session, whose context is read again at each call.
