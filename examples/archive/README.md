# Archive

An example of the package `compress` of [Sosu](../../README.md). No window, no network, nothing to type.

The package `compress` (spec §8.3) in a small task: the log of a day of a web server, compressed with gzip at levels 1, 6 and 9 and read back (`compress.gzip`), as a zlib stream (`compress.zlib`), with its CRC-32 and Adler-32 (`compress.checksum`). A copy with one byte changed is refused by its checksum, and a limit given to `decompress_limit` refuses what would grow larger than it.

What it writes is read by `gzip -d`; the log stays in memory.

Run it from the root of the repository:

```sh
sosuc run examples/archive
```

Its output is checked by the tests (`tests/program_tests.c`, against `tests/examples/archive.txt`).
