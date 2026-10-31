# Security

## Supported versions

Sosu is before 1.0: only the latest [release](https://github.com/Clement-Bonet/Sosu/releases)
receives fixes.

## Reporting a vulnerability

Please do not open a public issue. Use **Report a vulnerability** in the
[Security tab](https://github.com/Clement-Bonet/Sosu/security) of the repository, with the version,
the system, and the steps or the file that show the problem.

## What to know before using it

- Sosu is **not memory safe yet**: some dangling pointers are not caught (spec §7.7, in
  [docs/SPEC.md](docs/SPEC.md)). A program can have undefined behaviour without `unsafe`.
- Building a package runs code of its author: `sosuc` compiles the C sources and applies the
  `[link]` settings of its `sosu.toml`, and of the `sosu.toml` of its git dependencies. Add a git
  dependency only from a source you trust; `sosu.lock` pins the commit that was used.
- `sosuc` runs the C compiler and `git` through the system shell; the values of `sosu.toml` are
  checked before they reach a command (error E0416).
