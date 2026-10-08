# Third-party licences

mmcfm itself is under the MIT licence (`LICENSE`). It includes the following code and data. Each library's own licence file sits next to it in `vendor/` where the upstream ships one.

| Component | Where | Licence |
|---|---|---|
| SDL 2.32.10 | `vendor/SDL2-2.32.10.tar.gz`, `vendor/SDL2-include/` | zlib |
| SDL Android Java glue (`org.libsdl.app`) | taken from the SDL source at build time | zlib |
| miniz 3.0.2 | `vendor/miniz/` | MIT |
| bzip2 1.0.8 | `vendor/bzip2/` | bzip2 licence (BSD-style) |
| LZMA SDK (C part of 7-Zip 24.09) | `vendor/lzma/` | public domain |
| zstd 1.5.6 | `vendor/zstd/` | BSD-3-Clause (dual GPLv2; used under BSD) |
| stb_image, stb_truetype, stb_vorbis | `vendor/` | MIT or public domain (your choice) |
| dr_mp3, dr_flac, dr_wav | `vendor/` | MIT-0 or public domain (your choice) |
| pl_mpeg | `vendor/pl_mpeg.h` | MIT |
| nanosvg, nanosvgrast | `vendor/` | zlib |
| Poppins font | `assets/fonts/` | SIL Open Font License 1.1 (`assets/fonts/OFL.txt`) |
| RAR decoding in `src/frar.c` | derived from libarchive | BSD-2-Clause (notice kept in the file) |
| RAR test fixtures | `tests/fixtures/` | from libarchive's test suite, BSD-2-Clause |

## Optional run-time components

- **FFmpeg.** It is not shipped. When FFmpeg's libraries are present on the system or next to the program, mmcfm loads them at run time to play more video and audio formats. They keep their own (L)GPL licence.
- **System fonts.** They are used as fallbacks for scripts Poppins does not cover. They are read from the operating system and are not redistributed.
