# Bookshelf

An example of the packages `db` and `sqlite` of [Sosu](../../README.md). No window, no network, nothing to type.

The packages `db` and `sqlite` (spec §8.3) in a small task: the books of a small library and their loans, in a SQLite database in memory (`sqlite.memory`; `sqlite.open` takes a file). A statement prepared once and run for each book in a transaction, a search whose text stays a parameter (a quote in it changes nothing), a join, a loan refused by a foreign key, and a transaction rolled back.

The function that does the work returns `bool!db.DbError`, so that each call uses `try`; `main` prints the error and what SQLite said of it.

Run it from the root of the repository:

```sh
sosuc run examples/bookshelf
```

Its output is checked by the tests (`tests/program_tests.c`, against `tests/examples/bookshelf.txt`).
