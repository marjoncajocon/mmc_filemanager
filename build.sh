#!/usr/bin/env bash
#
# build.sh -- the one build script for mmc_filemanager (mmcfm).
#
#   ./build.sh [target] [debug|release] [options] [command]
#
# Targets (default win-x64 on Windows, the host otherwise):
#   win-x64  win-arm64            zig cc, SDL2 static inside the exe
#   linux-x64  linux-arm64        zig cc (glibc 2.17), SDL2 loaded at run time
#   macos-x64  macos-arm64        zig cc, .app bundle, SDL2 loaded at run time
#   freebsd-x64                   zig cc, SDL2 loaded at run time
#   android                       NDK clang, static SDL2, APK packed without Gradle
#   web                           emsdk (emcc -sUSE_SDL=2), demo on MEMFS
#
# Options:
#   --cc=zig|tcc   compiler for desktop targets (tcc: a 32-bit Windows build, SDL2 loaded
#                  at run time from the SDL2.dll this script builds itself)
#   --jobs=N       parallel compiles (default 4)
#   --no-asm       build without the inline-asm fast paths
#   --with-ffmpeg=DIR  copy FFmpeg 4-8 shared libraries (and their licence) from DIR
#                  into dist/, so every video format plays; desktop zig builds only.
#                  Without it video uses the OS decoders (Media Foundation on Windows,
#                  MediaCodec on Android) or an FFmpeg already on the system. Use an
#                  LGPL build: a GPL one makes the shipped bundle GPL.
#
# Commands:
#   clean          remove build/ and dist/
#   sdl            only build the static SDL2 for the target
#   run            build, then start the result from dist/
#   selftest       build, then run `mmcfm --selftest`
#
# Environment overrides: ZIG TCC ANDROID_HOME NDK EMSDK CMAKE JOBS KEYSTORE
#   BUILD_TAG=x               separate build/ and dist/ folders (parallel builds)
#   ABIS="arm64-v8a x86_64"   Android ABIs   KEYSTORE/KEY_ALIAS/KEY_PASS  release key
#
# Everything comes from vendor/ inside this folder; the script never picks up
# an SDL2.dll or headers from the system. Output lands in dist/<target>/,
# objects in build/<target>-<mode>/ (tcc: dist/win-x86-tcc, build/win-x86-tcc-<mode>).
#
# Shell: run with mmc-shell (D:\mmc-shell\mmc-shell.exe build.sh ...) or any
# Linux-style bash (Git Bash, MSYS2, Linux, macOS). Paths are Linux style;
# a path handed to a Windows program is converted on the spot with cygpath.

set -euo pipefail
cd "$(dirname "$0")"

APP="mmcfm"
APP_NAME="MMC File Manager"
APP_ID="io.github.mmc.filemanager"
APP_VERSION="0.1.0"
APP_VERSION_CODE=1
SDL_VER="2.32.10"

# ---- arguments ----------------------------------------------------------------

TARGET=""
MODE="release"
CCKIND="zig"
JOBS="${JOBS:-4}"
USE_ASM=1
CMD="build"
FFMPEG_DIR=""
for a in "$@"; do
  case "$a" in
    debug|release) MODE="$a" ;;
    clean|sdl|run|selftest) CMD="$a" ;;
    --cc=*) CCKIND="${a#--cc=}" ;;
    --jobs=*) JOBS="${a#--jobs=}" ;;
    --no-asm) USE_ASM=0 ;;
    --with-ffmpeg=*) FFMPEG_DIR="${a#--with-ffmpeg=}" ;;
    win-x64|win-arm64|linux-x64|linux-arm64|macos-x64|macos-arm64|freebsd-x64|android|web)
      TARGET="$a" ;;
    -h|--help) sed -n '2,36p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "error: unknown argument '$a' (see ./build.sh --help)" >&2; exit 1 ;;
  esac
done

HOST_OS="$(uname -s)"
case "$HOST_OS" in
  MINGW*|MSYS*|CYGWIN*|Windows*) HOST=win; EXE=".exe" ;;
  Darwin) HOST=mac; EXE="" ;;
  FreeBSD) HOST=bsd; EXE="" ;;
  *) HOST=linux; EXE="" ;;
esac
if [ -z "$TARGET" ]; then
  case "$HOST" in
    win) TARGET=win-x64 ;;
    mac) [ "$(uname -m)" = arm64 ] && TARGET=macos-arm64 || TARGET=macos-x64 ;;
    bsd) TARGET=freebsd-x64 ;;
    *) [ "$(uname -m)" = aarch64 ] && TARGET=linux-arm64 || TARGET=linux-x64 ;;
  esac
fi

if [ "$CMD" = "clean" ]; then
  rm -rf build dist
  echo "cleaned"
  exit 0
fi

# Converts a path for a native Windows program; a no-op elsewhere.
W() { if [ "$HOST" = win ]; then cygpath -am "$1"; else echo "$1"; fi; }

die() { echo "error: $*" >&2; exit 1; }

# ---- tools ----------------------------------------------------------------------

find_zig() {
  if [ -n "${ZIG:-}" ]; then return; fi
  ZIG="$(command -v zig 2>/dev/null || true)"
  [ -n "$ZIG" ] || ZIG="/d/env/zig/zig.exe"
  [ -x "$ZIG" ] || die "zig not found (set ZIG=/path/to/zig)"
}

find_tcc() {
  if [ -n "${TCC:-}" ]; then return; fi
  TCC="$(command -v i386-win32-tcc 2>/dev/null || true)"
  [ -n "$TCC" ] || TCC="/d/env/tcc/i386-win32-tcc.exe"
  [ -x "$TCC" ] || die "tcc not found (set TCC=/path/to/tcc)"
}

