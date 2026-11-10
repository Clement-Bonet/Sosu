# Checksums

An example of the package `crypto` of [Sosu](../../README.md). No window, no network, nothing to type.

The package `crypto` (spec §8.3) in a small task: the SHA-256 of a file, as `sha256sum` prints it, computed at once and by parts; a download link signed with HMAC-SHA256 (`crypto.hmac`, `crypto.base64` for URLs) and checked again, refused once expired or changed; and a session token from the generator of the system (`crypto.secure`).

The signatures are compared with `hmac.equal`, in a time that does not depend on where they differ. The key is written in the code to keep the example short: a real one comes from `secure.fill` and stays out of the sources.

Run it from the root of the repository:

```sh
sosuc run examples/checksums
```

Its output is checked by the tests (`tests/program_tests.c`, against `tests/examples/checksums.txt`).
