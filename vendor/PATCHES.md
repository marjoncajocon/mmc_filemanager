# Changes to vendored code

Third-party code in `vendor/` is kept as released, except for the patches listed here.

| File | Change | Why |
|---|---|---|
| `lzma/CpuArch.h` | Adds a `__TINYC__` branch that defines `MY_CPU_pragma_pack_push_1` and `MY_CPU_pragma_pop` as empty. | tcc 0.9.27 does not support `_Pragma`. The SDK's own comment says the pack pragma is not needed on most compilers. |
| `lzma/CpuArch.c` | Adds `z7_x86_cpuid_subFunc` to the "unsupported cpuid" branch, as a stub returning zeros. | That branch, which tcc compiles, lacked the function, so linking failed. |
| `lzma/*.h` | All headers from `C/` of 7-Zip 24.09 are copied, including ones whose `.c` files are not compiled. | Some compiled files include them. |

Versions:

| Library | Version |
|---|---|
| SDL2 | 2.32.10 (source tarball) |
| miniz | 3.0.2 |
| bzip2 | 1.0.8 |
| LZMA SDK | the `C/` folder of 7-Zip 24.09 |
| zstd | 1.5.6 (`lib/` only) |
| stb_image, stb_truetype, stb_vorbis | `nothings/stb` master |
| dr_mp3, dr_flac, dr_wav | `mackron/dr_libs` master |
| pl_mpeg | master |
| nanosvg | master |