find_android() {
  SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-/d/env/Android/Sdk}}"
  SDK="$(cygpath -u "$SDK" 2>/dev/null || echo "$SDK")"
  [ -d "$SDK" ] || die "Android SDK not found (set ANDROID_HOME)"
  NDK="${NDK:-$(ls -d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -1)}"
  [ -d "$NDK" ] || die "no NDK under $SDK/ndk"
  NDK_BIN="$(ls -d "$NDK"/toolchains/llvm/prebuilt/*/bin | head -1)"
  BT="$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)"
  PLATFORM_JAR="$(ls -d "$SDK"/platforms/android-*/android.jar | sort -V | tail -1)"
  [ -f "$PLATFORM_JAR" ] || die "no platforms/android-*/android.jar in $SDK"
  JAVA_BIN=""
  if [ -n "${JAVA_HOME:-}" ]; then JAVA_BIN="$(cygpath -u "$JAVA_HOME" 2>/dev/null || echo "$JAVA_HOME")/bin/"; fi
  [ -x "${JAVA_BIN}javac$EXE" ] || [ -x "${JAVA_BIN}javac" ] || JAVA_BIN="/d/env/java/jdk-21.0.11/bin/"
}

find_cmake() {
  if [ -n "${CMAKE:-}" ]; then :
  elif command -v cmake >/dev/null 2>&1; then CMAKE="$(command -v cmake)"
  else CMAKE="$(ls -d /d/env/Android/Sdk/cmake/*/bin/cmake.exe 2>/dev/null | sort -V | tail -1)"; fi
  [ -n "$CMAKE" ] && [ -x "$CMAKE" ] || die "cmake not found (set CMAKE=...)"
  NINJA="$(dirname "$CMAKE")/ninja$EXE"
  [ -x "$NINJA" ] || NINJA="$(command -v ninja 2>/dev/null || true)"
  [ -n "$NINJA" ] || die "ninja not found next to cmake or on PATH"
}

# ---- SDL2 source and static builds ----------------------------------------------

SDL_SRC="build/SDL2-$SDL_VER"

sdl_source() {
  [ -f "$SDL_SRC/CMakeLists.txt" ] && return
  mkdir -p build
  echo "unpacking SDL2 $SDL_VER from vendor/"
  tar -xzf "vendor/SDL2-$SDL_VER.tar.gz" -C build
}

# Static SDL2 for Windows (x64 or arm64) through zig, CMake and Ninja.
# zig's tools are wrapped in tiny .cmd files because CMake wants one
# executable per tool.
sdl_static_windows() {   # $1 = zig target triple, $2 = cmake processor
  local triple="$1" proc="$2"
  local prefix="build/sdl/$triple" obj="build/sdl/$triple-cmake" wrap="build/sdl/$triple-wrap"
  SDL_PREFIX="$prefix"
  [ -f "$prefix/lib/libSDL2.a" ] && [ "$CMD" != "sdl" ] && return
  sdl_source; find_zig; find_cmake
  mkdir -p "$wrap" "$obj"
  local wz; wz="$(cygpath -w "$ZIG")"
  printf '@"%s" cc -target %s %%*\r\n' "$wz" "$triple" > "$wrap/zig-cc.cmd"
  printf '@"%s" ar %%*\r\n' "$wz" > "$wrap/zig-ar.cmd"
  printf '@"%s" ranlib %%*\r\n' "$wz" > "$wrap/zig-ranlib.cmd"
  printf '@"%s" rc %%*\r\n' "$wz" > "$wrap/zig-rc.cmd"
  echo "building static SDL2 $SDL_VER for $triple (once)"
  "$CMAKE" -G Ninja -S "$(W "$SDL_SRC")" -B "$(W "$obj")" \
    -DCMAKE_MAKE_PROGRAM="$(W "$NINJA")" \
    -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR="$proc" \
    -DCMAKE_C_COMPILER="$(W "$wrap/zig-cc.cmd")" \
    -DCMAKE_AR="$(W "$wrap/zig-ar.cmd")" \
    -DCMAKE_RANLIB="$(W "$wrap/zig-ranlib.cmd")" \
    -DCMAKE_RC_COMPILER="$(W "$wrap/zig-rc.cmd")" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$(W "$prefix")" \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_TESTS=OFF \
    -DSDL2_DISABLE_SDL2MAIN=ON > "build/sdl/$triple-configure.log"
  "$CMAKE" --build "$(W "$obj")"
  "$CMAKE" --install "$(W "$obj")" > /dev/null
  echo "built $prefix/lib/libSDL2.a"
}

# zig's tools wrapped in tiny .cmd files for one target triple: CMake wants
# one executable per tool.
zig_wrappers() {   # $1 = triple, sets WRAP
  WRAP="build/sdl/$1-wrap"
  mkdir -p "$WRAP"
  local wz; wz="$(cygpath -w "$ZIG")"
  printf '@"%s" cc -target %s %%*\r\n' "$wz" "$1" > "$WRAP/zig-cc.cmd"
  printf '@"%s" ar %%*\r\n' "$wz" > "$WRAP/zig-ar.cmd"
  printf '@"%s" ranlib %%*\r\n' "$wz" > "$WRAP/zig-ranlib.cmd"
  printf '@"%s" rc %%*\r\n' "$wz" > "$WRAP/zig-rc.cmd"
}

