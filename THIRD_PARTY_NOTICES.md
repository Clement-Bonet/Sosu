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

- Files: `packages/audio/native/vendor/stb_vorbis.c` (version 1.22), compiled into the programs
  that use the `audio` package (through `packages/audio/native/ogg.c`)
- Source: <https://github.com/nothings/stb>, file `stb_vorbis.c` at commit
  `1ee679ca2ef753a528db5ba6801e1067b40481b8` (192 790 bytes, SHA-256
  `4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636`)
- Written by Sean Barrett (2007) and its contributors, named in the file
- License: public domain (the Unlicense), or the MIT license (Copyright (c) 2017 Sean Barrett),
  at your option; both are at the end of the file
- Changed by Sosu: five lines, each marked `// Sosu:`. `setup_malloc` and `setup_temp_malloc`
  refuse a negative size (a size that overflowed), and the comment header refuses a negative
  length or more than 2^24 comments (CVE-2023-45675 and CVE-2023-45676). The other faults
  reported against 1.22 on hostile files, by the same review of GitHub Security Lab (2023), are
  not fixed: the decoder's memory is bounded, but a hostile file can still crash it.
