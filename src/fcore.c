/* fcore.c -- memory, strings, UTF-8, paths and file types.
**
** Design decisions:
**   - fm_alloc keeps a size header so fm_mem_in_use is exact without asking
**     the OS; the counter uses SDL-free atomics (a plain counter guarded by
**     the compiler builtins when present) so workers may allocate too.
**   - Path helpers accept both '/' and '\\' on Windows, only '/' elsewhere.
*/
#include "fcore.h"
#include <ctype.h>
#include <time.h>

/* ---- errors ------------------------------------------------------------- */

const char *fm_err_str(FmErr e) {
  static const char *const s[FM_ERR_COUNT] = {
    "OK", "I/O error", "Not found", "Already exists", "Permission denied",
    "Corrupt or unknown format", "Not supported", "Wrong or missing password",
    "Checksum mismatch", "Out of memory", "Cancelled", "Disk full",
  };
  return (unsigned)e < FM_ERR_COUNT ? s[e] : "Error";
}

/* ---- memory ------------------------------------------------------------- */

static volatile size_t g_mem_in_use;

static void mem_add(size_t n, int sign) {
#if defined(__GNUC__) || defined(__clang__)
  if (sign > 0) __atomic_add_fetch(&g_mem_in_use, n, __ATOMIC_RELAXED);
  else __atomic_sub_fetch(&g_mem_in_use, n, __ATOMIC_RELAXED);
#else
  /* tcc: a racy counter is fine, it is only shown to the user. */
  if (sign > 0) g_mem_in_use += n; else g_mem_in_use -= n;
#endif
}

#define HDR 16   /* keeps 16-byte alignment of the returned pointer */

static void oom(size_t n) {
  fprintf(stderr, "mmcfm: out of memory (%lu bytes)\n", (unsigned long)n);
  abort();
}

void *fm_alloc(size_t n) {
  u8 *p = (u8 *)malloc(n + HDR);
  if (!p) oom(n);
  *(size_t *)p = n;
  mem_add(n, 1);
  return p + HDR;
}

void *fm_calloc(size_t n, size_t size) {
  size_t t = n * size;
  if (size && t / size != n) oom(t);
  void *p = fm_alloc(t);
  memset(p, 0, t);
  return p;
}

void *fm_realloc(void *p, size_t n) {
  if (!p) return fm_alloc(n);
  u8 *b = (u8 *)p - HDR;
  size_t old = *(size_t *)b;
  u8 *nb = (u8 *)realloc(b, n + HDR);
  if (!nb) oom(n);
  *(size_t *)nb = n;
  mem_add(old, -1);
  mem_add(n, 1);
  return nb + HDR;
}

void fm_free(void *p) {
  if (!p) return;
  u8 *b = (u8 *)p - HDR;
  mem_add(*(size_t *)b, -1);
  free(b);
}

char *fm_strdup(const char *s) { return fm_strndup(s, strlen(s)); }

char *fm_strndup(const char *s, size_t n) {
  size_t l = 0;
  while (l < n && s[l]) l++;
  char *d = (char *)fm_alloc(l + 1);
  memcpy(d, s, l);
  d[l] = 0;
  return d;
}

size_t fm_mem_in_use(void) { return g_mem_in_use; }

/* ---- arena -------------------------------------------------------------- */

struct FmArenaBlock {
  FmArenaBlock *next;
  size_t used, cap;
  /* data follows, 16-aligned */
};
#define BLOCK_HDR ((sizeof(FmArenaBlock) + 15) & ~(size_t)15)

void arena_init(FmArena *a, size_t block_size) {
  a->head = NULL;
  a->block_size = block_size ? block_size : 64 * 1024;
  a->total = 0;
}

void *arena_alloc(FmArena *a, size_t n) {
  n = (n + 7) & ~(size_t)7;
  FmArenaBlock *b = a->head;
  if (!b || b->used + n > b->cap) {
    size_t cap = n > a->block_size ? n : a->block_size;
    b = (FmArenaBlock *)fm_alloc(BLOCK_HDR + cap);
    b->next = a->head;
    b->used = 0;
    b->cap = cap;
    a->head = b;
    a->total += cap;
  }
  u8 *p = (u8 *)b + BLOCK_HDR + b->used;
  b->used += n;
  memset(p, 0, n);
  return p;
}