# 32-bit SDL2.dll for the tcc build: built here from vendor/, never taken
# from PATH or the system.
sdl_shared_windows() {
  local triple="x86-windows-gnu"
  local prefix="build/sdl/$triple-shared" obj="build/sdl/$triple-shared-cmake"
  SDL_DLL="$prefix/bin/SDL2.dll"
  [ -f "$SDL_DLL" ] && return
  sdl_source; find_zig; find_cmake
  zig_wrappers "$triple"
  mkdir -p "$obj"
  echo "building 32-bit SDL2.dll $SDL_VER for the tcc build (once)"
  "$CMAKE" -G Ninja -S "$(W "$SDL_SRC")" -B "$(W "$obj")" \
    -DCMAKE_MAKE_PROGRAM="$(W "$NINJA")" \
    -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=X86 \
    -DCMAKE_C_COMPILER="$(W "$WRAP/zig-cc.cmd")" \
    -DCMAKE_AR="$(W "$WRAP/zig-ar.cmd")" \
    -DCMAKE_RANLIB="$(W "$WRAP/zig-ranlib.cmd")" \
    -DCMAKE_RC_COMPILER="$(W "$WRAP/zig-rc.cmd")" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$(W "$prefix")" \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_TESTS=OFF \
    -DSDL2_DISABLE_SDL2MAIN=ON > "build/sdl/$triple-shared-configure.log"
  "$CMAKE" --build "$(W "$obj")"
  "$CMAKE" --install "$(W "$obj")" > /dev/null
}

# Static SDL2 for one Android ABI through the NDK's CMake toolchain.
sdl_static_android() {   # $1 = ABI
  local abi="$1"
  local prefix="build/sdl/android-$abi" obj="build/sdl/android-$abi-cmake"
  SDL_PREFIX="$prefix"
  [ -f "$prefix/lib/libSDL2.a" ] && [ "$CMD" != "sdl" ] && return
  sdl_source; find_android
  [ -n "${CMAKE:-}" ] || CMAKE="$(ls -d "$SDK"/cmake/*/bin/cmake$EXE | sort -V | tail -1)"
  find_cmake
  mkdir -p "$obj"
  echo "building static SDL2 $SDL_VER for android $abi (once)"
  "$CMAKE" -G Ninja -S "$(W "$SDL_SRC")" -B "$(W "$obj")" \
    -DCMAKE_MAKE_PROGRAM="$(W "$NINJA")" \
    -DCMAKE_TOOLCHAIN_FILE="$(W "$NDK/build/cmake/android.toolchain.cmake")" \
    -DANDROID_ABI="$abi" -DANDROID_PLATFORM=android-$ANDROID_API \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$(W "$prefix")" \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON -DSDL_TEST=OFF \
    -DSDL_TESTS=OFF -DSDL2_DISABLE_SDL2MAIN=ON -DSDL_HIDAPI=OFF \
    > "build/sdl/android-$abi-configure.log"
  "$CMAKE" --build "$(W "$obj")"
  "$CMAKE" --install "$(W "$obj")" > /dev/null
}

# ---- sources --------------------------------------------------------------------

# Third-party C files compiled as they are (the single-header libraries are
# compiled through the src/x*.c wrappers instead).
VENDOR_SRCS="
  vendor/bzip2/blocksort.c vendor/bzip2/huffman.c vendor/bzip2/crctable.c
  vendor/bzip2/randtable.c vendor/bzip2/compress.c vendor/bzip2/decompress.c
  vendor/bzip2/bzlib.c
  vendor/lzma/7zAlloc.c vendor/lzma/7zArcIn.c vendor/lzma/7zBuf.c vendor/lzma/7zBuf2.c
  vendor/lzma/7zCrc.c vendor/lzma/7zCrcOpt.c vendor/lzma/7zDec.c vendor/lzma/7zStream.c
  vendor/lzma/Alloc.c vendor/lzma/Bcj2.c vendor/lzma/Bcj2Enc.c vendor/lzma/Bra.c
  vendor/lzma/Bra86.c vendor/lzma/BraIA64.c vendor/lzma/CpuArch.c vendor/lzma/Delta.c
  vendor/lzma/LzFind.c vendor/lzma/LzFindOpt.c vendor/lzma/Lzma2Dec.c vendor/lzma/Lzma2Enc.c
  vendor/lzma/LzmaDec.c vendor/lzma/LzmaEnc.c vendor/lzma/Ppmd7.c vendor/lzma/Ppmd7Dec.c
  vendor/lzma/Ppmd7Enc.c vendor/lzma/Ppmd7aDec.c vendor/lzma/Ppmd8.c vendor/lzma/Ppmd8Dec.c
  vendor/lzma/Sha256.c vendor/lzma/Sha256Opt.c vendor/lzma/Xz.c vendor/lzma/XzCrc64.c
  vendor/lzma/XzCrc64Opt.c vendor/lzma/XzDec.c vendor/lzma/XzEnc.c vendor/lzma/XzIn.c
