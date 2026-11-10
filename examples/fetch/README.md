# Fetch

An example of the package `net` of [Sosu](../../README.md). No window, nothing to type, and no network: it talks to a server of its own, on this machine.

The package `net` (spec §8.3) in a small task, without a network: `main` listens on a port of this machine that the system chooses (`tcp.listen`) and answers HTTP requests; a client on a thread of its own (`std.thread`) asks it with `net.http`: a text, a moved page whose redirect is followed, a page not found, and a form posted by a request made field by field. The client prints the status, the type and the body of each answer, and the path of the URL that answered (`net.url`).

Against a real server, the same calls take an `https://` address: the certificate is checked against the roots of the system (spec §8.3).

Run it from the root of the repository:

```sh
sosuc run examples/fetch
```

Its output is checked by the tests (`tests/program_tests.c`, against `tests/examples/fetch.txt`).
