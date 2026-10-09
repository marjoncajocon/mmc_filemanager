# MMC File Manager (mmcfm)

MMC File Manager is a dual-panel file manager written in plain C11 on SDL2, styled after X-plore on Android. It also has viewers for images, text, music and video, a media library, online video, photo and audio sources, and cloud storage. One code base builds for Windows, Linux, macOS, FreeBSD, Android (a native APK built without Gradle) and the web (Emscripten).

**Design goals**

- **Low memory and no idle cost.** Big files are streamed through fixed buffers and are never read whole. The event loop sleeps when nothing moves, so an idle window uses 0% CPU. Thumbnails share atlas pages, and shapes and text are batched into a few draw calls.
- **Nothing to install.** Everything is built from `vendor/`. On Windows SDL2 is linked statically into one `.exe`.
- **System libraries are loaded at run time, never linked.** This covers SDL2 (on Linux, macOS, FreeBSD and the tcc build), FFmpeg, WinHTTP, libcurl, Media Foundation, MediaCodec, dwmapi, shell32 (tray) and ws2_32. Builds and machines without them still start.
- **Old systems keep working.** Newer OS APIs are looked up dynamically and have a fallback. For example, Windows 11 gets rounded window corners and Windows 7, 8 and 10 get a window region instead. Media Foundation needs Windows 7 or later.
- **tcc must compile it.** A 32-bit Windows build with Tiny C Compiler is part of the build matrix, so the code avoids `_Atomic`, VLAs and other extensions.

Version: 0.1.0. Licence: MIT (see `LICENSE`, and `LICENSES.md` for third-party code).

---

## Features

### File management

- Two panels. Every operation goes from the active panel to the other one, and the middle action bar shows arrows pointing at the target.
- Layouts: side by side (landscape), stacked (portrait), or a single panel with a Left/Right switcher on narrow windows. The splitter can be dragged.
- File operations run as background jobs with progress, speed/ETA, pause and cancel, and they ask on name conflicts:
  - copy (F5) and move (F6)
  - delete or move to the trash (Windows Recycle Bin, or the freedesktop.org trash on Linux/BSD)
  - rename (F2), new folder (F7), new file
  - folder size, properties
  - cut/copy/paste, copy path
  - "open with system app"
- Drag and drop between panels: drag to copy, Shift to move.
- List and grid views, with image and video thumbnails decoded in the background and cached on disk.
- Sort by name (natural order), size, date or type, folders first, show hidden files.
- Filter/search row per panel, bookmarks, history, and places in the sidebar.
- Archives can be browsed like folders, extracted, and opened file by file:

  | Format | List / extract | Create |
  |---|---|---|
  | ZIP (store, deflate, bzip2, lzma, zstd, xz; ZIP64; ZipCrypto and WinZip AES) | yes | yes (ZipCrypto or AES-256) |
  | 7z (LZMA, LZMA2, PPMd, BCJ/BCJ2, AES-256, encrypted headers) | yes | yes (LZMA2, solid, AES-256) |
  | RAR 4.x and 5.0 | yes (no encrypted entries, first volume only) | no |
  | tar (ustar, pax, GNU), plain or .gz/.bz2/.xz/.zst/.lzma | yes | yes (tar, tar.gz, tar.bz2, tar.xz, tar.zst) |
  | Single compressed files (.gz, .bz2, .xz, .zst, .lzma) | yes | yes |

  Extraction refuses absolute paths and `..` ("zip slip"). Wrong passwords are asked again.