"
VENDOR_SRCS="$VENDOR_SRCS $(ls vendor/zstd/common/*.c vendor/zstd/compress/*.c vendor/zstd/decompress/*.c | tr '\n' ' ')"
# Built-in video (src/fdec_vid_soft.c): WebM demuxer, VP9 decoder (libvpx,
# generic C) and Opus decoder (libopus, float); see vendor/PATCHES.md.
VENDOR_SRCS="$VENDOR_SRCS vendor/nestegg/nestegg.c
  vendor/libvpx/vpx_config.c
  vendor/libvpx/vp9/common/vp9_alloccommon.c vendor/libvpx/vp9/common/vp9_blockd.c
  vendor/libvpx/vp9/common/vp9_common_data.c vendor/libvpx/vp9/common/vp9_entropy.c
  vendor/libvpx/vp9/common/vp9_entropymode.c vendor/libvpx/vp9/common/vp9_entropymv.c
  vendor/libvpx/vp9/common/vp9_filter.c vendor/libvpx/vp9/common/vp9_frame_buffers.c
  vendor/libvpx/vp9/common/vp9_idct.c vendor/libvpx/vp9/common/vp9_loopfilter.c
  vendor/libvpx/vp9/common/vp9_mvref_common.c vendor/libvpx/vp9/common/vp9_pred_common.c
  vendor/libvpx/vp9/common/vp9_quant_common.c vendor/libvpx/vp9/common/vp9_reconinter.c
  vendor/libvpx/vp9/common/vp9_reconintra.c vendor/libvpx/vp9/common/vp9_rtcd.c
  vendor/libvpx/vp9/common/vp9_scale.c vendor/libvpx/vp9/common/vp9_scan.c
  vendor/libvpx/vp9/common/vp9_seg_common.c vendor/libvpx/vp9/common/vp9_thread_common.c
  vendor/libvpx/vp9/common/vp9_tile_common.c
  vendor/libvpx/vp9/decoder/vp9_decodeframe.c vendor/libvpx/vp9/decoder/vp9_decodemv.c
  vendor/libvpx/vp9/decoder/vp9_decoder.c vendor/libvpx/vp9/decoder/vp9_detokenize.c
  vendor/libvpx/vp9/decoder/vp9_dsubexp.c vendor/libvpx/vp9/decoder/vp9_job_queue.c
  vendor/libvpx/vp9/vp9_dx_iface.c vendor/libvpx/vp9/vp9_iface_common.c
  vendor/libvpx/vpx/src/vpx_codec.c vendor/libvpx/vpx/src/vpx_decoder.c vendor/libvpx/vpx/src/vpx_image.c
  vendor/libvpx/vpx_dsp/bitreader.c vendor/libvpx/vpx_dsp/bitreader_buffer.c vendor/libvpx/vpx_dsp/intrapred.c
  vendor/libvpx/vpx_dsp/inv_txfm.c vendor/libvpx/vpx_dsp/loopfilter.c vendor/libvpx/vpx_dsp/prob.c
  vendor/libvpx/vpx_dsp/vpx_convolve.c vendor/libvpx/vpx_dsp/vpx_dsp_rtcd.c vendor/libvpx/vpx_mem/vpx_mem.c
  vendor/libvpx/vpx_scale/generic/gen_scalers.c vendor/libvpx/vpx_scale/generic/vpx_scale.c
  vendor/libvpx/vpx_scale/generic/yv12config.c vendor/libvpx/vpx_scale/generic/yv12extend.c
  vendor/libvpx/vpx_scale/vpx_scale_rtcd.c vendor/libvpx/vpx_util/vpx_thread.c
"
VENDOR_SRCS="$VENDOR_SRCS $(ls vendor/opus/celt/*.c vendor/opus/silk/*.c vendor/opus/src/*.c | tr '\n' ' ')"

SRCS="$(ls src/*.c | tr '\n' ' ')"

# Fonts are embedded as C arrays generated into build/gen/ (xxd when present,
# od + awk otherwise, so any POSIX shell works).
GEN="build/gen"
embed() {   # $1 = file, $2 = C name, $3 = output header
  local f="$1" name="$2" out="$3"
  [ -f "$out" ] && [ ! "$f" -nt "$out" ] && return
  mkdir -p "$(dirname "$out")"
  echo "  gen $out"
  {
    echo "/* generated by build.sh from $f -- do not edit */"
    echo "static const unsigned char $name[] = {"
    od -An -v -tu1 "$f" | awk '{ s = ""; for (i = 1; i <= NF; i++) s = s $i ","; print s }'
    echo "};"
    echo "static const unsigned int ${name}_len = sizeof($name);"
  } > "$out.tmp.$$"
  mv "$out.tmp.$$" "$out"
}
gen_assets() {
  embed assets/fonts/Poppins-Regular.ttf font_regular_ttf "$GEN/gen_font_regular.h"
  embed assets/fonts/Poppins-SemiBold.ttf font_semibold_ttf "$GEN/gen_font_semibold.h"
  # SDL redirects for run-time loading (see src/fsdl.h).
  local redef="$GEN/gen_sdl_redef.h"
  if [ ! -f "$redef" ] || [ src/fsdl_list.h -nt "$redef" ]; then
    echo "  gen $redef"
    {
      echo "/* generated by build.sh from src/fsdl_list.h -- do not edit */"
      sed -n 's/^FM_SDL_FN([^,]*, *\(SDL_[A-Za-z0-9_]*\),.*/#define \1 fmsdl_\1/p' src/fsdl_list.h
    } > "$redef.tmp.$$"
    mv "$redef.tmp.$$" "$redef"
  fi
}

# ---- per-target configuration ----------------------------------------------------

COMMON_DEFS="-DFM_VERSION=\"$APP_VERSION\" -DZSTD_DISABLE_ASM -DZ7_ST"
WARN="-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare"
INC="-Isrc -I$GEN -Ivendor -Ivendor/SDL2-include -Ivendor/lzma -Ivendor/zstd -Ivendor/bzip2 -Ivendor/miniz"
# built-in video: nestegg, libvpx (its generated config headers sit at its root), libopus
INC="$INC -Ivendor/nestegg -Ivendor/libvpx -Ivendor/opus/include -Ivendor/opus/celt -Ivendor/opus/silk"
COMMON_DEFS="$COMMON_DEFS -DOPUS_BUILD -DUSE_ALLOCA -DHAVE_LRINTF -DNO_ASSERTS"
[ "$USE_ASM" = 1 ] || COMMON_DEFS="$COMMON_DEFS -DFM_NO_ASM"