char *arena_strdup(FmArena *a, const char *s) { return arena_strndup(a, s, strlen(s)); }

char *arena_strndup(FmArena *a, const char *s, size_t n) {
  size_t l = 0;
  while (l < n && s[l]) l++;
  char *d = (char *)arena_alloc(a, l + 1);
  memcpy(d, s, l);
  return d;
}

void arena_reset(FmArena *a) {
  FmArenaBlock *b = a->head, *keep = NULL;
  while (b) {
    FmArenaBlock *n = b->next;
    if (!n) keep = b; else { a->total -= b->cap; fm_free(b); }
    b = n;
  }
  a->head = keep;
  if (keep) keep->used = 0;
}

void arena_free(FmArena *a) {
  FmArenaBlock *b = a->head;
  while (b) { FmArenaBlock *n = b->next; fm_free(b); b = n; }
  a->head = NULL;
  a->total = 0;
}

/* ---- strings ------------------------------------------------------------ */

size_t fm_strlcpy(char *dst, const char *src, size_t cap) {
  size_t n = strlen(src);
  if (cap) {
    size_t c = n < cap - 1 ? n : cap - 1;
    memcpy(dst, src, c);
    dst[c] = 0;
  }
  return n;
}

size_t fm_strlcat(char *dst, const char *src, size_t cap) {
  size_t d = 0;
  while (d < cap && dst[d]) d++;
  if (d == cap) return d + strlen(src);
  return d + fm_strlcpy(dst + d, src, cap - d);
}

int fm_snprintf(char *dst, size_t cap, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(dst, cap, fmt, ap);
  va_end(ap);
  if (cap) dst[cap - 1] = 0;
  return n;
}

int fm_stricmp(const char *a, const char *b) {
  for (;; a++, b++) {
    int ca = tolower((u8)*a), cb = tolower((u8)*b);
    if (ca != cb || !ca) return ca - cb;
  }
}

int fm_strnicmp(const char *a, const char *b, size_t n) {
  for (; n; n--, a++, b++) {
    int ca = tolower((u8)*a), cb = tolower((u8)*b);
    if (ca != cb || !ca) return ca - cb;
  }
  return 0;
}

bool fm_ends_with_i(const char *s, const char *suffix) {
  size_t a = strlen(s), b = strlen(suffix);
  return a >= b && fm_stricmp(s + a - b, suffix) == 0;
}

const char *fm_stristr(const char *hay, const char *needle) {
  size_t n = strlen(needle);
  if (!n) return hay;
  for (; *hay; hay++)
    if (fm_strnicmp(hay, needle, n) == 0) return hay;
  return NULL;
}

int fm_natcmp(const char *a, const char *b) {
  while (*a && *b) {
    if (isdigit((u8)*a) && isdigit((u8)*b)) {
      while (*a == '0') a++;
      while (*b == '0') b++;
      const char *sa = a, *sb = b;
      while (isdigit((u8)*a)) a++;
      while (isdigit((u8)*b)) b++;
      size_t la = (size_t)(a - sa), lb = (size_t)(b - sb);
      if (la != lb) return la < lb ? -1 : 1;
      int c = memcmp(sa, sb, la);
      if (c) return c;
    } else {
      int ca = tolower((u8)*a), cb = tolower((u8)*b);
      if (ca != cb) return ca - cb;
      a++; b++;
    }
  }
  return (u8)*a - (u8)*b;
}

/* ---- utf-8 -------------------------------------------------------------- */

int utf8_decode(const char *str, u32 *cp) {
  const u8 *s = (const u8 *)str;
  u32 c = s[0];
  if (c < 0x80) { *cp = c; return 1; }
  if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
    *cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
    if (*cp >= 0x80) return 2;
  } else if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
    *cp = ((c & 0x0F) << 12) | ((u32)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    if (*cp >= 0x800) return 3;
  } else if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
             (s[3] & 0xC0) == 0x80) {
    *cp = ((c & 0x07) << 18) | ((u32)(s[1] & 0x3F) << 12) | ((u32)(s[2] & 0x3F) << 6) |
          (s[3] & 0x3F);
    if (*cp >= 0x10000 && *cp <= 0x10FFFF) return 4;
  }
  *cp = 0xFFFD;
  return 1;
}

