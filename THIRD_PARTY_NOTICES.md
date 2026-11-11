# Third-party components

Sosu itself is licensed under the MIT license or the Apache License 2.0, at your option (see
`LICENSE-MIT` and `LICENSE-APACHE`). The repository also contains or uses the following works,
under their own licenses.

## Noto Sans

- Files: `packages/render/fonts/NotoSans-Regular.ttf`, and the same file in `assets/` of each
  application of `showcase/` (with its `OFL.txt`), with the Bold, Italic and BoldItalic faces in
  `showcase/notes/assets/` and `showcase/sheets/assets/`, and the Bold face in `showcase/queens/assets/`
- Source: <https://github.com/notofonts/notofonts.github.io> (unhinted TrueType)
- Copyright 2022 The Noto Project Authors (<https://github.com/notofonts/latin-greek-cyrillic>)
- License: SIL Open Font License, Version 1.1, in `packages/render/fonts/OFL.txt`. The font may
  be used, embedded and redistributed with programs; it may not be sold by itself, and a modified
  version must not use the name "Noto".

## Noto Serif

- Files: `showcase/notes/assets/NotoSerif-Regular.ttf`, `-Bold`, `-Italic` and `-BoldItalic`
- Source: <https://github.com/notofonts/notofonts.github.io> (unhinted TrueType)
- Copyright 2022 The Noto Project Authors (<https://github.com/notofonts/latin-greek-cyrillic>)
- License: SIL Open Font License, Version 1.1, in `showcase/notes/assets/OFL.txt`

## Font Awesome Free

- Files: `showcase/beatbox/assets/fa-solid-900.ttf` (version 6.5.2), the icons of Beatbox,
  `showcase/queens/assets/fa-solid-900.ttf`, the icons of Queens, and
  `showcase/courier/assets/fa-solid-900.ttf`, the icons of Courier
- Source: <https://fontawesome.com> (Font Awesome Free, desktop font files)
- Copyright (c) 2024 Fonticons, Inc., with the Reserved Font Name "Font Awesome"
- License: SIL Open Font License, Version 1.1, for the font files, in
  `showcase/beatbox/assets/FA-LICENSE.txt`, `showcase/courier/assets/FA-LICENSE.txt` and `showcase/queens/assets/FontAwesome-LICENSE.txt` (the icons as SVG and JS files are under CC BY 4.0, and
  the code under MIT; neither is given here)

## Montserrat

- Files: `examples/poster/fonts/Montserrat-ExtraBold.ttf`, `examples/poster/fonts/Montserrat-Bold.ttf`
- Source: <https://github.com/JulietaUla/Montserrat> (static TrueType, through Google Fonts)
- Copyright 2024 The Montserrat.Git Project Authors (<https://github.com/JulietaUla/Montserrat.git>)
- License: SIL Open Font License, Version 1.1, in `examples/poster/fonts/OFL-Montserrat.txt`

## JetBrains Mono

- Files: `examples/poster/fonts/JetBrainsMono-Regular.ttf`, and
  `showcase/notes/assets/JetBrainsMono-Regular.ttf`, `-Bold`, `-Italic` and `-BoldItalic`
- Source: <https://github.com/JetBrains/JetBrainsMono> (static TrueType)
- Copyright 2020 The JetBrains Mono Project Authors (<https://github.com/JetBrains/JetBrainsMono>)
- License: SIL Open Font License, Version 1.1, in `examples/poster/fonts/OFL-JetBrainsMono.txt` and
  `showcase/notes/assets/OFL-JetBrainsMono.txt`

## font8x8

- Files: the glyph table of `packages/render/src/font.sosu`
- Source: font8x8 by Daniel Hepper (<https://github.com/dhepper/font8x8>), after the IBM PC font
- License: public domain

## SDL3

- Not stored in the repository: `packages/platform/fetch-sdl3.ps1` downloads it into
  `packages/platform/vendor/` on Windows; on Linux, the SDL3 of the system is used. Programs that
  use the `platform` package link it.
- Source: <https://github.com/libsdl-org/SDL>
- Copyright (C) 1997-2026 Sam Lantinga
- License: zlib license (<https://www.libsdl.org/license.php>): free use, including in
  commercial programs, without having to publish their source.

## CommonMark specification

- Files: `showcase/notes/spec/spec.json`, and `showcase/notes/spec/spec.txt`, the same examples with
  other separators; the test of the Markdown parser of Notes compares its output with them
- Source: <https://spec.commonmark.org/0.31.2/>, version 0.31.2
- Written by John MacFarlane
- License: Creative Commons Attribution-ShareAlike 4.0 International
  (<https://creativecommons.org/licenses/by-sa/4.0/>), for these two files only

## stb_vorbis

- Files: `packages/audio/native/vendor/stb_vorbis.c` (version 1.22 with the fixes of SDL),
  compiled into the programs that use the `audio` package (through `packages/audio/native/ogg.c`)
- Source: <https://github.com/sezero/stb>, branch `stb_vorbis-sezero`, file `stb_vorbis.c` at
  commit `dd0c5eccaf092012b531d69d595fb587ece79571` (198 352 bytes, SHA-256
  `6f4805310469cd087e771c30866d5259a0725905a8eff06cfcb406c0ce41e68c`): the copy kept by a
  developer of SDL for SDL_sound and SDL_mixer, since <https://github.com/nothings/stb> has not
  changed it since 1.22 (2021). It fixes the eight faults that GitHub Security Lab reported against
  1.22 on hostile files in 2023 (CVE-2023-45675 to CVE-2023-45682, among them writes outside of
  a buffer and the free of a pointer never set, not only crashes), and others found since.
- Written by Sean Barrett (2007) and its contributors, named in the file
- License: public domain (the Unlicense), or the MIT license (Copyright (c) 2017 Sean Barrett),
  at your option; both are at the end of the file
- Changed by Sosu: two places, marked `// Sosu:`, both found by `make fuzz-ogg`. `get_window`
  compares a length with half of the block sizes instead of shifting it, which was undefined for
  the negative length of a hostile file (the window was not found either way).
  `fixup_current_playback_loc` does not cut the samples to a stream length above `INT_MAX`, which
  overflowed an `int` and gave a negative number of samples. `ogg.c` bounds its memory and
  compiles it without the comment header (`STB_VORBIS_NO_COMMENTS`), which Sosu does not read;
  the CI fuzzes it. A fault found later in it is fixed by taking the new commit of that branch, or
  here when that branch has none (`SECURITY.md`).

## ufbx

- Files: `packages/model/native/vendor/ufbx.c` and `ufbx.h` (version 0.23.1), and its license
  `packages/model/native/vendor/LICENSE`, compiled into the programs that use the `model`
  package (through `packages/model/native/fbx.c`), for the FBX files of `model.fbx`
- Source: <https://github.com/ufbx/ufbx>, tag `v0.23.1` (commit
  `26a482ae66871d7de36eb722aa060bce95bce274`, September 25, 2026): `ufbx.c` (1 181 511 bytes,
  SHA-256 `4a956c26a708e40d82ecb0aeaee54da709d5ba7c144c9642174970059bda7e1d`), `ufbx.h`
  (223 898 bytes, SHA-256 `f787be529af577efc04f3e3e735844a08556718dbb613d1211c2b313d72895dc`)
- Written by Samuli Raivio (2020) and its contributors
- License: the MIT license (Copyright (c) 2020 Samuli Raivio), or public domain (the Unlicense), at
  your option; both are in `LICENSE`
- Not changed by Sosu. `fbx.c` limits the memory a file may ask for (1 GB) and turns the scene to
  the axes and the unit of Sosu. ufbx is written to read files from anywhere and fuzzed by its
  authors; a fault found in it is fixed by taking its new version (`SECURITY.md`).

## SQLite

- Files: `packages/sqlite/native/vendor/sqlite3.c` and `sqlite3.h` (version 3.54.1), compiled
  into the programs that use the `sqlite` package (through `packages/sqlite/native/sqlite.c`)
- Source: <https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip> (2 990 966 bytes,
  SHA3-256 `92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30`, as
  <https://www.sqlite.org/download.html> gives it); the other files of the archive (`shell.c`,
  `sqlite3ext.h`) are not kept
- Written by D. Richard Hipp and the developers of SQLite
- License: public domain (<https://www.sqlite.org/copyright.html>), with no condition
- Not changed: its options are set by `native/sqlite.c` before it includes the file
  (`SQLITE_THREADSAFE=2`, `SQLITE_DQS=0`, `SQLITE_DEFAULT_FOREIGN_KEYS=1`,
  `SQLITE_OMIT_LOAD_EXTENSION`...)

## Mbed TLS

- Files: `packages/net/native/mbedtls/` (version 4.1.1, the long-term support branch 4.1,
  maintained until March 2029), compiled into the programs that use the `net` package (through
  `packages/net/native/tls.c`)
- Source: <https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-4.1.1/mbedtls-4.1.1.tar.bz2>
  (SHA-256 `3359a349e23db3d5536fcee032ae7b2ecbfc08972fab643089b5cbf2a375c98c`, as the release
  gives it); only the library is kept: `include/`, `library/` and, of `tf-psa-crypto/`,
  `include/`, `core/`, `dispatch/`, `drivers/builtin/`, `extras/`, `platform/` and `utilities/`
  (not the tests, the programs, the documentation, the build files, nor the drivers `everest`,
  `p256-m` and `pqcp`)
- Copyright The Mbed TLS Contributors
- License: Apache License 2.0 (or GPL 2.0 or later, at your option), in
  `packages/net/native/mbedtls/LICENSE`
- Changed: one line at the end of `include/mbedtls/mbedtls_config.h` and of
  `tf-psa-crypto/include/psa/crypto_config.h`, which includes the choices of Sosu
  (`packages/net/native/sosu_tls_config.h`, `sosu_crypto_config.h`: no file written, no
  session kept, threads). `packages/net/native/UPDATING.md` says how to take a new version.

## Box2D

- Files: none in the repository. `packages/physics2d/sosu.toml` names it as a native dependency
  (`[native.box2d]`, spec §9.1): `sosuc` clones it into the `build/deps` of the program that uses
  `physics2d` and compiles its `src/` into it, with `packages/physics2d/native/physics2d.c` and
  `box2d_avx2.c`
- Source: <https://github.com/erincatto/box2d>, tag `v3.2.0` (commit
  `178495ba35695aefe9c321e569d2277f06db3b90`), pinned by `sosu.lock`
- Written by Erin Catto and its contributors
- License: the MIT license (Copyright (c) 2022 Erin Catto), in `LICENSE` of the repository; a
  program that ships `physics2d` carries this notice
- Not changed. `box2d_avx2.c` includes its `contact_solver_w8.c` so that MSVC compiles it without
  `/arch:AVX2` (Box2D calls it only on processors that have AVX2)

## Jolt Physics

- Files: none in the repository. `packages/physics/sosu.toml` names it as a native dependency
  (`[native.jolt]`, spec §9.1): `sosuc` clones it into the `build/deps` of the program that uses
  `physics` and compiles its `Jolt/` into it, with `packages/physics/native/physics.cpp`
- Source: <https://github.com/jrouwe/JoltPhysics>, tag `v5.6.0` (commit
  `e77f175595e64cb44218cc9d9d56fc365ad0e36a`), pinned by `sosu.lock`
- Written by Jorrit Rouwe and its contributors
- License: the MIT license (Copyright 2021 Jorrit Rouwe), in `LICENSE` of the repository; a program
  that ships `physics` carries this notice
- Not changed

## Recast & Detour

- Files: none in the repository. `packages/nav/sosu.toml` names it as a native dependency
  (`[native.recast]`, spec §9.1): `sosuc` clones it into the `build/deps` of the program that uses
  `nav` and compiles its `Recast/`, `Detour/` and `DetourCrowd/` into it, with
  `packages/nav/native/nav.cpp`
- Source: <https://github.com/recastnavigation/recastnavigation>, tag `v1.6.0` (commit
  `6dc1667f580357e8a2154c28b7867bea7e8ad3a7`), pinned by `sosu.lock`
- Written by Mikko Mononen and its contributors
- License: the zlib license (Copyright (c) 2009 Mikko Mononen), in `License.txt` of the
  repository: a program that ships `nav` may say so in its documentation, which is not required
- Not changed
