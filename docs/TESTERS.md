# Welcome, testers

Thank you for trying Sosu before 1.0. You are among the first people to use it besides its author, and what you find now can still change the language: after 1.0, it no longer can without breaking code (§13.1 of the [specification](SPEC.md)).

This page says what Sosu is today, how to install it, what to try, and how to tell us what you found. An hour or two is already useful, and so is any time you give it over the next few weeks.

## What you are trying

Sosu is a C-like language that compiles to C. Effects (`uses io`, `time`, `random`, `threads`) are in signatures and checked by the compiler; failures are values (`T!E`) and absence is `T?`, without exceptions or null pointers outside `unsafe`; every allocation goes through an arena. It comes with a standard library, packages (`net`, `image`, `audio`, `db`...) and a toolkit for windows written in Sosu itself, on SDL3.

Know these limits before you start, so that they do not surprise you:

- **It is a 0.x version.** The language and the libraries can still change, and a program written now may need changes later.
- **Sosu does not promise memory safety.** The compiler refuses a pointer that outlives its arena or its function (§6.1), but it does not follow `unsafe`, C code, lambdas, or calls through a function value: a program can have undefined behavior without writing `unsafe` (§7.7, [SECURITY.md](../SECURITY.md)).
- **The binaries are not signed.** Windows and macOS may warn you; the steps are below.
- **It needs a C compiler**: MSVC on Windows, `cc` (gcc or clang) elsewhere. `sosuc` writes C, and that compiler finishes the job.
- **Not tried yet**: iOS on a real iPhone; the dialogs and the sound on Linux and macOS. If you have one of these, your report is especially welcome.

## Installing

On Linux and macOS:

```bash
curl -fsSL https://github.com/Clement-Bonet/Sosu/releases/latest/download/install.sh | sh
```

On Windows, in PowerShell:

```powershell
irm https://github.com/Clement-Bonet/Sosu/releases/latest/download/install.ps1 | iex
```

Both install the latest release for your user, without administrator rights, and check its SHA-256. Then, in a new terminal:

```bash
sosuc --version
```

Other ways, all in [§1.1 of the guide](GUIDE.md): a `.deb` for Ubuntu and Debian, a setup for Windows, a `.pkg` for macOS, and the archives.

If your system complains:

- **Windows** (the setup only): SmartScreen may stop it. Click *More info*, then *Run anyway*.
- **macOS** (the `.pkg` or an archive downloaded by a browser): click *Open Anyway* in System Settings, under Privacy & Security. For an archive, `xattr -dr com.apple.quarantine <its folder>` also works. The scripts above avoid both.

To uninstall: `curl -fsSL https://github.com/Clement-Bonet/Sosu/releases/latest/download/install.sh | sh -s -- --uninstall` on Linux and macOS, or in PowerShell `& ([scriptblock]::Create((irm https://github.com/Clement-Bonet/Sosu/releases/latest/download/install.ps1))) -Uninstall`.

For Visual Studio Code, install the extension from the `.vsix` of [its releases](https://github.com/Clement-Bonet/VSCode-Sosu-Plugins/releases) (§1.5 of the guide):

```bash
code --install-extension sosu-0.3.0.vsix
```

## What to try

In this order, stopping wherever you like:

1. **The quick guide.** [QUICK_GUIDE.md](QUICK_GUIDE.md) goes from the first program to a window. Every example in it is run by our tests, so if one fails for you, that is a bug worth reporting.
2. **A program of your own.** Something you have written before in another language, a small tool, a puzzle, an exercise. This is where we learn the most: what was easy, what was missing, which error message lost you.
3. **A window.** Start from [examples/window](../examples/window) or [examples/form](../examples/form), and change it.
4. **A package.** Each one has a small example in `examples/` ([§22.9 of the guide](GUIDE.md) lists them): `fetch` for `net`, `thumbnail` for `image`, `bookshelf` for `db`, `archive` for `compress`, `checksums` for `crypto`...
5. **A larger program.** The `showcase/` folder holds complete applications (a download manager, a spreadsheet, a music sequencer...). Read one, build it, break it.

Clone the repository to get the examples:

```bash
git clone https://github.com/Clement-Bonet/Sosu
cd Sosu/examples/window
sosuc run
```

## What we would like to hear

Anything, but these questions help most:

- Where did you get stuck, and for how long?
- Which error message did not tell you what to do?
- What did you expect to exist and did not find (in the language, the library, the tools)?
- What felt heavier than it should, compared with the languages you know?
- Did anything crash: `sosuc` itself, the C compiler on the code `sosuc` wrote, or your program?
- Did the guides say something false, or skip a step?

A short "it all worked" is also useful: say on which system.

## How to report

Open an [issue](https://github.com/Clement-Bonet/Sosu/issues), one per problem, with:

- the output of `sosuc --version`, your system, and your C compiler (`cl`, `gcc --version` or `clang --version`);
- the smallest code that shows the problem, and the command you ran;
- what you expected, and what happened, with the full message.

Impressions that are not a single problem (what you liked, what you would change) can go in one issue titled "Feedback: ...".

A security problem, such as a way to make `sosuc` run something it should not, never goes in a public issue: use *Report a vulnerability* in the Security tab, as [SECURITY.md](../SECURITY.md) says.

Thank you again.