int utf8_encode(u32 cp, char out[4]) {
  if (cp < 0x80) { out[0] = (char)cp; return 1; }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

int utf8_prev(const char *s, int pos) {
  if (pos <= 0) return 0;
  pos--;
  while (pos > 0 && ((u8)s[pos] & 0xC0) == 0x80) pos--;
  return pos;
}

int utf8_len(const char *s) {
  int n = 0;
  u32 cp;
  while (*s) { s += utf8_decode(s, &cp); n++; }
  return n;
}

const char *fm_fmt_size(u64 bytes, char *buf, size_t cap) {
  static const char *const unit[] = { "B", "KB", "MB", "GB", "TB", "PB" };
  if (bytes < 1024) { fm_snprintf(buf, cap, "%u B", (unsigned)bytes); return buf; }
  double v = (double)bytes;
  int u = 0;
  while (v >= 1024.0 && u < 5) { v /= 1024.0; u++; }
  fm_snprintf(buf, cap, v < 10 ? "%.2f %s" : v < 100 ? "%.1f %s" : "%.0f %s", v, unit[u]);
  return buf;
}

const char *fm_fmt_time(i64 unix_sec, char *buf, size_t cap) {
  time_t t = (time_t)unix_sec;
  struct tm *tm = localtime(&t);
  if (!tm) { fm_strlcpy(buf, "-", cap); return buf; }
  fm_snprintf(buf, cap, "%04d-%02d-%02d %02d:%02d", tm->tm_year + 1900, tm->tm_mon + 1,
              tm->tm_mday, tm->tm_hour, tm->tm_min);
  return buf;
}

/* ---- paths -------------------------------------------------------------- */

bool fm_is_sep(char c) {
#ifdef FM_WIN
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

bool fm_path_join(char *out, size_t cap, const char *a, const char *b) {
  char tmp[FM_PATH_MAX];
  size_t la = strlen(a);
  while (*b && fm_is_sep(*b)) b++;
  int n;
  if (la == 0) n = fm_snprintf(tmp, sizeof tmp, "%s", b);
  else if (fm_is_sep(a[la - 1])) n = fm_snprintf(tmp, sizeof tmp, "%s%s", a, b);
  else n = fm_snprintf(tmp, sizeof tmp, "%s%c%s", a, FM_SEP, b);
  if (n < 0 || (size_t)n >= cap || (size_t)n >= sizeof tmp) return false;
  memcpy(out, tmp, (size_t)n + 1);
  return true;
}

const char *fm_path_base(const char *path) {
  const char *b = path;
  for (const char *p = path; *p; p++)
    if (fm_is_sep(*p) && p[1]) b = p + 1;
  return b;
}

const char *fm_path_ext(const char *path) {
  const char *b = fm_path_base(path);
  const char *dot = strrchr(b, '.');
  return (dot && dot != b) ? dot : b + strlen(b);
}

bool fm_path_is_root(const char *p) {
  if (!p[0]) return true;
#ifdef FM_WIN
  if (p[0] && p[1] == ':' && (!p[2] || (fm_is_sep(p[2]) && !p[3]))) return true;
  if (fm_is_sep(p[0]) && fm_is_sep(p[1])) {   /* \\server\share */
    const char *s = p + 2;
    int seps = 0;
    for (; *s; s++) if (fm_is_sep(*s) && s[1]) seps++;
    return seps <= 1;
  }
#endif
  return fm_is_sep(p[0]) && !p[1];
}

bool fm_path_parent(char *path) {
  size_t n = strlen(path);
  while (n > 1 && fm_is_sep(path[n - 1])) path[--n] = 0;
  if (fm_path_is_root(path)) return false;
  while (n > 0 && !fm_is_sep(path[n - 1])) n--;
  if (n == 0) return false;
  path[n] = 0;
  if (!fm_path_is_root(path)) {
    while (n > 1 && fm_is_sep(path[n - 1])) path[--n] = 0;
  }
#ifdef FM_WIN
  if (n == 2 && path[1] == ':') { path[2] = '\\'; path[3] = 0; }
#endif
  return true;
}

void fm_path_normalize(char *path) {
  char *segs[256];
  int nseg = 0;
  char prefix[8] = { 0 };
  char *p = path;
#ifdef FM_WIN
  for (char *q = path; *q; q++) if (*q == '/') *q = '\\';
  if (p[0] && p[1] == ':') { prefix[0] = p[0]; prefix[1] = ':'; prefix[2] = '\\'; p += 2; }
  else if (p[0] == '\\' && p[1] == '\\') { fm_strlcpy(prefix, "\\\\", sizeof prefix); p += 2; }
  else if (p[0] == '\\') { prefix[0] = '\\'; }
#else
  if (p[0] == '/') prefix[0] = '/';
#endif
  char buf[FM_PATH_MAX];
  fm_strlcpy(buf, p, sizeof buf);
  char *s = buf;
  while (*s) {
    while (*s && fm_is_sep(*s)) *s++ = 0;
    if (!*s) break;
    char *seg = s;
    while (*s && !fm_is_sep(*s)) s++;
    if (*s) *s++ = 0;
    if (strcmp(seg, ".") == 0) continue;
    if (strcmp(seg, "..") == 0) { if (nseg > 0 && strcmp(segs[nseg - 1], "..") != 0) { nseg--; continue; } if (prefix[0]) continue; }
    if (nseg < 256) segs[nseg++] = seg;
  }
  char out[FM_PATH_MAX];
  fm_strlcpy(out, prefix, sizeof out);
  for (int i = 0; i < nseg; i++) {
    if (i) fm_strlcat(out, FM_SEP_STR, sizeof out);
    fm_strlcat(out, segs[i], sizeof out);
  }
  if (!out[0]) fm_strlcpy(out, ".", sizeof out);
  fm_strlcpy(path, out, FM_PATH_MAX);
}

bool fm_path_is_inside(const char *child, const char *dir) {
  size_t n = strlen(dir);
  while (n > 0 && fm_is_sep(dir[n - 1])) n--;
#ifdef FM_WIN
  if (fm_strnicmp(child, dir, n) != 0) return false;
#else
  if (strncmp(child, dir, n) != 0) return false;
#endif
  return child[n] == 0 || fm_is_sep(child[n]);
}

bool fm_path_is_safe_relative(const char *rel) {
  if (!rel[0]) return false;
  if (rel[0] == '/' || rel[0] == '\\') return false;
  if (rel[1] == ':') return false;                 /* drive letter */
  const char *s = rel;
  while (*s) {
    const char *e = s;
    while (*e && *e != '/' && *e != '\\') e++;
    if (e - s == 2 && s[0] == '.' && s[1] == '.') return false;
    s = *e ? e + 1 : e;
  }
  return true;
}

/* ---- file types --------------------------------------------------------- */

typedef struct { const char *ext; FmType t; } ExtMap;

static const ExtMap kExt[] = {
  { "jpg", FT_IMAGE }, { "jpeg", FT_IMAGE }, { "png", FT_IMAGE }, { "bmp", FT_IMAGE },
  { "tga", FT_IMAGE }, { "psd", FT_IMAGE }, { "hdr", FT_IMAGE }, { "pic", FT_IMAGE },
  { "pnm", FT_IMAGE }, { "ppm", FT_IMAGE }, { "pgm", FT_IMAGE }, { "webp", FT_IMAGE },
  { "ico", FT_IMAGE }, { "heic", FT_IMAGE }, { "tif", FT_IMAGE }, { "tiff", FT_IMAGE },
  { "gif", FT_GIF }, { "svg", FT_SVG },
  { "mp3", FT_AUDIO }, { "flac", FT_AUDIO }, { "wav", FT_AUDIO }, { "ogg", FT_AUDIO },
  { "oga", FT_AUDIO }, { "m4a", FT_AUDIO }, { "aac", FT_AUDIO }, { "opus", FT_AUDIO },
  { "wma", FT_AUDIO }, { "aif", FT_AUDIO }, { "aiff", FT_AUDIO },
  { "mpg", FT_VIDEO }, { "mpeg", FT_VIDEO }, { "mp4", FT_VIDEO }, { "m4v", FT_VIDEO },
  { "mkv", FT_VIDEO }, { "webm", FT_VIDEO }, { "avi", FT_VIDEO }, { "mov", FT_VIDEO },
  { "wmv", FT_VIDEO }, { "flv", FT_VIDEO }, { "3gp", FT_VIDEO }, { "ts", FT_VIDEO },
  { "zip", FT_ARCHIVE }, { "7z", FT_ARCHIVE }, { "rar", FT_ARCHIVE }, { "tar", FT_ARCHIVE },
  { "gz", FT_ARCHIVE }, { "tgz", FT_ARCHIVE }, { "bz2", FT_ARCHIVE }, { "tbz", FT_ARCHIVE },
  { "tbz2", FT_ARCHIVE }, { "xz", FT_ARCHIVE }, { "txz", FT_ARCHIVE }, { "zst", FT_ARCHIVE },
  { "tzst", FT_ARCHIVE }, { "lzma", FT_ARCHIVE }, { "jar", FT_ARCHIVE }, { "cbz", FT_ARCHIVE },
  { "cbr", FT_ARCHIVE },
  { "txt", FT_TEXT }, { "md", FT_TEXT }, { "log", FT_TEXT }, { "ini", FT_TEXT },
  { "cfg", FT_TEXT }, { "conf", FT_TEXT }, { "csv", FT_TEXT }, { "nfo", FT_TEXT },
  { "c", FT_CODE }, { "h", FT_CODE }, { "cpp", FT_CODE }, { "hpp", FT_CODE },
  { "cc", FT_CODE }, { "java", FT_CODE }, { "kt", FT_CODE }, { "py", FT_CODE },
  { "js", FT_CODE }, { "ts", FT_CODE }, { "json", FT_CODE }, { "xml", FT_CODE },
  { "html", FT_CODE }, { "htm", FT_CODE }, { "css", FT_CODE }, { "sh", FT_CODE },
  { "bat", FT_CODE }, { "lua", FT_CODE }, { "rs", FT_CODE }, { "go", FT_CODE },
  { "zig", FT_CODE }, { "yml", FT_CODE }, { "yaml", FT_CODE }, { "toml", FT_CODE },
  { "dart", FT_CODE }, { "swift", FT_CODE }, { "cs", FT_CODE }, { "php", FT_CODE },
  { "rb", FT_CODE }, { "sql", FT_CODE }, { "s", FT_CODE }, { "asm", FT_CODE },
  { "pdf", FT_PDF },
  { "doc", FT_DOC }, { "docx", FT_DOC }, { "odt", FT_DOC }, { "rtf", FT_DOC },
  { "xls", FT_SHEET }, { "xlsx", FT_SHEET }, { "ods", FT_SHEET },
  { "ppt", FT_SLIDE }, { "pptx", FT_SLIDE }, { "odp", FT_SLIDE },
  { "apk", FT_APK }, { "aab", FT_APK }, { "xapk", FT_APK },
  { "exe", FT_EXE }, { "msi", FT_EXE }, { "dll", FT_EXE }, { "so", FT_EXE },
  { "dmg", FT_DISK }, { "iso", FT_DISK }, { "img", FT_DISK },
  { "ttf", FT_FONT }, { "otf", FT_FONT }, { "woff", FT_FONT }, { "woff2", FT_FONT },
};

FmType fm_type_from_name(const char *name) {
  const char *e = fm_path_ext(name);
  if (!*e) return FT_FILE;
  e++;
  for (int i = 0; i < FM_COUNT(kExt); i++)
    if (fm_stricmp(e, kExt[i].ext) == 0) {
      /* ".ts" is far more often TypeScript than MPEG transport stream. */
      return kExt[i].t;
    }
  return FT_FILE;
}

const char *fm_type_label(FmType t) {
  static const char *const s[FT_COUNT] = {
    "File", "Folder", "Up", "Image", "Animated image", "Vector image", "Audio", "Video",
    "Archive", "Text", "Source code", "PDF", "Document", "Spreadsheet", "Presentation",
    "Android package", "Program", "Font", "Disk image",
  };
  return (unsigned)t < FT_COUNT ? s[t] : "File";
}
