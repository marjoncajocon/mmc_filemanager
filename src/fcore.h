/* fcore.h -- base types, platform detection, memory, strings and paths.
**
** Every source file includes this first. It has no dependency on SDL, so
** worker code (archives, decoders) can use it on any thread.
**
** Design decisions:
**   - Plain C11 that tcc 0.9.27 also accepts: no _Atomic, no <threads.h>,
**     no VLAs in headers, no statement expressions.
**   - Paths are UTF-8 everywhere with '/' or the native separator; only
**     fplat_*.c converts to the OS encoding (UTF-16 on Windows).
**   - Allocations go through fm_alloc/fm_free so the memory counter in the
**     status bar is exact, and arenas hold listings so a whole directory is
**     freed in one call.
*/
#ifndef FCORE_H
#define FCORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#if defined(__TINYC__)
/* tcc's x86-64 math.h implements these with x87 asm constraints it cannot
** compile; math.h is already parsed above, so the macros only affect us. */
#  define fabsf(x) ((float)fabs((double)(x)))
#  define fabsl(x) fabs((double)(x))
/* 32-bit msvcrt.dll has no float variants of the math functions. */
#  define sqrtf(x) ((float)sqrt((double)(x)))
#  define sinf(x) ((float)sin((double)(x)))
#  define cosf(x) ((float)cos((double)(x)))
#  define tanf(x) ((float)tan((double)(x)))
#  define atan2f(y, x) ((float)atan2((double)(y), (double)(x)))
#  define floorf(x) ((float)floor((double)(x)))
#  define ceilf(x) ((float)ceil((double)(x)))
#  define expf(x) ((float)exp((double)(x)))
#  define logf(x) ((float)log((double)(x)))
#  define log10f(x) ((float)log10((double)(x)))
#  define powf(x, y) ((float)pow((double)(x), (double)(y)))
#  define fmodf(x, y) ((float)fmod((double)(x), (double)(y)))
#endif

/* ---- platform ----------------------------------------------------------- */

#if defined(_WIN32)
#  define FM_WIN 1
#elif defined(__ANDROID__)
#  define FM_ANDROID 1
#  define FM_POSIX 1
#elif defined(__EMSCRIPTEN__)
#  define FM_WEB 1
#  define FM_POSIX 1
#elif defined(__APPLE__)
#  include <TargetConditionals.h>
#  if TARGET_OS_IPHONE
#    define FM_IOS 1
#  else
#    define FM_MACOS 1
#  endif
#  define FM_POSIX 1
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
#  define FM_BSD 1
#  define FM_POSIX 1
#else
#  define FM_LINUX 1
#  define FM_POSIX 1
#endif

/* Touch-first platforms start with the touch layout (bigger rows). */
#if defined(FM_ANDROID) || defined(FM_IOS)
#  define FM_MOBILE 1
#endif

#if defined(__x86_64__) || defined(_M_X64)
#  define FM_X64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define FM_ARM64 1
#endif

/* Inline assembly fast paths (fasm.h): GCC-style asm on x64/arm64 only. */
#if !defined(FM_NO_ASM) && !defined(__TINYC__) && !defined(__EMSCRIPTEN__) && \
    (defined(__GNUC__) || defined(__clang__)) && (defined(FM_X64) || defined(FM_ARM64))
#  define FM_ASM 1
#endif

#ifndef FM_VERSION
#  define FM_VERSION "0.1.0"
#endif

#ifdef FM_WIN
#  define FM_SEP '\\'
#  define FM_SEP_STR "\\"
#else
#  define FM_SEP '/'
#  define FM_SEP_STR "/"
#endif

#define FM_PATH_MAX 1024

/* ---- types -------------------------------------------------------------- */

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef float    f32;
typedef double   f64;

#define FM_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define FM_MIN(a, b) ((a) < (b) ? (a) : (b))
#define FM_MAX(a, b) ((a) > (b) ? (a) : (b))
#define FM_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define FM_UNUSED(x) ((void)(x))

#if defined(__GNUC__) || defined(__clang__)
#  define FM_LIKELY(x) __builtin_expect(!!(x), 1)
#  define FM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define FM_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#  define FM_LIKELY(x) (x)
#  define FM_UNLIKELY(x) (x)
#  define FM_PRINTF(a, b)
#endif

/* ---- errors ------------------------------------------------------------- */

typedef enum FmErr {
  FM_OK = 0,
  FM_ERR_IO,          /* read/write/open failed */
  FM_ERR_NOT_FOUND,
  FM_ERR_EXISTS,
  FM_ERR_ACCESS,      /* permission denied */
  FM_ERR_FORMAT,      /* corrupt or unrecognised data */
  FM_ERR_UNSUPPORTED, /* valid but not handled (method, feature) */
  FM_ERR_PASSWORD,    /* password needed or wrong */
  FM_ERR_CRC,         /* checksum mismatch */
  FM_ERR_NOMEM,
  FM_ERR_CANCEL,      /* user cancelled */
  FM_ERR_FULL,        /* disk full */
  FM_ERR_COUNT
} FmErr;