case "$MODE" in
  debug)   OPT="-O1 -g"; ;;
  release) OPT="-O2 -DNDEBUG"; ;;
esac

SDL_LINK="dynamic"       # dynamic: fsdl.c loads SDL2 at run time
LIBS=""
LDFLAGS=""
OUTDIR="dist/$TARGET"
OUT="$OUTDIR/$APP"
ANDROID_API=26

case "$TARGET" in
  win-x64|win-arm64)
    if [ "$TARGET" = win-x64 ]; then TRIPLE="x86_64-windows-gnu"; PROC=AMD64; else TRIPLE="aarch64-windows-gnu"; PROC=ARM64; fi
    OUT="$OUTDIR/$APP.exe"
    WINLIBS="-luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lshell32 -lsetupapi -lversion -luuid -ladvapi32 -lcfgmgr32 -lrpcrt4 -lshlwapi"
    if [ "$CCKIND" = tcc ]; then
      # The tcc build is 32-bit (i386-win32-tcc): tcc 0.9.27's x86-64 code
      # generator mixes up float arguments computed in the call itself.
      [ "$TARGET" = win-x64 ] || die "--cc=tcc builds for Windows only (use win-x64)"
      find_tcc
      CC="$TCC"
      CFLAGS="-std=c11 $OPT -DFM_SDL_DYNAMIC -DFM_NO_ASM -DSTBI_NO_SIMD -DZSTD_NO_INTRINSICS"
      VCFLAGS="$CFLAGS -w"
      CFLAGS="$CFLAGS -Wall"
      # tcc's bundled .def files miss newer exports (GetTickCount64 ...):
      # make complete ones from this machine's 32-bit system DLLs, once.
      TCCDEF="build/tccdef32"
      mkdir -p "$TCCDEF"
      WINDIR_M="$(cygpath -m "${SYSTEMROOT:-C:\\Windows}")"
      SYSDIR="$WINDIR_M/SysWOW64"
      [ -d "$SYSDIR" ] || SYSDIR="$WINDIR_M/System32"
      for d in kernel32 user32 gdi32 msvcrt; do
        [ -f "$TCCDEF/$d.def" ] || "$TCC" -impdef "$SYSDIR/$d.dll" -o "$TCCDEF/$d.def"
      done
      LIBS="-L$TCCDEF -luser32 -lgdi32"
      [ "$MODE" = release ] && LDFLAGS="-Wl,-subsystem=windows"
      OUTDIR="dist/win-x86-tcc"; OUT="$OUTDIR/$APP.exe"
    else
      find_zig
      CC="$ZIG cc -target $TRIPLE"
      sdl_static_windows "$TRIPLE" "$PROC"
      [ "$CMD" = sdl ] && exit 0
      SDL_LINK="static"
      CFLAGS="-std=c11 $OPT $WARN"
      VCFLAGS="-std=c11 $OPT -w"
      INC="$INC -I$SDL_PREFIX/include/SDL2"
      LIBS="$SDL_PREFIX/lib/libSDL2.a $WINLIBS"
      [ "$MODE" = release ] && LDFLAGS="-s -Wl,--subsystem,windows"
    fi
    ;;
  linux-x64|linux-arm64|freebsd-x64|macos-x64|macos-arm64)
    find_zig
    case "$TARGET" in
      linux-x64)   TRIPLE="x86_64-linux-gnu.2.17";  LIBS="-ldl -lpthread -lm" ;;
      linux-arm64) TRIPLE="aarch64-linux-gnu.2.17"; LIBS="-ldl -lpthread -lm" ;;
      freebsd-x64) TRIPLE="x86_64-freebsd";         LIBS="-lpthread -lm" ;;
      macos-x64)   TRIPLE="x86_64-macos";           LIBS="-lm" ;;
      macos-arm64) TRIPLE="aarch64-macos";          LIBS="-lm" ;;
    esac
    [ "$CMD" = sdl ] && { echo "$TARGET loads the system SDL2 at run time; nothing to build"; exit 0; }
    CC="$ZIG cc -target $TRIPLE"
    CFLAGS="-std=c11 $OPT $WARN -DFM_SDL_DYNAMIC -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64"
    VCFLAGS="-std=c11 $OPT -w -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64"
    [ "$MODE" = release ] && LDFLAGS="-s"
    case "$TARGET" in linux-*) LDFLAGS="$LDFLAGS -Wl,-rpath,\$ORIGIN/lib" ;; esac
    ;;
  android)
    find_android
    ABIS="${ABIS:-arm64-v8a x86_64}"
    OUT="$OUTDIR/$APP.apk"
    ;;
  web)
    EMSDK="${EMSDK:-/d/env/emsdk}"
    EMCC="$(command -v emcc 2>/dev/null || true)"
    if [ -z "$EMCC" ]; then
      EMCC="$(ls "$EMSDK"/upstream/emscripten/emcc 2>/dev/null | head -1)"
    fi
    [ -n "$EMCC" ] || die "emcc not found (activate emsdk or set EMSDK)"
    # emcc needs python 3.10+; prefer the one emsdk ships over an older system python
    if [ -z "${EMSDK_PYTHON:-}" ]; then
      for py in "$EMSDK"/python/*/python.exe "$EMSDK"/python/*/bin/python3; do
        [ -x "$py" ] && { export EMSDK_PYTHON="$py"; break; }
      done
    fi
    [ "$CMD" = sdl ] && { echo "web uses emscripten's SDL2 port; nothing to build"; exit 0; }
    CC="$EMCC"
    CFLAGS="-std=c11 $OPT $WARN -sUSE_SDL=2 -DFM_NO_ASM"
    VCFLAGS="-std=c11 $OPT -w -sUSE_SDL=2 -D_POSIX_C_SOURCE=200809L -DHAVE_ALLOCA_H"   # bzip2 uses fdopen; opus alloca
    SDL_LINK="static"
    LDFLAGS="-sUSE_SDL=2 -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432 -lidbfs.js --shell-file web/index.html"
    OUT="$OUTDIR/index.html"
    ;;
