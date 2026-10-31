# Contributing to Sosu

Thank you for your interest. Questions, bug reports and ideas go to the
[issues](https://github.com/Clement-Bonet/Sosu/issues); issues marked `good first issue` are a
good place to start.

## Rules of the project

- English for code, comments, compiler messages and documentation.
- The compiler is C99 with libc only: no other dependency. It must build without warnings with
  MSVC (`/W4 /WX`), gcc and clang (`-Wall -Wextra -pedantic -Werror`).
- The language is described by [docs/SPEC.md](docs/SPEC.md): a change of the language changes the
  spec in the same pull request.
- A new diagnostic gets a stable code, listed in [docs/errors.md](docs/errors.md) with an example,
  and a test.

## Making a change

1. Branch from `dev`: `feature/<name>`. Pull requests go into `dev`; `main` only receives `dev`
   (see [Branches](README.md#branches)).
2. Build and test (see [Building](README.md#building)): `build.bat test` on Windows, `make test`
   elsewhere.
3. Format the `.sosu` files you changed: `sosuc fmt <folder>`.
4. If you changed `lib/core` or `lib/std`, run `make doc` and commit `docs/REFERENCE.md` (the CI
   checks that it is up to date).
5. Open the pull request. The CI builds and tests it on Linux (gcc, clang), Windows (MSVC), macOS
   and Android; it is merged once everything passes.

## AI-assisted contributions

Much of Sosu was written with an AI coding assistant (see [How Sosu is made](README.md#how-sosu-is-made)),
so such contributions are welcome on the same terms as the others: say so in the pull request,
and make sure you understand, review and test what you submit.

## License

Contributions are dual licensed under the MIT license and the Apache License 2.0, as the project
(see [License](README.md#license)).
