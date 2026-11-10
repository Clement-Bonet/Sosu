# Orbit

An example of the package `math` of [Sosu](../../README.md). No window, no network, nothing to type.

The package `math` (spec §8.3) in a small task: a planet around its sun and a moon around the planet, placed by `linear.Transform`s one inside the other (`then`), seen by a camera (`look_at`, `perspective`) and projected to the pixels of a screen of 640 by 480, as a 3D program does before it draws. The same moon is placed again with matrices, and the inverse of the view gives back the position of the camera.

Operators are methods (`a.mul(b)` applies `b` first): Sosu has no operator overloading.

Run it from the root of the repository:

```sh
sosuc run examples/orbit
```

Its output is checked by the tests (`tests/program_tests.c`, against `tests/examples/orbit.txt`).
