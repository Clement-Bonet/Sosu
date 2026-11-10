# Thumbnail

An example of the package `image` of [Sosu](../../README.md). No window, no network, nothing to type.

The package `image` (spec §8.3) in a small task: a sunset drawn pixel by pixel, saved as PNG (every pixel kept) and as JPEG (what the eye sees), both read back; then a thumbnail: cropped, resized with Lanczos, turned a quarter, made gray, and saved to `examples/thumbnail/build/thumbnail.png`. A text read as a PNG file is refused, with why.

Run it from the root of the repository:

```sh
sosuc run examples/thumbnail
```

Its output is checked by the tests (`tests/program_tests.c`, against `tests/examples/thumbnail.txt`).
