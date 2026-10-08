# Test fixtures

Binary archives used by `mmcfm --selftest` (`src/ftest_arc2.c`). The self
test finds this folder as `tests/fixtures` under the current directory, or
through the `MMCFM_FIXTURES` environment variable; without it the RAR tests
are skipped.

## From libarchive (BSD-2-Clause)

`test_read_format_rar*.rar` are the reference files of libarchive 3.7.7
(`libarchive/test/*.rar.uu`, uudecoded), Copyright (c) 2003-2018 Tim
Kientzle, Andres Mejia, Grzegorz Antoniak and the libarchive contributors,
distributed under the 2-clause BSD licence:

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:
    1. Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer
       in this position and unchanged.
    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
    IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
    OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
    IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
    INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
    NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
    THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

| file | covers |
|---|---|
| `test_read_format_rar.rar` | RAR4 store, Unix symlink, folders |
| `test_read_format_rar_unicode.rar` | RAR4 packed UTF-16 names, symlink, LZ |
| `test_read_format_rar_compress_normal.rar` | RAR 2.9 LZ |
| `test_read_format_rar_compress_best.rar` | RAR 2.9 PPMd var.H |
| `test_read_format_rar_filter.rar` | RarVM E8E9 and DELTA filters (a PE file) |
| `test_read_format_rar_multi_lzss_blocks.rar` | several LZ table blocks, 20 MB output |
| `test_read_format_rar4_encrypted.rar` | encrypted entries next to plain ones |
| `test_read_format_rar_encryption_header.rar` | RAR4 encrypted headers |
| `test_read_format_rar5_compressed.rar` | RAR5 LZ |
| `test_read_format_rar5_multiple_files.rar` | RAR5, several files |
| `test_read_format_rar5_solid.rar` | RAR5 solid stream |
| `test_read_format_rar5_stored.rar` | RAR5 store |
| `test_read_format_rar5_arm.rar` | RAR5 ARM filter |
| `test_read_format_rar5_blake2.rar` | RAR5 BLAKE2sp checksum |
| `test_read_format_rar5_symlink.rar` | RAR5 redirection records |
| `test_read_format_rar5_hardlink.rar` | RAR5 hard link (refused) |
| `test_read_format_rar5_encrypted.rar` | RAR5 encrypted entries |
| `test_read_format_rar5_encrypted_filenames.rar` | RAR5 encrypted headers |
| `test_read_format_rar5_multiarchive.part01.rar` | first volume, file continued in part 2 |

## From rarfile (ISC licence)

`rarfile_*.rar` come from the test suite of the Python `rarfile` module
(https://github.com/markokr/rarfile, `test/files/`), Copyright (c) 2005-2026
Marko Kreen, under the ISC licence:

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

| file | covers |
|---|---|
| `rarfile_rar3-solid.rar` | RAR 2.9 solid stream, second file references the first |
| `rarfile_unicode.rar` | solid PPMd stream, non-BMP names |
| `rarfile_rar202-comment-nopsw.rar` | RAR 2.0 headers with comments (CRC of the short form) |

## Made for mmcfm

`mmcfm_*.rar` were made for this project from generated data (no third-party
content), with RAR 4.20 (RAR 2.9 format; forced `-mc` filters) and RAR 7.01
(RAR 5.0 format). They are part of mmcfm and share its licence.

| file | covers |
|---|---|
| `mmcfm_rar4_filters.rar` | RarVM DELTA (`-mc4d+`), AUDIO (`-mc4a+`), RGB (`-mcc+`), E8 (`-mce+`), ITANIUM (`-mci+`) |
| `mmcfm_rar4_solid_lz.rar` | RAR 2.9 solid LZ with an empty file in the stream |
| `mmcfm_rar4_solid_ppmd.rar` | RAR 2.9 solid PPMd (`-mct+`) |
| `mmcfm_rar4_vol.part1.rar` | RAR4 first volume: one whole file, one continued |
| `mmcfm_rar5_filters.rar` | RAR5 DELTA (`-mc4D+`) and E8 (`-mcE+`) filters |
| `mmcfm_rar5_solid.rar` | RAR5 solid stream with folders and an empty file |