esac

# ---- bundled FFmpeg (--with-ffmpeg=DIR) -------------------------------------------

# Copies the libraries fdec_vid.c loads (it looks next to the exe, in lib/ on
# Linux/BSD and in Contents/Frameworks on macOS) plus FFmpeg's licence.
copy_ffmpeg() {
  case "$TARGET" in
    android|web) die "--with-ffmpeg: not used on $TARGET (it uses the OS decoders)" ;;
  esac
  [ "$CCKIND" = tcc ] && die "--with-ffmpeg: the tcc build is 32-bit and never loads FFmpeg"
  [ -d "$FFMPEG_DIR" ] || die "--with-ffmpeg: no folder '$FFMPEG_DIR'"
  local dest pats src n=0
  case "$TARGET" in
    win-*)   dest="$OUTDIR"; pats="avformat-*.dll avcodec-*.dll avutil-*.dll swscale-*.dll swresample-*.dll" ;;
    macos-*) dest="$BUNDLE/Contents/Frameworks"
             pats="libavformat.*.dylib libavcodec.*.dylib libavutil.*.dylib libswscale.*.dylib libswresample.*.dylib" ;;
    *)       dest="$OUTDIR/lib"; pats="libavformat.so.* libavcodec.so.* libavutil.so.* libswscale.so.* libswresample.so.*" ;;
  esac
  mkdir -p "$dest"
  for sub in bin lib .; do
    [ -d "$FFMPEG_DIR/$sub" ] || continue
    for p in $pats; do
      for src in "$FFMPEG_DIR/$sub"/$p; do
        [ -f "$src" ] || continue
        cp -L "$src" "$dest/" && n=$((n + 1))
      done
    done
    [ $n -gt 0 ] && break
  done
  [ $n -gt 0 ] || die "--with-ffmpeg: no FFmpeg shared libraries in '$FFMPEG_DIR' (bin/, lib/ or the folder itself)"
  local lic=""
  for f in "$FFMPEG_DIR"/LICENSE* "$FFMPEG_DIR"/COPYING* "$FFMPEG_DIR"/../LICENSE*; do
    [ -f "$f" ] && { lic="$f"; break; }
  done
  if [ -n "$lic" ]; then
    cp "$lic" "$OUTDIR/FFMPEG-LICENSE.txt"
    grep -qi "GNU GENERAL PUBLIC LICENSE" "$lic" && ! grep -qi "LESSER" "$lic" &&
      echo "  warning: this FFmpeg build is GPL; the bundle you ship becomes GPL (use an LGPL build)" >&2
  else
    echo "  warning: no FFmpeg licence file found in '$FFMPEG_DIR'; ship one with the bundle" >&2
  fi
  cat > "$OUTDIR/FFMPEG-NOTICE.txt" <<'EOF'
This package includes FFmpeg (https://ffmpeg.org) as separate shared libraries,
loaded at run time and replaceable with any compatible build. FFmpeg is licensed
under the LGPL v2.1 or later (see FFMPEG-LICENSE.txt); its source code is at
https://ffmpeg.org/download.html and https://git.ffmpeg.org/ffmpeg.git
EOF
  echo "  ffmpeg: $n libraries -> ${dest#./}"
}

# ---- compile + link (one ABI or one desktop target) -------------------------------

