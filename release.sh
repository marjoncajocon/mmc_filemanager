#!/usr/bin/env bash
#
# release.sh -- build every target in release mode and pack each one into a
# single file, all side by side in release/<version>/ (ready to drag into a
# GitHub release, or to upload with --upload).
#
#   ./release.sh [options] [target ...]
#
# Targets (default: all of them; a target whose toolchain is missing fails on
# its own and the rest still pack):
#   win-x64 win-arm64 win-x86-tcc linux-x64 linux-arm64 macos-x64 macos-arm64
#   freebsd-x64 android web
#
# Options:
#   --version=X    version in the file names (default: APP_VERSION from build.sh)
#   --jobs=N       parallel compiles (default 4)
#   --with-ffmpeg=DIR  passed to the desktop zig builds (see build.sh)
#   --upload       after packing, create a DRAFT GitHub release vX with the files
#                  (needs the gh CLI, logged in: gh auth login); you publish it
#                  on GitHub after a look
#
# Output (example for 0.1.0):
#   release/0.1.0/mmcfm-0.1.0-win-x64.zip          mmcfm.exe
#   release/0.1.0/mmcfm-0.1.0-linux-x64.tar.gz     mmcfm, .desktop, icon
#   release/0.1.0/mmcfm-0.1.0-macos-arm64.tar.gz   MMC File Manager.app
#   release/0.1.0/mmcfm-0.1.0-android.apk
#   release/0.1.0/mmcfm-0.1.0-web.zip              index.html + .js + .wasm
#   release/0.1.0/SHA256SUMS.txt
# Each archive holds one folder, mmcfm-<version>-<target>/, with LICENSE and
# LICENSES.md beside the program.
#
# Builds go to build/*-rel and dist/*-rel (BUILD_TAG=rel), so they never touch
# the everyday dist/<target>/ folders, or an exe that is running.
#
# Android: set KEYSTORE / KEY_ALIAS / KEY_PASS for a release-signed APK.
# Without them build.sh signs with its debug key: it installs fine, but a later
# APK signed with another key cannot update it (uninstall first).

set -uo pipefail
cd "$(dirname "$0")"

ALL_TARGETS="win-x64 win-arm64 win-x86-tcc linux-x64 linux-arm64 macos-x64 macos-arm64 freebsd-x64 android web"
VERSION="$(sed -n 's/^APP_VERSION="\(.*\)"/\1/p' build.sh)"
JOBS="${JOBS:-4}"
FFMPEG=""
UPLOAD=0
TARGETS=""

for a in "$@"; do
  case "$a" in
    --version=*) VERSION="${a#*=}" ;;
    --jobs=*) JOBS="${a#*=}" ;;
    --with-ffmpeg=*) FFMPEG="$a" ;;
    --upload) UPLOAD=1 ;;
    -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) echo "unknown option $a (see ./release.sh --help)" >&2; exit 1 ;;
    *) TARGETS="$TARGETS $a" ;;
  esac
done
[ -n "$TARGETS" ] || TARGETS="$ALL_TARGETS"
[ -n "$VERSION" ] || { echo "no version (APP_VERSION in build.sh)" >&2; exit 1; }

TAG=rel
# Inside mmc-shell, build.sh runs in mmc-shell again (its builtin zip, and no
# MSYS path rewriting); a plain ./build.sh would start Git Bash instead.
if [ -n "${MMC_ROOT:-}" ] && [ -x "$MMC_ROOT/mmc-shell.exe" ]; then
  BUILD=("$MMC_ROOT/mmc-shell.exe" build.sh)
else
  BUILD=(./build.sh)
fi
OUT="release/$VERSION"
STAGE="build/release-stage"
mkdir -p "$OUT"
rm -rf "$STAGE"

# tar that marks the programs executable even when packed on Windows (GNU tar
# there sees no exec bits; bsdtar on macOS/BSD keeps the real ones)
targz() {   # targz ARCHIVE DIR-IN-STAGE
  if tar --version 2>/dev/null | grep -q GNU; then
    tar -C "$STAGE" --owner=0 --group=0 --mode=755 -czf "$1" "$2"
  else
    tar -C "$STAGE" -czf "$1" "$2"
  fi
}

zipdir() {  # zipdir ARCHIVE DIR-IN-STAGE
  local abs
  abs="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
  (cd "$STAGE" && zip -r -q -9 "$abs" "$2")
}

OK=""
FAILED=""

for t in $TARGETS; do
  echo
  echo "==== $t"
  case "$t" in
    win-x86-tcc) args="win-x64 release --cc=tcc"; dist="dist/win-x86-tcc-$TAG" ;;
    win-*|linux-*|macos-*|freebsd-*) args="$t release $FFMPEG"; dist="dist/$t-$TAG" ;;
    android|web) args="$t release"; dist="dist/$t-$TAG" ;;
    *) echo "unknown target $t"; FAILED="$FAILED $t"; continue ;;
  esac
  rm -rf "$dist"
  if ! BUILD_TAG=$TAG "${BUILD[@]}" $args --jobs="$JOBS"; then
    echo "!! $t did not build"
    FAILED="$FAILED $t"
    continue
  fi

  name="mmcfm-$VERSION-$t"
  case "$t" in
    android)
      cp "$dist/mmcfm.apk" "$OUT/$name.apk" || { FAILED="$FAILED $t"; continue; }
      ;;
    *)
      rm -rf "$STAGE/$name"
      mkdir -p "$STAGE/$name"
      cp -R "$dist/." "$STAGE/$name/"
      rm -f "$STAGE/$name"/*.pdb                     # debug symbols stay home
      cp LICENSE LICENSES.md "$STAGE/$name/"
      rm -f "$OUT/$name.zip" "$OUT/$name.tar.gz"
      case "$t" in
        win-*|web) zipdir "$OUT/$name.zip" "$name" ;;
        *) targz "$OUT/$name.tar.gz" "$name" ;;
      esac || { FAILED="$FAILED $t"; continue; }
      ;;
  esac
  OK="$OK $t"
done

rm -rf "$STAGE"

# checksums for whatever is in the folder now
(cd "$OUT" && rm -f SHA256SUMS.txt && sha256sum -- * > SHA256SUMS.txt.tmp && mv SHA256SUMS.txt.tmp SHA256SUMS.txt)

echo
echo "release $VERSION -> $OUT/"
ls -l "$OUT"
[ -z "$OK" ] || echo "packed:$OK"
[ -z "$FAILED" ] || echo "FAILED:$FAILED"

if [ "$UPLOAD" = 1 ]; then
  command -v gh >/dev/null || { echo "--upload needs the gh CLI (https://cli.github.com), then: gh auth login" >&2; exit 1; }
  gh release create "v$VERSION" "$OUT"/* --draft --title "MMC File Manager $VERSION" \
    --notes "MMC File Manager $VERSION. Check downloads against SHA256SUMS.txt." || exit 1
  echo "draft release v$VERSION created: review and publish it on GitHub"
fi

[ -z "$FAILED" ]
