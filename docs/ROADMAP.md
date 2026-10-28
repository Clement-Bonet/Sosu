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

## What remains (October 28, 2026)

Estimated again from the pace above: the modules of `std` took about the time planned, about 10 of the 25 of work 4.

| # | Work | What remains | Output tokens |
|---|---|---|---|
| 4 | Standard library | The packages of §8.2 marked 1.0 not written yet (`crypto`, `compress` and `math.linear` are done, §8.3): `testing`, `image` (PNG written, JPEG, resize, crop, rotate), `audio` (SDL3, mixer, WAV, Ogg), `db` and `db.sqlite`, `platform.dialogs` | 350 to 650 k |
| 5 | Quality | Begun (E0219, E0416, E0371); the fuzzing of the parser and of the semantic analysis, the grammar and the error codes frozen, a compatibility policy, the time of `sosuc` on large programs, the two limits found while writing `std` (`void!E` as the type of a parameter or a variable, a pointer as a type argument) | 250 to 400 k |
| 6 | Tooling | unchanged | 250 to 400 k |
| 7 | Documentation | unchanged | 200 to 400 k |
| 8 | Interface | unchanged | 150 to 250 k |
| 9 | Distribution | unchanged | 120 to 250 k |

In all, about 1.35 to 2.35 million output tokens: 0.6 to 1.1 weeks of the Pro quota. With about 10 % of the quota left until October 31 (`testing`, in a session of its own), the works would end around mid-November, if each week of quota is used.

What tokens do not buy, and sets the pace as much:

- The tests by hand: the dialogs and the sound of `platform`, the examples run on an Android phone, iOS on an iPhone (a Mac is needed).
- The accounts and certificates of signed binaries (Apple developer program, a code-signing certificate for Windows), which cost money.
- The 0.x versions tried by others than their authors, for a few weeks, before 1.0 (§13.1).

A new session for each package or work spends much less than one long session, whose context is read again at each call.