# compile_all <cc> <cflags> <vendor cflags> <objdir>  -> sets OBJS
compile_all() {
  local cc="$1" cflags="$2" vflags="$3" objdir="$4"
  mkdir -p "$objdir/src" "$objdir/vendor"
  local newest_h=0 t h
  for h in src/*.h; do
    t=$(stat -c %Y "$h"); [ "$t" -gt "$newest_h" ] && newest_h=$t
  done
  OBJS=""
  # Three parallel lists: source, object, flags kind (s = ours, v = vendor).
  local todo_src=() todo_obj=() todo_kind=() s o
  for s in $SRCS; do
    o="$objdir/src/$(basename "${s%.c}").o"
    OBJS="$OBJS $o"
    if [ ! -f "$o" ] || [ "$s" -nt "$o" ] || [ "$(stat -c %Y "$o")" -lt "$newest_h" ]; then
      todo_src+=("$s"); todo_obj+=("$o"); todo_kind+=(s)
    fi
  done
  for s in $VENDOR_SRCS; do
    o="$objdir/vendor/$(echo "${s#vendor/}" | tr '/' '_' | sed 's/\.c$/.o/')"
    OBJS="$OBJS $o"
    if [ ! -f "$o" ] || [ "$s" -nt "$o" ]; then
      todo_src+=("$s"); todo_obj+=("$o"); todo_kind+=(v)
    fi
  done
  local failed=0 pids="" n=0 i flags p
  for ((i = 0; i < ${#todo_src[@]}; i++)); do
    if [ "${todo_kind[$i]}" = s ]; then flags="$cflags"; else flags="$vflags"; fi
    echo "  cc  ${todo_src[$i]}"
    # shellcheck disable=SC2086
    $cc $flags $COMMON_DEFS $INC -c "${todo_src[$i]}" -o "${todo_obj[$i]}" &
    pids="$pids $!"
    n=$((n + 1))
    if [ "$n" -ge "$JOBS" ]; then
      for p in $pids; do wait "$p" || failed=1; done
      pids=""; n=0
    fi
  done
  for p in $pids; do wait "$p" || failed=1; done
  [ "$failed" -eq 0 ] || die "build failed"
}

gen_assets

if [ "$TARGET" != android ]; then
  OBJDIR="build/$TARGET-$MODE${BUILD_TAG:+-$BUILD_TAG}"
  [ "$CCKIND" = tcc ] && OBJDIR="build/win-x86-tcc-$MODE${BUILD_TAG:+-$BUILD_TAG}"
  if [ -n "${BUILD_TAG:-}" ]; then rest="${OUT#$OUTDIR}"; OUTDIR="$OUTDIR-$BUILD_TAG"; OUT="$OUTDIR$rest"; fi
  compile_all "$CC" "$CFLAGS" "$VCFLAGS" "$OBJDIR"
  # The exe's icon (icons/, made by tools/make_icons.py): zig compiles the
  # resource script; tcc cannot, its build sets the window icon at run time.
  case "$TARGET" in
    win-*)
      if [ "$CCKIND" != tcc ] && [ -f icons/mmcfm.rc ]; then
        if [ ! -f "$OBJDIR/mmcfm.res" ] || [ icons/mmcfm.ico -nt "$OBJDIR/mmcfm.res" ]; then
          echo "  rc  icons/mmcfm.rc"
          "$ZIG" rc /i icons /fo "$OBJDIR/mmcfm.res" icons/mmcfm.rc || die "zig rc failed"
        fi
        OBJS="$OBJS $OBJDIR/mmcfm.res"
      fi
      ;;
  esac
  mkdir -p "$OUTDIR"
  echo "  ld  $OUT"
  # shellcheck disable=SC2086
  $CC $OBJS $LDFLAGS $LIBS -o "$OUT"

  # Platform packaging.
  case "$TARGET" in
    win-*)
      if [ "$CCKIND" = tcc ]; then
        sdl_shared_windows
        cp "$SDL_DLL" "$OUTDIR/"
      fi
      ;;
    linux-*|freebsd-*)
      # the icon the .desktop names (copy both to ~/.local/share/applications
      # and ~/.local/share/icons/hicolor/256x256/apps to install)
      cp icons/mmcfm.png "$OUTDIR/$APP.png"
      sed -e "s/@APP@/$APP/g" -e "s/@NAME@/$APP_NAME/g" > "$OUTDIR/$APP.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=@NAME@
Exec=@APP@ %U
Icon=@APP@
Terminal=false
Categories=System;FileManager;Utility;
MimeType=inode/directory;
EOF
      ;;
    macos-*)
      BUNDLE="$OUTDIR/$APP_NAME.app"
      rm -rf "$BUNDLE"
      mkdir -p "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Frameworks" "$BUNDLE/Contents/Resources"
      mv "$OUT" "$BUNDLE/Contents/MacOS/$APP"
      cp icons/mmcfm.icns "$BUNDLE/Contents/Resources/$APP.icns"
      cat > "$BUNDLE/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>$APP_NAME</string>
  <key>CFBundleDisplayName</key><string>$APP_NAME</string>
  <key>CFBundleIdentifier</key><string>$APP_ID</string>
  <key>CFBundleVersion</key><string>$APP_VERSION</string>
  <key>CFBundleShortVersionString</key><string>$APP_VERSION</string>
  <key>CFBundleExecutable</key><string>$APP</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleIconFile</key><string>$APP.icns</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSMinimumSystemVersion</key><string>10.13</string>
</dict></plist>
EOF
      echo "  note: put libSDL2.dylib in '$BUNDLE/Contents/Frameworks' for a self-contained bundle"
      OUT="$BUNDLE"
      ;;
    web)
      cp icons/favicon.png "$OUTDIR/favicon.png"
      ;;
  esac
  [ -n "$FFMPEG_DIR" ] && copy_ffmpeg
  echo "built $OUT"

  if [ "$CMD" = run ] || [ "$CMD" = selftest ]; then
    case "$TARGET" in
      win-x64) ;;
      *) [ "$HOST" = win ] && die "cannot run a $TARGET build on Windows" ;;
    esac
    ARGS=""; [ "$CMD" = selftest ] && ARGS="--selftest"
    if [ "$TARGET" = macos-x64 ] || [ "$TARGET" = macos-arm64 ]; then
      "$OUT/Contents/MacOS/$APP" $ARGS
    else
      "./$OUT" $ARGS
    fi
  fi
  exit 0
fi

# ---- android --------------------------------------------------------------------

if [ "$CMD" = selftest ]; then die "selftest runs on desktop targets only"; fi

# BUILD_TAG keeps parallel Android builds apart, as on desktop
ATAG="${BUILD_TAG:+-$BUILD_TAG}"
if [ -n "$ATAG" ]; then OUTDIR="$OUTDIR$ATAG"; OUT="$OUTDIR/$APP.apk"; fi
APKDIR="build/android-$MODE$ATAG/apk"
rm -rf "$APKDIR"
mkdir -p "$APKDIR/lib"
for ABI in $ABIS; do
  case "$ABI" in
    arm64-v8a) CTRIPLE="aarch64-linux-android$ANDROID_API" ;;
    x86_64)    CTRIPLE="x86_64-linux-android$ANDROID_API" ;;
    *) die "unknown ABI $ABI (arm64-v8a or x86_64)" ;;
  esac
  sdl_static_android "$ABI"
  [ "$CMD" = sdl ] && continue
  CLANG="$NDK_BIN/clang$EXE"
  CC="$CLANG --target=$CTRIPLE"
  ACFLAGS="-std=c11 $OPT $WARN -fPIC -ffunction-sections -fdata-sections -I$SDL_PREFIX/include/SDL2"
  AVFLAGS="-std=c11 $OPT -w -fPIC -ffunction-sections -fdata-sections"
  compile_all "$CC" "$ACFLAGS" "$AVFLAGS" "build/android-$ABI-$MODE$ATAG"
  mkdir -p "$APKDIR/lib/$ABI"
  echo "  ld  lib/$ABI/libmain.so"
  # shellcheck disable=SC2086
  $CC -shared $OBJS "$SDL_PREFIX/lib/libSDL2.a" -o "$APKDIR/lib/$ABI/libmain.so" \
    -Wl,--gc-sections -Wl,-z,max-page-size=16384 -Wl,--no-undefined \
    -landroid -llog -lGLESv1_CM -lGLESv2 -lEGL -lOpenSLES -laaudio -ldl -lm
  "$NDK_BIN/llvm-strip$EXE" "$APKDIR/lib/$ABI/libmain.so"
done
[ "$CMD" = sdl ] && exit 0

# Java: SDL's activity classes (from the vendored source) + FmActivity.
sdl_source
JOUT="build/android-$MODE$ATAG/java"
rm -rf "$JOUT"; mkdir -p "$JOUT/classes" "$JOUT/dex"
JAVA_SRCS="$(ls "$SDL_SRC"/android-project/app/src/main/java/org/libsdl/app/*.java) $(find android/java -name '*.java')"
WJ=""; for j in $JAVA_SRCS; do WJ="$WJ $(W "$j")"; done
echo "  javac"
# shellcheck disable=SC2086
"${JAVA_BIN}javac" -nowarn -source 11 -target 11 -encoding UTF-8 \
  -cp "$(W "$PLATFORM_JAR")" -d "$(W "$JOUT/classes")" $WJ 2> "$JOUT/javac.log" || {
  cat "$JOUT/javac.log" >&2; die "javac failed"; }
echo "  d8"
CLASSFILES="$(find "$JOUT/classes" -name '*.class')"
WC=""; for c in $CLASSFILES; do WC="$WC $(W "$c")"; done
D8="$BT/d8"; [ -f "$D8.bat" ] && D8="$D8.bat"
# shellcheck disable=SC2086
"$D8" --min-api $ANDROID_API $([ "$MODE" = release ] && echo --release) \
  --lib "$(W "$PLATFORM_JAR")" --output "$(W "$JOUT/dex")" $WC
cp "$JOUT/dex/classes.dex" "$APKDIR/"

# Resources + manifest.
RES="build/android-$MODE$ATAG/res"
rm -rf "$RES"; mkdir -p "$RES"
echo "  aapt2"
"$BT/aapt2$EXE" compile --dir "$(W android/res)" -o "$(W "$RES/res.zip")"
"$BT/aapt2$EXE" link -o "$(W "$RES/base.apk")" -I "$(W "$PLATFORM_JAR")" \
  --manifest "$(W android/AndroidManifest.xml)" \
  --min-sdk-version $ANDROID_API --target-sdk-version 35 \
  --version-code $APP_VERSION_CODE --version-name $APP_VERSION \
  "$(W "$RES/res.zip")"

# Add dex + native libraries, both stored uncompressed: native libraries so the
# loader can map them straight from the APK (extractNativeLibs=false), the dex
# because it loads a little faster and does not depend on the zip deflater.
cp "$RES/base.apk" "$RES/unaligned.apk"
( cd "$APKDIR" && zip -q -0 -r "../../../$RES/unaligned.apk" lib classes.dex )
mkdir -p "$OUTDIR"
"$BT/zipalign$EXE" -f -P 16 4 "$(W "$RES/unaligned.apk")" "$(W "$RES/aligned.apk")"

# Signing: a release key from KEYSTORE, otherwise a debug key made once.
if [ -n "${KEYSTORE:-}" ]; then
  KS="$KEYSTORE"; KA="${KEY_ALIAS:-release}"; KP="${KEY_PASS:?set KEY_PASS}"
else
  KS="build/debug.keystore"; KA="androiddebugkey"; KP="android"
  if [ ! -f "$KS" ]; then
    echo "  keytool (debug key, once)"
    "${JAVA_BIN}keytool" -genkeypair -keystore "$(W "$KS")" -storepass android -keypass android \
      -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
      -dname "CN=Android Debug,O=Android,C=US" > /dev/null 2>&1
  fi
fi
APKSIGNER="$BT/apksigner"; [ -f "$APKSIGNER.bat" ] && APKSIGNER="$APKSIGNER.bat"
echo "  apksigner"
"$APKSIGNER" sign --ks "$(W "$KS")" --ks-key-alias "$KA" --ks-pass "pass:$KP" --key-pass "pass:$KP" \
  --out "$(W "$OUT")" "$(W "$RES/aligned.apk")"
rm -f "$OUT.idsig"
echo "built $OUT ($(du -h "$OUT" | awk '{print $1}'))"

if [ "$CMD" = run ]; then
  ADB="$SDK/platform-tools/adb$EXE"
  "$ADB" install -r "$(W "$OUT")"
  "$ADB" shell am start -n "$APP_ID/$APP_ID.FmActivity"
fi
