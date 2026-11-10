# Wordcount

An example of the package `testing` of [Sosu](../../README.md).

The package `testing` (spec §8.3) in a small task: a library that counts the words of a text, and its tests (`test` blocks, run by `sosuc test`). `testing.check` compares texts, lists and numbers and prints what differs when a test fails; `testing.fixture` reads `tests/data/poem.txt` from the folder of the package and compares the report with the golden file `tests/data/report.txt`; `testing.bench` measures the counting.

```sh
sosuc test examples/wordcount            # the tests
sosuc test --bench examples/wordcount    # and the time of the benchmark
sosuc test --update examples/wordcount   # writes the golden file again, after a change of the report
```

Run them from the root of the repository. Their output is checked by the tests (`tests/program_tests.c`, against `tests/examples/wordcount.txt`).