- **20 built-in themes** (24 palettes): Graphite, Daylight, Tonal, Nord, Dracula Neon, Solarized Warm, OLED Contrast, Commander, Gallery, Finder Split, X-plore Tree, Frost Layers, Pastel Soft, Phosphor, Brutal Blocks, Aurora Hero, Mono Red, Bento Board, Thumb Zone, Fold Duo. Graphite, Tonal, Solarized and Mono have both dark and light variants. There are 8 accent colours plus each theme's own, and a UI zoom from 0.75x to 2x.
- **Custom title bar** (desktop): a borderless window with its own minimize/maximize/close buttons. On macOS they are traffic lights on the left. It supports Aero Snap, double-click to maximize and resize edges. On Windows the corners are rounded. "System title bar" in Settings brings back the OS frame.
- **Touch mode**: automatic, on or off. It gives larger targets, long-press menus and swipe gestures. Dialogs move above the on-screen keyboard on Android.
- **System tray** (Windows only): a tray icon with show/hide, play/pause, next and quit. Settings > "Icon in the system tray" (on), "Minimize to the tray" (on: minimizing removes the taskbar button) and "Close button hides to the tray" (off; Quit is in the icon's menu). Music, background video and downloads keep running while hidden, and the app draws nothing (no CPU).

### Viewers

- **Images**:
  - JPEG, PNG, BMP and the other stb_image formats, animated GIF (streamed frame by frame) and SVG (re-rasterized when you zoom)
  - EXIF rotation
  - zoom and pan with inertia, swipe to the next image, double-tap zoom
- **Text**: a streamed viewer with hex mode, word wrap, selection and find. A 1 GB log costs about 1 MB of memory.
- **Music player**:
  - MP3, FLAC, WAV/AIFF and Ogg Vorbis built in; AAC, Opus, WMA and others through FFmpeg or the OS decoders
  - tags and cover art
  - keeps playing in a mini bar after the viewer closes
  - **Visualizer**: 9 styles (Bars, Mirror, Pills, Wave, Scope, Radial, Pulse, Dots, Neon) plus a custom slot with its own knobs
  - **Equalizer**: 16 presets plus a 10-band custom curve (±12 dB), preamp, stereo width, balance and a limiter. It is shared with the video player.
- **Video player**:
  - Backends are tried in order:
    1. pl_mpeg (MPEG-1)
    2. FFmpeg 4-8 shared libraries, if found
    3. the OS decoder: Media Foundation on Windows, MediaCodec on Android
    4. the built-in portable decoder (WebM with VP9 and Opus) on every target, tcc and web included
  - Picture shapes: Fit, Fill, Stretch, 16:9, 4:3, 21:9, 1:1, 9:16 and Original (key A).
  - Touch lock.
  - **Play in background (sound only)** (key B): online videos switch to the audio-only quality.
  - **Picture in picture** (desktop, key P): the window shrinks to a small always-on-top player in the bottom-right corner. Android's system PiP is not done yet.
  - Visualizer overlay and equalizer.

### Media library

- A full-window library with Songs, Artists, Albums, Videos, Favorites, Recent, Playlists and Folders sections.
- It indexes the Music and Videos folders plus folders you add, in the background.
- There are favourite (heart) buttons in the music and video players.

### Music queue and playlists

- Play next, add to queue and add to playlist work from the file panels (multi-select, whole folders), the library and online audio.
- The queue panel can be reordered by dragging (long-press on touch, Alt+Up/Down on the keyboard).
- Shuffle mixes only the tracks that come next, and turning it off restores the order.
- The queue is saved and restored, paused at the saved position.
- Named playlists can mix local files and online tracks. Lists of local files export to `.m3u8`, and `.m3u` files can be imported.

### Online videos

Search, stream with a buffer, pick a quality from the menu (it reopens at the current time), and download.

| Source | Notes |
|---|---|
| YouTube | Built in through YouTube's InnerTube API, with no key and no yt-dlp. 144p to 2160p60. An optional free Data API v3 key (Settings) makes search use the official API. |
| Internet Archive | Direct streams, no key. |
| PeerTube | Through SepiaSearch, no key. |
| Dailymotion | Native, with its own HLS reader, no key. |
| Bilibili | Native (WBI-signed web API). Up to 720p when not signed in. Accepts bilibili.com and b23.tv links. |
| Any site | Paste a page link. Plays pages that contain a plain video (media links, JSON-LD, og:video, `<video>`). |

- **yt-dlp is optional.** It is a fallback for playlists, live streams, cache-first playback and HLS downloads. Settings can download it ("Get yt-dlp"). Current yt-dlp needs a JavaScript runtime (deno or node) for YouTube.
- HLS is supported (TS or fMP4, AES-128, live reload), and so are separate video and audio streams played as a pair, and expired links that are resolved again.

### Online photos

Openverse, Wikimedia Commons, NASA Images and the Art Institute of Chicago need no key. Pexels, Unsplash and Pixabay need a free key.

- Justified photo wall.
- Lightbox with wheel, pinch and double-tap zoom that loads the full-size image.
- Favourites and local albums.
- Background downloads that save a `.txt` credit/licence file next to each photo.

### Online audio

| Source | Notes |
|---|---|
| Radio Browser | Over 50,000 live stations, with now-playing titles (ICY). MP3 built in, AAC through FFmpeg or Media Foundation. |
| YouTube | Sound only (Opus), no key. |
| Audius | Independent artists, no key. |
| Internet Archive | Concerts, old-time radio, audiobooks, 78s, no key. |
| Podcasts | Apple's directory, played from each show's own RSS feed, no key. |
| Jamendo | Free client id needed. |
| Freesound | Free key needed. |

Everything streams, with nothing downloaded before playback. Downloads are optional.

### Cloud storage

Cloud accounts appear as places, and their folders open in the panels like local ones. Uploads, downloads and cloud-to-cloud copies run as normal jobs, and large files stream in chunks.

| Service | Sign-in |
|---|---|
| Google Drive | OAuth with **your own** free "Desktop app" client id **and** client secret (Google Cloud Console, enable the Drive API). Google Docs, Sheets and Slides download as .docx, .xlsx and .pptx exports. |
| Dropbox | OAuth with **your own** app key as the client id, no secret. Create a Scoped access app, allow public clients (PKCE), and add the redirect URI `http://127.0.0.1`. |
| OneDrive | OAuth with **your own** Application (client) ID. In the Azure App registration choose personal accounts, public client redirect `http://localhost`, allow public client flows, and grant Files.ReadWrite. |
| MEGA | Email and password, or public file/folder links. Files are decrypted and MAC-checked locally. |
| WebDAV | URL, user and password. Nextcloud chunked upload above 100 MB. Plain http only for LAN hosts. |
| S3-compatible | Endpoint, region, bucket, key and secret (SigV4, path-style, multipart above 64 MB). |

OAuth uses the system browser with PKCE and a loopback redirect. The sign-in form explains each step. Saved credentials are encrypted with DPAPI on Windows and only obfuscated on other systems.

---

## Building

### Prerequisites

There is one script, `build.sh`. It needs a POSIX shell: [mmc-shell](#mmc-shell), Git Bash, MSYS2, Linux or macOS. Everything else comes from `vendor/`. The script never picks up an SDL2 DLL or headers from the system.

| Tool | Needed for | Where `build.sh` looks |
|---|---|---|
| [zig](https://ziglang.org) (`zig cc`) | all desktop targets, the static SDL2, the 32-bit SDL2.dll for tcc | `$ZIG`, then `zig` on PATH, then `/d/env/zig/zig.exe` |
| CMake + Ninja | building SDL2 from `vendor/SDL2-2.32.10.tar.gz` (Windows and Android) | `$CMAKE`, then `cmake` on PATH, then the Android SDK's `cmake/*/bin/cmake.exe`; Ninja next to cmake or on PATH |
| tcc 0.9.27 (`i386-win32-tcc`) | `--cc=tcc` only | `$TCC`, then `i386-win32-tcc` on PATH, then `/d/env/tcc/i386-win32-tcc.exe` |
| Android SDK + NDK, JDK 11+ | `android` | SDK: `$ANDROID_HOME`, `$ANDROID_SDK_ROOT`, then `/d/env/Android/Sdk`. NDK: `$NDK` or the newest `$SDK/ndk/*`. Newest build-tools and `platforms/android-*`. JDK: `$JAVA_HOME/bin`, else `/d/env/java/jdk-21.0.11/bin` |
| emsdk | `web` | `emcc` on PATH, else `$EMSDK/upstream/emscripten/emcc` (`EMSDK` defaults to `/d/env/emsdk`). Prefers emsdk's own Python 3.10+ (`EMSDK_PYTHON`) |
| `zip` | `android` (packing the APK) | PATH |

<a id="mmc-shell"></a>On Windows the project is built through **mmc-shell**:

```sh
D:/mmc-shell/mmc-shell.exe build.sh [target] [debug|release] [options] [command]
```

On Linux, macOS, Git Bash or MSYS2 run `./build.sh ...` directly.

### Usage

```
./build.sh [target] [debug|release] [options] [command]
```

Arguments can come in any order. An unknown argument is an error. `./build.sh --help` (or `-h`) prints the summary.

#### Targets

The default is `win-x64` on Windows, otherwise the host (`macos-arm64`/`macos-x64`, `freebsd-x64`, `linux-arm64`/`linux-x64`).

| Target | Compiler | SDL2 | Output |
|---|---|---|---|
| `win-x64` | `zig cc -target x86_64-windows-gnu` | static, inside the exe | `dist/win-x64/mmcfm.exe` |
| `win-arm64` | `zig cc -target aarch64-windows-gnu` | static, inside the exe | `dist/win-arm64/mmcfm.exe` |
| `linux-x64` | `zig cc -target x86_64-linux-gnu.2.17` (glibc 2.17+) | loaded at run time (`lib/libSDL2-2.0.so.0` next to the exe, or the system one) | `dist/linux-x64/mmcfm` + `mmcfm.desktop` |
| `linux-arm64` | `zig cc -target aarch64-linux-gnu.2.17` | loaded at run time | `dist/linux-arm64/mmcfm` + `mmcfm.desktop` |
| `macos-x64` | `zig cc -target x86_64-macos` | loaded at run time (`Contents/Frameworks/libSDL2-2.0.0.dylib`, Homebrew or the system) | `dist/macos-x64/MMC File Manager.app` (minimum macOS 10.13) |
| `macos-arm64` | `zig cc -target aarch64-macos` | loaded at run time | `dist/macos-arm64/MMC File Manager.app` |
| `freebsd-x64` | `zig cc -target x86_64-freebsd` | loaded at run time | `dist/freebsd-x64/mmcfm` + `mmcfm.desktop` |
| `android` | NDK clang (API 26+, target SDK 35) | static, built per ABI | `dist/android/mmcfm.apk` |
| `web` | `emcc -sUSE_SDL=2` (Emscripten's SDL2 port) | static | `dist/web/index.html` (+ `.js`/`.wasm`), shell from `web/index.html` |

The macOS bundle does not include SDL2. Put `libSDL2.dylib` into `Contents/Frameworks` for a self-contained app, or install SDL2 (for example with Homebrew).

#### Build modes

| Mode | Flags |
|---|---|
| `release` (default) | `-O2 -DNDEBUG`, stripped (`-s`). Windows builds use the GUI subsystem (no console). |
| `debug` | `-O1 -g`, keeps the console on Windows. |

#### Options

| Option | Meaning |
|---|---|
| `--cc=zig` | Default compiler for desktop targets. |
| `--cc=tcc` | A **32-bit Windows** build with Tiny C Compiler (use with `win-x64`, the only target it accepts). SDL2 is loaded at run time from a 32-bit `SDL2.dll` that the script builds once from `vendor/` with zig and copies next to the exe. Output: `dist/win-x86-tcc/mmcfm.exe` + `SDL2.dll`, objects in `build/win-x86-tcc-<mode>/`. It also builds without inline asm and SIMD, and makes `.def` import files from the machine's 32-bit system DLLs (`build/tccdef32/`). |
| `--jobs=N` | Parallel compiles (default 4, or `$JOBS`). |
| `--no-asm` | Builds without the inline-assembly fast paths (`-DFM_NO_ASM`). The C versions are always present. |
| `--with-ffmpeg=DIR` | Copies FFmpeg 4-8 **shared** libraries (avformat, avcodec, avutil, swscale, swresample) from `DIR`, `DIR/bin` or `DIR/lib` into the output so every video format plays. On Windows they go next to the exe, on Linux/BSD into `lib/`, and on macOS into `Contents/Frameworks`. It also writes `FFMPEG-LICENSE.txt` and `FFMPEG-NOTICE.txt`. Desktop zig builds only (refused for `android`, `web` and `--cc=tcc`). **Use an LGPL build**: the script warns if the licence is GPL, which would make the shipped bundle GPL. Without this option, video uses the OS decoders, an FFmpeg already on the system, or the built-in decoders. |

#### Commands

| Command | Meaning |
|---|---|
| *(none)* | Build. |
| `clean` | Removes `build/` and `dist/` entirely (all targets and tags). |
| `sdl` | Builds only the static SDL2 for the target (Windows: `build/sdl/<triple>/`; Android: `build/sdl/android-<abi>/`). There is nothing to build for Linux, macOS, BSD or web. |
| `run` | Builds, then starts the result from `dist/`. On a Windows host only `win-x64` (including tcc) can run. For `android` it installs the APK with `adb install -r` and starts `io.github.mmc.filemanager/.FmActivity`. |
| `selftest` | Builds, then runs `mmcfm --selftest`. Desktop targets only. |

#### Environment overrides

| Variable | Meaning |
|---|---|
| `ZIG` | Path to `zig`. |
| `TCC` | Path to `i386-win32-tcc`. |
| `CMAKE` | Path to `cmake` (Ninja is looked for next to it, then on PATH). |
| `ANDROID_HOME` / `ANDROID_SDK_ROOT` | Android SDK folder. |
| `NDK` | NDK folder (default: newest under `$SDK/ndk`). |
| `JAVA_HOME` | JDK used for `javac` and `keytool`. |
| `EMSDK` | emsdk folder, used when `emcc` is not on PATH. |
| `EMSDK_PYTHON` | Python for emcc (default: the one inside emsdk). |
| `JOBS` | Default for `--jobs`. |
| `BUILD_TAG=x` | Separate folders so several builds can run in parallel: `build/<target>-<mode>-x/` and `dist/<target>-x/` (tcc: `dist/win-x86-tcc-x/`; Android: `build/android-<mode>-x/`, `dist/android-x/`). |
| `ABIS` | Android ABIs, default `"arm64-v8a x86_64"` (only those two are known). |
| `KEYSTORE` | Release keystore for signing the APK. |
| `KEY_ALIAS` | Key alias in `KEYSTORE` (default `release`). |
| `KEY_PASS` | Store and key password (required when `KEYSTORE` is set). |

#### Output and intermediate files

- Programs: `dist/<target>[-<tag>]/`. The tcc build goes to `dist/win-x86-tcc[-<tag>]/`.
- Objects: `build/<target>-<mode>[-<tag>]/`.
- Shared build artefacts in `build/`:
  - SDL2 unpacked once into `build/SDL2-2.32.10/`, SDL2 builds in `build/sdl/`
  - the embedded fonts and SDL redirects generated into `build/gen/`
  - the Android debug key in `build/debug.keystore`
- Builds are incremental: a source is recompiled when it or any `src/*.h` is newer than its object.

### Examples

```sh
# Windows (through mmc-shell)
D:/mmc-shell/mmc-shell.exe build.sh                         # release win-x64
D:/mmc-shell/mmc-shell.exe build.sh win-x64 debug run       # debug build, then start it
D:/mmc-shell/mmc-shell.exe build.sh win-arm64
D:/mmc-shell/mmc-shell.exe build.sh win-x64 --cc=tcc        # 32-bit tcc build + SDL2.dll
D:/mmc-shell/mmc-shell.exe build.sh win-x64 --no-asm
D:/mmc-shell/mmc-shell.exe build.sh selftest                # build, then mmcfm --selftest
D:/mmc-shell/mmc-shell.exe build.sh win-x64 --with-ffmpeg=C:/ffmpeg-lgpl-shared

# cross-compile the other desktops with zig
./build.sh linux-x64
./build.sh linux-arm64
./build.sh macos-arm64
./build.sh macos-x64 --with-ffmpeg=/opt/ffmpeg
./build.sh freebsd-x64

# Android
./build.sh android                                           # debug-signed APK
ABIS=arm64-v8a ./build.sh android debug                      # one ABI, faster
KEYSTORE=~/keys/release.jks KEY_ALIAS=mmcfm KEY_PASS=secret ./build.sh android release
./build.sh android run                                       # build, adb install, start

# web
EMSDK=/d/env/emsdk ./build.sh web

# parallel builds in separate folders
BUILD_TAG=a ./build.sh win-x64 --jobs=8 &
BUILD_TAG=b ./build.sh linux-x64 --jobs=8 &

./build.sh clean
```

### Notes per target

- **Android.**
  - The APK is packed without Gradle: javac (SDL's Java glue from the SDL source plus `android/java`), d8, aapt2 (`android/res`, `android/AndroidManifest.xml`), zip, zipalign (16 KB page alignment) and apksigner.
  - Native libraries and `classes.dex` are stored uncompressed.
  - Without `KEYSTORE` the APK is signed with a debug key made once in `build/debug.keystore` (alias `androiddebugkey`, password `android`).
  - Permissions: storage (including all-files access), media, internet, network state and wake lock.
- **Web.**
  - The app runs on Emscripten's in-memory file system (a demo; the home folder starts empty).
  - Serve `dist/web/` from any static HTTP server.
  - `run` does not work on a Windows host.
- **tcc.** The tcc build is 32-bit because tcc 0.9.27's x86-64 code generator mishandles some float arguments. It never loads FFmpeg, and libvpx runs single-threaded.
- **FFmpeg.** It is never linked. When FFmpeg libraries are next to the program (or in `lib/` or `Frameworks`) or installed on the system, they are loaded at run time. FFmpeg 4 to 8 (avformat 58-62) is supported.

---

## Running

```sh
mmcfm [folder-or-file]
```

A folder opens in the left panel. A file opens its folder with the file selected, and an archive opens inside it.

### Command-line options

| Option | Meaning |
|---|---|
| `--version` | Prints `mmcfm <version>`. |
| `--selftest` | Runs the built-in tests without a window and exits with 0 when all pass. On Android the report goes to `<external files>/selftest.txt`. |
| `--left DIR`, `--right DIR` | Folder (or file) for each panel. |
| `--theme N\|name` | Theme by index (0-19) or name. |
| `--dark`, `--light` | Dark or light variant. |
| `--grid` | Grid view in both panels. |
| `--layout auto\|side\|stack\|single` | Panel layout. |
| `--touch` | Force touch mode. |
| `--shot FILE.bmp [--size WxH] [--frames N]` | Renders the UI with default settings (nothing is loaded or saved), saves a 32-bit BMP and exits. Default size 1180x740, 6 frames. |
| `--perf [--size WxH] [--frames N] [--out FILE] [--perf-shots DIR] [--perf-only a,b] [--perf-probe]` | Performance tour of 14 screens on a generated 2,000-file folder: CPU per frame, draw calls, textures, memory, then 5 s of idle. `--perf-probe` measures the driver's texture and geometry cost. |

Demo flags put a screen into a given state for screenshots and visual checks:

| Flag | Shows |
|---|---|
| `--demo-dialog NAME` | a dialog (for example `settings`) |
| `--demo-select`, `--demo-job` | a selection, a running job card |
| `--demo-library SECTION` | the media library on the `--left` folder |
| `--demo-audio FILE [--demo-viz N] [--demo-viz-panel] [--demo-viz-overlay] [--demo-eq N]` | the music player, visualizer and equalizer |
| `--demo-video FILE [--demo-aspect N] [--demo-lock] [--demo-chrome] [--demo-video-bg AT[,BACK]] [--demo-video-pip AT[,OUT]]` | the video player, picture shape, touch lock, background play, picture in picture |
| `--demo-quality-menu`, `--demo-stream-seeks` | the streaming quality menu, seek runs |
| `--demo-online SRC [QUERY] [--demo-online-state S]` | online videos |
| `--demo-photos SRC [QUERY] [--demo-photos-state S]` | online photos (for example `--demo-photos wikimedia lighthouse --demo-photos-state zoom`) |
| `--demo-audio-online SRC [QUERY] [--demo-audio-online-state S]` | online audio |
| `--demo-queue DIR [--demo-queue-state queue\|drag\|mini\|player\|playlists\|playlist]` | the play queue and playlists (parked, no sound) |
| `--demo-cloud [panel\|folder\|loading\|signedout\|delete\|job\|home\|add\|form-KEY\|link\|settings]` | cloud storage with the in-memory "Demo cloud" |

Example: `mmcfm --shot out.bmp --theme 15 --demo-dialog settings`.

### Self test

`mmcfm --selftest` runs these areas in order, each in its own temporary folder:

`core`, `crypto`, `archives`, `7z-rar`, `fs`, `media`, `soft-video`, `themes`, `net`, `viz`, `library`, `queue`, `vsrc`, `hls`, `generic`, `online-ui`, `psrc`, `photo-ui`, `asrc`, `aonline-ui`, `cloud`, `oauth`, `dav`, `mega`.

The tests are offline by default. Network checks are opt-in through the variables below.

### Environment variables (test and debug hooks)

| Variable | Effect |
|---|---|
| `MMCFM_TEST_ONLY=a,b` | Runs only these self-test areas. |
| `MMCFM_KEEP_TEST=1` | Keeps the self-test temp folder. |
| `MMCFM_TRAY_DEMO=1` | (Windows) Minimizes to the tray after 2 s and clicks the icon after 4 s, logging each step. |
| `MMCFM_FIXTURES=DIR` | Folder with the RAR and WebM test fixtures (default `tests/fixtures`). |
| `MMCFM_NET_TEST=1` | Adds live network checks (searches on every keyless source, downloads, decoding). |
| `MMCFM_NETREQ_TEST=1` | Live `net_request` checks against httpbin.org. |
| `MMCFM_LOG_FILE=PATH` | Writes the log to a file (any run). |
| `MMCFM_LOG_STDOUT=1` | Prints the log to stdout (self test). |
| `MMCFM_INPUT_TRACE=1` | Logs where presses land. |
| `MMCFM_CORNERS=region` | Forces the Windows 7/8/10 rounded-corner path on Windows 11. |
| `MMCFM_VIDEO_BACKEND=soft\|os\|ffmpeg` | Forces one video backend. |
| `MMCFM_VIDEO_SAMPLES=DIR` | Self test plays every video in DIR. |
| `MMCFM_SOFT_BENCH=FILE\|URL[\|...]` | Built-in VP9 decoder speed. |
| `MMCFM_URL_TEST=URL` | Opens a video URL and decodes frames. |
| `MMCFM_PAIR_TEST=VIDEO\|AUDIO` | Plays separate video and audio streams as one. |
| `MMCFM_AUDIO_URL=URL[\|...]` | Plays online audio through the stream reader. |
| `MMCFM_NS_TEST=URL\|OUT\|PAUSE\|SEEK` | Dumps a stream read through FmNetStream. |
| `MMCFM_STREAM_PROBE=VIDEO\|AUDIO` | Time to open and first frame for each half. |
| `MMCFM_STREAM_TEST=360,720,...` | Player switches through these qualities. |
| `MMCFM_STREAM_LOG=FILE` | Appends the stream test's measurements. |
| `MMCFM_YT_TEST=ID\|LINK` | Live built-in YouTube resolve and play. |
| `MMCFM_ASRC_YT=QUERY` | Live YouTube audio search and play. |
| `MMCFM_DM_TEST=ID[\|ID]`, `MMCFM_DM_HEIGHT=N` | Live Dailymotion test, max height. |
| `MMCFM_DM_HOLD_MF=1` | Holds a Media Foundation reference while debugging the DM test. |
| `MMCFM_HLS_TEST=M3U8[\|MB]` | Reads an HLS stream. |
| `MMCFM_GENERIC_TEST=URL`, `MMCFM_GENERIC_FFMPEG=1` | Live "Any site" extraction (optionally as if FFmpeg were present). |
| `MMCFM_BILI_TEST=QUERY`, `MMCFM_BILI_DL=DIR` | Live Bilibili search/play and download. |
| `MMCFM_AONLINE_PLAY=src:query\|...` | Plays the first item of each online-audio search. |
| `MMCFM_AAC_PROBE=1` | AAC radio through the video decoder fallback. |
| `MMCFM_QUEUE_PLAY=1` | Play queue on a real sound device. |
| `MMCFM_YTDLP`, `MMCFM_JS`, `MMCFM_JS_RUNTIME`, `MMCFM_FFMPEG`, `MMCFM_FFMPEG_DIR` | yt-dlp, JS runtime and ffmpeg paths for tests and `--demo-online`. |
| `MMCFM_INSTALL_TEST=1` | Tests "Get yt-dlp" (writes into the real config folder). |
| `MMCFM_YT_KEY`, `MMCFM_PEXELS_KEY`, `MMCFM_UNSPLASH_KEY`, `MMCFM_PIXABAY_KEY`, `MMCFM_JAMENDO_KEY`, `MMCFM_FREESOUND_KEY` | API keys for test and demo runs. |
| `MMCFM_DAV_TEST=url\|user\|pass`, `MMCFM_DAV_CHUNK=BYTES` | Live WebDAV test, forced chunk size. |
| `MMCFM_S3_TEST=endpoint\|region\|bucket\|key\|secret` | Live S3 test. |
| `MMCFM_MEGA_LINK=link[\|...]` | Opens and downloads MEGA public links. |
| `MMCFM_MEGA_LOGIN=email\|password`, `MMCFM_MEGA_WRITE=1` | Live MEGA account test (with writes). |
| `MMCFM_AMC_TRACE=1` | Android: logs every MediaCodec extractor read. |

SDL's own `SDL_RENDER_DRIVER` is honoured too. On Windows the default is Direct3D 11.

---

## Configuration

Settings are plain `key=value` text in `mmcfm.ini`. They are saved about 0.8 s after a change and at exit.

| OS | Config folder | Cache folder |
|---|---|---|
| Windows | `%APPDATA%\mmcfm` | `%LOCALAPPDATA%\mmcfm\cache` |
| Linux, FreeBSD | `$XDG_CONFIG_HOME/mmcfm` (`~/.config/mmcfm`) | `$XDG_CACHE_HOME/mmcfm` (`~/.cache/mmcfm`) |
| macOS | `~/Library/Application Support/mmcfm` | `~/Library/Caches/mmcfm` |
| Android | the app's private `config` folder | the app's private `cache` folder |

Files in the config folder:

| File | Holds |
|---|---|
| `mmcfm.ini` | Settings, bookmarks, history, window position. |
| `library.txt` | Media library folders, favourites, recents. |
| `audio-library.txt`, `audio-recent.txt` | Online audio favourites and recents. |
| `audio-queue.txt`, `audio-playlists.txt` | Play queue and playlists. |
| `photo-albums.txt` | Online photo albums and favourites. |
| `cloud.txt` | Cloud accounts. Secrets are DPAPI-encrypted on Windows and obfuscated elsewhere. |

The cache holds disk thumbnails, the library tag cache (`library-index.txt`) and the `--perf` test folder. All of it can be deleted.

**Keys and client ids** (all optional, entered in Settings):

| Setting | Used for |
|---|---|
| YouTube Data API v3 key (`yt_api_key`) | Official YouTube search with safe search. Search works without it. |
| Pexels, Unsplash, Pixabay keys | Those photo sources. |
| Jamendo client id, Freesound key | Those audio sources. |
| yt-dlp path, JS runtime (`node:<path>` / `deno:<path>`), ffmpeg folder | Optional yt-dlp fallback and merging downloads. They are found automatically when empty. |
| Download folder, preferred height (360/480/720/1080), safe search | Online videos. |
| Google Drive, Dropbox, OneDrive client id (+ Google client secret) | Entered per account in the cloud sign-in form. See [Cloud storage](#cloud-storage). |

---

## Project layout

```
src/            flat module folder ("the Lua way"): f*.c modules, x*.c wrappers
vendor/         third-party code, compiled as shipped (patches in vendor/PATCHES.md)
assets/fonts/   Poppins, embedded into the binary at build time
android/        AndroidManifest.xml, res/, Java (FmActivity, FmFileProvider)
web/            index.html shell for Emscripten
tests/fixtures/ RAR archives from libarchive's tests, a small VP9+Opus WebM
tools/          gen_themes.py (generates src/ftheme_data.h)
build.sh        the one build script
```

| Prefix | Role |
|---|---|
| `fmain`, `fapp`, `fpanel`, `flayout`, `fops`, `fvfs`, `fconf` | Entry point, the file manager, panels, layout, jobs, locations, settings |
| `fcore`, `fsdl`, `fplat_*`, `fproc`, `fwin.h` | Core types/memory/strings, SDL loader (`fsdl_list.h` lists every SDL call), OS layer, child processes |
| `fgfx`, `ffont`, `ficon`, `fui`, `ftheme`, `ftitle`, `ftray` | Drawing, text, icons, widgets, themes, title bar, tray |
| `farc`, `fzip`, `f7z`, `frar`, `ftar`, `fsingle`, `fstream`, `fcrypt`, `fbignum`, `fasm.h` | Archives (`fsingle`: single .gz/.bz2/.xz/.zst/.lzma files), compression streams, cryptography, asm fast paths |
| `fview*`, `fdec_*`, `fthumb`, `fviz`, `feq` | Viewers, image/audio/video decoders and backends, thumbnails, visualizer, equalizer |
| `flib*`, `fqueue*` | Media library, play queue and playlists |
| `fnet`, `fnetstream`, `fhls`, `fjson`, `fxml`, `foauth` | HTTP(S), streaming reader, HLS, JSON/XML parsers, OAuth |
| `fvsrc*`, `fonline*` | Online video sources and screen |
| `fpsrc*`, `fphoto*` | Online photo sources and screen |
| `fasrc*`, `faudio_online*` | Online audio sources and screen |
| `fcloud*` | Cloud adapters, accounts, VFS, jobs, UI |
| `fperf`, `ftest*` | `--perf` tour, self tests |
| `x*.c` | Compile single-header libraries from `vendor/` (stb, dr_libs, miniz, pl_mpeg, nanosvg) |

See `CONTRIBUTING.md` for code style and portability rules.

---

## Third-party code

| Component | Licence |
|---|---|
| SDL 2.32.10 (and its Android Java glue) | zlib |
| miniz 3.0.2 | MIT |
| bzip2 1.0.8 | bzip2 licence (BSD-style) |
| LZMA SDK (C part of 7-Zip 24.09) | public domain |
| zstd 1.5.6 | BSD-3-Clause |
| stb_image, stb_truetype, stb_vorbis | MIT or public domain |
| dr_mp3, dr_flac, dr_wav | MIT-0 or public domain |
| pl_mpeg | MIT |
| nanosvg, nanosvgrast | zlib |
| nestegg (git 767aab2) | ISC |
| libvpx 1.17.0 (VP9 decoder only) | BSD-3-Clause + WebM patent grant |
| libopus 1.6.1 (decoder only) | BSD-3-Clause |
| Poppins font | SIL OFL 1.1 |
| RAR decoder in `src/frar.c` | derived from libarchive, BSD-2-Clause |

FFmpeg is not shipped unless you add it with `--with-ffmpeg`, and it keeps its own (L)GPL licence. See `LICENSES.md` and `vendor/PATCHES.md` for details.

---

## Known limits

- **Video**:
  - Without FFmpeg, formats depend on the OS decoders. On Linux, macOS and BSD without FFmpeg only MPEG-1 and WebM (VP9 + Opus) play.
  - The built-in VP9 decoder takes 8-bit 4:2:0 only, seeks slowly, and may not keep up at 720p+ when streaming.
  - Not built in: AV1, H.264 and AAC.
  - FFmpeg 9 is not declared yet.
  - Rotation metadata (portrait phone clips) is not applied.
  - Media Foundation cannot play fragmented H.264 DASH.
- **Online**:
  - Sites that need a Referer header do not stream.
  - HLS SAMPLE-AES is not played.
  - Bilibili is limited to 720p when not signed in, and plays only the first part of multi-part videos.
  - Wikimedia may refuse (HTTP 429) the current User-Agent.
  - Google Photos is not available.
- **Cloud**:
  - No thumbnails.
  - Only new folders can be created in the cloud (no new empty files).
  - The cloud cache is never trimmed.
  - Interrupted transfers are not resumed.
  - MEGA has no 2FA, no incoming shares and no `#P!` links.
  - Google/Dropbox/OneDrive need your own client id.
- **RAR**: read only. No encrypted entries, only the first volume, no RAR 1.5/2.x compression.
- **UI**:
  - The window cannot be dragged while a viewer is open (custom title bar).
  - Rounded corners are Windows only.
  - The tray icon is Windows only.
  - Android has no system picture-in-picture.
- **Platforms**:
  - Linux, macOS and FreeBSD binaries are build-verified but have not been run on those systems.
  - The web build is a demo on an empty in-memory file system.
