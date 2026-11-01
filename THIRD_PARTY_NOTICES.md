# Third-party components

Sosu itself is licensed under the MIT license or the Apache License 2.0, at your option (see
`LICENSE-MIT` and `LICENSE-APACHE`). The repository also contains or uses the following works,
under their own licenses.

## Noto Sans

- Files: `packages/render/fonts/NotoSans-Regular.ttf`
- Source: <https://github.com/notofonts/notofonts.github.io> (unhinted TrueType)
- Copyright 2022 The Noto Project Authors (<https://github.com/notofonts/latin-greek-cyrillic>)
- License: SIL Open Font License, Version 1.1, in `packages/render/fonts/OFL.txt`. The font may
  be used, embedded and redistributed with programs; it may not be sold by itself, and a modified
  version must not use the name "Noto".

## Montserrat

- Files: `examples/poster/fonts/Montserrat-ExtraBold.ttf`, `examples/poster/fonts/Montserrat-Bold.ttf`
- Source: <https://github.com/JulietaUla/Montserrat> (static TrueType, through Google Fonts)
- Copyright 2024 The Montserrat.Git Project Authors (<https://github.com/JulietaUla/Montserrat.git>)
- License: SIL Open Font License, Version 1.1, in `examples/poster/fonts/OFL-Montserrat.txt`

## JetBrains Mono

- Files: `examples/poster/fonts/JetBrainsMono-Regular.ttf`
- Source: <https://github.com/JetBrains/JetBrainsMono> (static TrueType)
- Copyright 2020 The JetBrains Mono Project Authors (<https://github.com/JetBrains/JetBrainsMono>)
- License: SIL Open Font License, Version 1.1, in `examples/poster/fonts/OFL-JetBrainsMono.txt`

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
- Not changed. `ogg.c` bounds its memory and compiles it without the comment header
  (`STB_VORBIS_NO_COMMENTS`), which Sosu does not read; the CI fuzzes it (`make fuzz-ogg`). A
  fault found later in it is fixed by taking the new commit of that branch (`SECURITY.md`).

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
