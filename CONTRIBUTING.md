# Contributing

mmcfm is plain C11 on SDL2. The code builds with zig cc, clang, gcc, the Android NDK, emcc and tcc. Keep it small, fast and readable.

## Building

There is one script, `build.sh`. It needs a POSIX shell: mmc-shell, Git Bash, MSYS2, Linux or macOS.

```sh
./build.sh                      # release build for this machine (win-x64 on Windows)
./build.sh win-x64 debug run    # debug build, then start it
./build.sh win-x64 --cc=tcc     # tcc build (32-bit Windows, loads SDL2.dll)
./build.sh linux-x64            # cross-compile with zig
./build.sh android              # APK: NDK + aapt2/d8/apksigner, no Gradle
./build.sh selftest             # build, then run mmcfm --selftest
./build.sh --help
```

Visual check without clicking around:

```sh
dist/win-x64/mmcfm.exe --shot out.bmp --size 420x860   # portrait phone size
```

## Layout: the Lua way

Like Lua's `src/`, the sources form one flat folder of short, prefixed `.c/.h` pairs:

| file | role |
|---|---|
| `fcore` | types, memory, strings, UTF-8, paths, file types |
| `fsdl` | SDL linked or loaded at run time (`fsdl_list.h` lists every SDL call) |
| `fplat_*` | operating system layer (`fplat.h`) |
| `fgfx` `ffont` `ficon` `fui` | drawing, text, icons, immediate-mode widgets |
| `fapp` `fpanel` `flayout` `fops` `fvfs` `fconf` | the file manager itself |
| `farc` `fzip` `f7z` `frar` `ftar` `fgz` ... `fcrypt` `fstream` | archives |
| `fview_*` `fdec_*` `fthumb` | viewers, decoders, thumbnails |
| `x*.c` | wrappers that compile a single-header library from `vendor/` |
| `ftest*` | the built-in self test (`--selftest`) |

Third-party code lives in `vendor/` and is not edited, apart from the few patches listed in `vendor/PATCHES.md`.

## File style

Every file starts with a header comment: what the file is, then the decisions behind it.

```c
/* fzip.c -- zip archives: list, extract, create; ZipCrypto and AES-256.
**
** Design decisions:
**   - The central directory is read once into the entry arena ...
*/
```

Sections are separated by lower-case rulers about 78 columns wide:

```c
/* ---- central directory -------------------------------------------------- */
```

- Indent with 2 spaces, use K&R braces, and keep lines to about 100 columns.
- Names are `snake_case`. Module prefixes come first: `gfx_rrect`, `ui_button`, `arc_open`, `plat_stat`.
- Types are `CapitalCase` with an `Fm` prefix (`FmRect`, `FmArc`). Constants are `UPPER_CASE`.
- Globals in a module are `static` and named `g_*`. Shared tables are `kName`.
- Comments say *why*, not what. Don't leave TODO notes or commented-out code.

## Rules that keep it portable and small

- **C11, but tcc must compile it.**
  - Don't use `_Atomic`, `<threads.h>`, statement expressions or VLAs.
  - Use `SDL_Atomic*`, `SDL_mutex`, and `fm_thread_create` (not `SDL_CreateThread`).
- **Every SDL function you call must be listed** in `src/fsdl_list.h`. The tcc, Linux, macOS and BSD builds load SDL at run time and fail to link otherwise.
- **Windows headers:** include `fwin.h`, never `<windows.h>`.
- **Files:** open them with `fm_fopen`, never `fopen`, so that UTF-8 paths work on Windows.
- **Memory:**
  - Allocate with `fm_alloc`/`fm_free`, and use `FmArena` for many small strings.
  - Stream big data through fixed buffers; never read a whole file to process it.
- **Drawing:**
  - Coordinates are pixels. Use `DP(x)` for sizes designed in dp.
  - Use `T.*` theme colours, never hard-coded ones (except file-type colours in `ficon.c`).
- **Idle cost:** nothing may animate or poll when idle. Call `ui_animate()` only while something actually moves.
- **Inline assembly** goes in `fasm.h`, always with a C fallback, and only under `FM_ASM`.

## Tests

`mmcfm --selftest` runs without a window. Add cases to the `ftest*.c` file for your area. They must pass before a change goes in.