const char *fm_err_str(FmErr e);

/* ---- logging ------------------------------------------------------------ */

/* Goes to SDL_Log once SDL is up (logcat on Android), stderr before. */
void fm_log(const char *fmt, ...) FM_PRINTF(1, 2);

/* ---- memory ------------------------------------------------------------- */

void *fm_alloc(size_t n);            /* never returns NULL: aborts on OOM */
void *fm_calloc(size_t n, size_t size);
void *fm_realloc(void *p, size_t n);
void  fm_free(void *p);
char *fm_strdup(const char *s);
char *fm_strndup(const char *s, size_t n);
size_t fm_mem_in_use(void);          /* bytes currently held via fm_alloc */

/* Growable array of fixed-size items: `T *items; int count, cap;`. */
#define FM_VEC_PUSH(v, item) do {                                           \
    if ((v).count == (v).cap) {                                             \
      (v).cap = (v).cap ? (v).cap * 2 : 16;                                 \
      (v).items = fm_realloc((v).items, (size_t)(v).cap * sizeof(*(v).items)); \
    }                                                                       \
    (v).items[(v).count++] = (item);                                        \
  } while (0)
#define FM_VEC_FREE(v) do { fm_free((v).items); (v).items = NULL; (v).count = (v).cap = 0; } while (0)

/* Arena: bump allocator in chained blocks; freed all at once. */
typedef struct FmArenaBlock FmArenaBlock;
typedef struct FmArena {
  FmArenaBlock *head;
  size_t block_size;   /* default 64 KB */
  size_t total;        /* bytes reserved */
} FmArena;

void  arena_init(FmArena *a, size_t block_size);
void *arena_alloc(FmArena *a, size_t n);          /* 8-byte aligned, zeroed */
char *arena_strdup(FmArena *a, const char *s);
char *arena_strndup(FmArena *a, const char *s, size_t n);
void  arena_reset(FmArena *a);                     /* keeps the first block */
void  arena_free(FmArena *a);

/* ---- strings ------------------------------------------------------------ */

size_t fm_strlcpy(char *dst, const char *src, size_t cap);
size_t fm_strlcat(char *dst, const char *src, size_t cap);
int    fm_snprintf(char *dst, size_t cap, const char *fmt, ...) FM_PRINTF(3, 4);
int    fm_stricmp(const char *a, const char *b);
int    fm_strnicmp(const char *a, const char *b, size_t n);
bool   fm_ends_with_i(const char *s, const char *suffix);
const char *fm_stristr(const char *hay, const char *needle);
/* Natural order compare: "file2" < "file10", case-insensitive. */
int    fm_natcmp(const char *a, const char *b);

/* UTF-8: decode one code point, returns bytes used (>=1); bad bytes give U+FFFD. */
int  utf8_decode(const char *s, u32 *cp);
int  utf8_encode(u32 cp, char out[4]);
int  utf8_prev(const char *s, int pos);   /* byte index of previous code point */
int  utf8_len(const char *s);             /* code points */

/* Human readable size: "1.4 MB". buf >= 16 bytes. */
const char *fm_fmt_size(u64 bytes, char *buf, size_t cap);
/* "2026-10-08 14:03" from unix seconds, local time. */
const char *fm_fmt_time(i64 unix_sec, char *buf, size_t cap);

/* ---- paths -------------------------------------------------------------- */

bool fm_is_sep(char c);
/* Joins a and b with one separator; returns false if it does not fit. */
bool fm_path_join(char *out, size_t cap, const char *a, const char *b);
const char *fm_path_base(const char *path);       /* after the last separator */
const char *fm_path_ext(const char *path);        /* ".txt" or "" */
/* Parent directory in place; returns false at a root ("/", "C:\"). */
bool fm_path_parent(char *path);
/* Collapses "a//b", "./", "../" and trailing separators. */
void fm_path_normalize(char *path);
bool fm_path_is_root(const char *path);
/* True when child is inside dir (or equal). Used to stop copying a folder into itself. */
bool fm_path_is_inside(const char *child, const char *dir);
/* Rejects archive entry names that would escape the target (zip slip). */
bool fm_path_is_safe_relative(const char *rel);

/* ---- file types --------------------------------------------------------- */

typedef enum FmType {
  FT_FILE = 0, FT_DIR, FT_UP, FT_IMAGE, FT_GIF, FT_SVG, FT_AUDIO, FT_VIDEO,
  FT_ARCHIVE, FT_TEXT, FT_CODE, FT_PDF, FT_DOC, FT_SHEET, FT_SLIDE, FT_APK,
  FT_EXE, FT_FONT, FT_DISK, FT_COUNT
} FmType;

FmType fm_type_from_name(const char *name);
const char *fm_type_label(FmType t);

#endif
