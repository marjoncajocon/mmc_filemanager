/* farc.c -- archives: detection, the FmArc object, extraction output and
** creation. The formats themselves live in fzip.c, ftar.c, fsingle.c,
** f7z.c and frar.c (see farc_int.h).
**
** Design decisions:
**   - Detection trusts magic bytes over names. Compressed data is told
**     apart from a compressed tar by the extension (.tar.gz, .tgz ...) or,
**     failing that, by decompressing the first 512 bytes and looking for a
**     tar header.
**   - Extraction output goes through one sink for every backend, so the
**     safety rules live in one place: names failing
**     fm_path_is_safe_relative are refused (the run ends with
**     FM_ERR_ACCESS after the other entries), Windows-illegal characters
**     become '_', and data goes to "<name>.mmcpart" which is renamed over
**     the target only when complete; errors and cancel delete it.
**   - Symlinks: on Windows they become small files holding the target. On
**     POSIX they are created after everything else and only when the
**     target stays inside the destination, so no later entry can be
**     written through a link and no link points outside.
**   - Folder mtimes and modes are applied at the end (writing files into a
**     folder changes its mtime). setuid/setgid/sticky bits are dropped.
**   - Selecting a folder selects everything below it: the selected folder
**     names go in a small hash set and each entry checks its parents.
**   - Entry names that are not UTF-8 are decoded once, here, for zip and
**     tar alike: as CP437, the zip default and what Windows tools write.
**   - arc_create writes "<out>.part" and renames it into place at the end;
**     an error or cancel deletes it.
*/
#include "farc_int.h"
#include "farc_ext.h"
#include "fstream.h"
#include "fcrypt.h"
#ifdef FM_POSIX
#  include <unistd.h>
#endif

/* ---- formats ------------------------------------------------------------ */

typedef struct FmtInfo { const char *name, *ext; } FmtInfo;

static const FmtInfo kFmt[ARC_COUNT] = {
  { "", "" },
  { "ZIP", ".zip" }, { "7Z", ".7z" }, { "RAR", ".rar" },
  { "TAR", ".tar" }, { "TAR.GZ", ".tar.gz" }, { "TAR.BZ2", ".tar.bz2" },
  { "TAR.XZ", ".tar.xz" }, { "TAR.ZST", ".tar.zst" },
  { "GZ", ".gz" }, { "BZ2", ".bz2" }, { "XZ", ".xz" }, { "ZST", ".zst" }, { "LZMA", ".lzma" },
};

const char *arc_fmt_name(FmArcFmt f) { return (f > 0 && f < ARC_COUNT) ? kFmt[f].name : "?"; }
const char *arc_fmt_ext(FmArcFmt f) { return (f > 0 && f < ARC_COUNT) ? kFmt[f].ext : ""; }
bool arc_fmt_can_create(FmArcFmt f) { return f > ARC_NONE && f < ARC_COUNT && f != ARC_RAR; }
bool arc_fmt_can_encrypt(FmArcFmt f) { return f == ARC_ZIP || f == ARC_7Z; }
bool arc_fmt_multi(FmArcFmt f) { return f >= ARC_ZIP && f <= ARC_TAR_ZST; }

typedef struct ExtMapA { const char *ext; FmArcFmt f; } ExtMapA;

/* Longest suffixes first so ".tar.gz" wins over ".gz". */
static const ExtMapA kExtA[] = {
  { ".tar.gz", ARC_TAR_GZ }, { ".tar.bz2", ARC_TAR_BZ2 }, { ".tar.xz", ARC_TAR_XZ },
  { ".tar.zst", ARC_TAR_ZST }, { ".tar.lzma", ARC_TAR }, { ".tgz", ARC_TAR_GZ },
  { ".taz", ARC_TAR_GZ }, { ".tbz2", ARC_TAR_BZ2 }, { ".tbz", ARC_TAR_BZ2 },
  { ".tb2", ARC_TAR_BZ2 }, { ".txz", ARC_TAR_XZ }, { ".tzst", ARC_TAR_ZST }, { ".tlz", ARC_TAR },
  { ".zip", ARC_ZIP }, { ".jar", ARC_ZIP }, { ".apk", ARC_ZIP }, { ".cbz", ARC_ZIP },
  { ".7z", ARC_7Z }, { ".cb7", ARC_7Z }, { ".rar", ARC_RAR }, { ".cbr", ARC_RAR },
  { ".tar", ARC_TAR }, { ".gz", ARC_GZ }, { ".bz2", ARC_BZ2 }, { ".xz", ARC_XZ },
  { ".zst", ARC_ZST }, { ".lzma", ARC_LZMA },
};

static FmArcFmt fmt_from_ext(const char *path) {
  for (int i = 0; i < FM_COUNT(kExtA); i++)
    if (fm_ends_with_i(path, kExtA[i].ext)) return kExtA[i].f;
  return ARC_NONE;
}

static u32 octal(const u8 *p, int n) {
  u32 v = 0;
  int i = 0;
  while (i < n && p[i] == ' ') i++;
  for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (u32)(p[i] - '0');
  return v;
}

/* A ustar magic, or an old-style header whose checksum adds up. */
static bool looks_like_tar(const u8 *h, size_t n) {
  if (n < 512) return false;
  if (memcmp(h + 257, "ustar", 5) == 0) return true;
  if (!h[0]) return false;
  u32 sum = 0;
  for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? 32u : h[i];
  return octal(h + 148, 8) == sum;
}

static FmArcFmt peek_compressed(FILE *f, const char *path, FmArcFmt single, FmArcFmt tar,
                                FmCodec codec) {
  FmArcFmt e = fmt_from_ext(path);
  if (e == tar) return tar;
  if (fm_fseek64(f, 0, SEEK_SET) != 0) return single;
  FmErr err;
  FmIn *in = in_open(f, codec, -1, &err);
  if (!in) return single;
  u8 *blk = (u8 *)fm_alloc(512);
  long n = in_read(in, blk, 512);
  bool is_tar = n == 512 && looks_like_tar(blk, 512);
  fm_free(blk);
  in_close(in);
  return is_tar ? tar : single;
}

FmArcFmt arc_detect(const char *path) {
  FmArcFmt e = fmt_from_ext(path);
  FILE *f = fm_fopen(path, "rb");
  if (!f) return e;
  u8 h[512];
  size_t n = fread(h, 1, sizeof h, f);
  FmArcFmt r = ARC_NONE;
  if (n >= 4 && h[0] == 'P' && h[1] == 'K' &&
      ((h[2] == 3 && h[3] == 4) || (h[2] == 5 && h[3] == 6) || (h[2] == 7 && h[3] == 8)))
    r = ARC_ZIP;
  else if (n >= 6 && memcmp(h, "7z\xBC\xAF\x27\x1C", 6) == 0)
    r = ARC_7Z;
  else if (n >= 7 && memcmp(h, "Rar!\x1A\x07", 6) == 0 && (h[6] == 0 || h[6] == 1))
    r = ARC_RAR;
  else if (n >= 262 && memcmp(h + 257, "ustar", 5) == 0)
    r = ARC_TAR;
  else if (n >= 3 && h[0] == 0x1F && h[1] == 0x8B && h[2] == 8)
    r = peek_compressed(f, path, ARC_GZ, ARC_TAR_GZ, CODEC_GZIP);
  else if (n >= 4 && h[0] == 'B' && h[1] == 'Z' && h[2] == 'h' && h[3] >= '1' && h[3] <= '9')
    r = peek_compressed(f, path, ARC_BZ2, ARC_TAR_BZ2, CODEC_BZIP2);
  else if (n >= 6 && memcmp(h, "\xFD" "7zXZ\0", 6) == 0)
    r = peek_compressed(f, path, ARC_XZ, ARC_TAR_XZ, CODEC_XZ);
  else if (n >= 4 && memcmp(h, "\x28\xB5\x2F\xFD", 4) == 0)
    r = peek_compressed(f, path, ARC_ZST, ARC_TAR_ZST, CODEC_ZSTD);
  else if (looks_like_tar(h, n))
    r = ARC_TAR;
  else if (e == ARC_LZMA || (e == ARC_TAR && fm_ends_with_i(path, "lzma")) ||
           (e == ARC_TAR && fm_ends_with_i(path, ".tlz")))
    r = (n >= 13 && h[0] < 225) ? e : ARC_NONE;        /* .lzma has no magic */
  else if (e == ARC_ZIP || e == ARC_7Z || e == ARC_RAR)
    r = e;                                             /* self-extractors, damaged heads */
  fclose(f);
  return r;
}

/* ---- names -------------------------------------------------------------- */

static const u16 kCp437[128] = {
  0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
  0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
  0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
  0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
  0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
  0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
  0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
  0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
  0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
  0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
  0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
  0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
  0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
  0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
  0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
  0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

bool arc_valid_utf8(const u8 *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    u8 c = s[i];
    if (c < 0x80) { i++; continue; }
    int k;
    u32 cp;
    if ((c & 0xE0) == 0xC0) { k = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { k = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { k = 3; cp = c & 0x07; }
    else return false;
    if (i + (size_t)k >= n) return false;
    for (int j = 1; j <= k; j++) {
      if ((s[i + j] & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (s[i + j] & 0x3F);
    }
    if ((k == 1 && cp < 0x80) || (k == 2 && cp < 0x800) || (k == 3 && cp < 0x10000) ||
        cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
      return false;
    i += (size_t)k + 1;
  }
  return true;
}

void arc_name_utf8(const u8 *raw, size_t n, bool utf8_flag, char *out) {
  size_t o = 0;
  if (utf8_flag || arc_valid_utf8(raw, n)) {
    memcpy(out, raw, n);
    o = n;
  } else {
    for (size_t i = 0; i < n; i++) {
      if (raw[i] < 0x80) out[o++] = (char)raw[i];
      else o += (size_t)utf8_encode(kCp437[raw[i] - 0x80], out + o);
    }
  }
  out[o] = 0;
}

/* ---- the archive object -------------------------------------------------- */

static const FmArcBackend *backend_for(FmArcFmt f) {
  switch (f) {
    case ARC_ZIP: return &g_arc_zip;
    case ARC_7Z: return &g_arc_7z;
    case ARC_RAR: return &g_arc_rar;
    case ARC_TAR: case ARC_TAR_GZ: case ARC_TAR_BZ2: case ARC_TAR_XZ: case ARC_TAR_ZST:
      return &g_arc_tar;
    case ARC_GZ: case ARC_BZ2: case ARC_XZ: case ARC_ZST: case ARC_LZMA:
      return &g_arc_single;
    default: return NULL;
  }
}

FmErr arc_open(const char *path, const FmArcCb *cb, FmArc **out) {
  *out = NULL;
  if (!plat_exists(path)) return FM_ERR_NOT_FOUND;
  FmArcFmt fmt = arc_detect(path);
  if (fmt == ARC_NONE) return FM_ERR_FORMAT;
  const FmArcBackend *be = backend_for(fmt);
  if (!be || !be->open) return FM_ERR_UNSUPPORTED;
  FmArc *a = (FmArc *)fm_calloc(1, sizeof *a);
  a->fmt = fmt;
  a->be = be;
  fm_strlcpy(a->path, path, sizeof a->path);
  arena_init(&a->arena, 0);
  if (cb) a->cb = *cb;
  a->f = fm_fopen(path, "rb");
  if (!a->f) {
    fm_free(a);
    return FM_ERR_IO;
  }
  a->file_size = fm_fsize(a->f);
  FmErr e = be->open(a);
  if (e) {
    arc_close(a);
    return e;
  }
  *out = a;
  return FM_OK;
}

void arc_close(FmArc *a) {
  if (!a) return;
  if (a->be && a->be->close) a->be->close(a);
  if (a->f) fclose(a->f);
  arena_free(&a->arena);
  fm_free(a->e);
  wipe(a->password, sizeof a->password);
  fm_free(a);
}

FmArcFmt arc_format(const FmArc *a) { return a ? a->fmt : ARC_NONE; }
int arc_count(const FmArc *a) { return a ? a->n : 0; }
const FmArcEntry *arc_entry(const FmArc *a, int i) {
  return (a && i >= 0 && i < a->n) ? &a->e[i] : NULL;
}
bool arc_has_encrypted(const FmArc *a) { return a && a->any_encrypted; }

u64 arc_total_size(const FmArc *a) {
  u64 t = 0;
  if (a)
    for (int i = 0; i < a->n; i++) t += a->e[i].size;
  return t;
}

/* ---- entries (backends) ------------------------------------------------- */

int arc_add(FmArc *a, const char *path, u64 size, u64 packed, i64 mtime, bool is_dir) {
  if (a->n == a->cap) {
    a->cap = a->cap ? a->cap * 2 : 64;
    a->e = (FmArcEntry *)fm_realloc(a->e, (size_t)a->cap * sizeof *a->e);
  }
  size_t len = strlen(path);
  char *p = (char *)arena_alloc(&a->arena, len + 1);
  size_t o = 0;
  for (size_t i = 0; i < len; i++) {
    char c = path[i] == '\\' ? '/' : path[i];
    if (c == '/' && (o == 0 || p[o - 1] == '/')) continue;      /* leading or doubled */
    if (c == '.' && (o == 0 || p[o - 1] == '/') &&
        (path[i + 1] == '/' || path[i + 1] == '\\')) {          /* "./" */
      i++;
      continue;
    }
    p[o++] = c;
  }
  while (o > 0 && p[o - 1] == '/') o--;
  p[o] = 0;
  FmArcEntry *e = &a->e[a->n];
  memset(e, 0, sizeof *e);
  e->path = p;
  e->size = is_dir ? 0 : size;
  e->packed = packed;
  e->mtime = mtime;
  e->is_dir = is_dir;
  return a->n++;
}

FmArcEntry *arc_at(FmArc *a, int i) { return (i >= 0 && i < a->n) ? &a->e[i] : NULL; }

FmErr arc_get_password(FmArc *a, bool retry) {
  if (retry) {
    wipe(a->password, sizeof a->password);
    a->have_password = false;
  }
  if (a->have_password) return FM_OK;
  if (!a->cb.password) return FM_ERR_PASSWORD;
  char buf[256];
  memset(buf, 0, sizeof buf);
  bool ok = a->cb.password(a->cb.ud, buf, (int)sizeof buf, retry);
  buf[sizeof buf - 1] = 0;
  if (ok) {
    memcpy(a->password, buf, sizeof buf);
    a->have_password = true;
  }
  wipe(buf, sizeof buf);
  return ok ? FM_OK : FM_ERR_PASSWORD;
}

/* ---- extraction sink ---------------------------------------------------- */

typedef struct Deferred { char *path; char *target; i64 mtime; u32 mode; bool is_dir; } Deferred;

struct FmArcSink {
  FmArc *a;
  u8 *sel;
  u8 *begun;
  const char *dst;           /* folder, or NULL in one-file mode */
  const char *one;           /* arc_extract_one target */
  const char *strip;
  size_t strip_len;
  FmArcCb cb;
  u64 done, total;
  int remaining;
  int written;
  bool cancelled, refused;
  /* the entry being written */
  FILE *out;
  int cur;
  bool link_mode;
  char *link;
  size_t link_len;
  char path[FM_PATH_MAX];
  char tmp[FM_PATH_MAX];
  char parent[FM_PATH_MAX];  /* last folder made, saves a stat per file */
  /* applied at the end */
  FmArena arena;
  struct { Deferred *items; int count, cap; } later;
};

#define LINK_MAX 4096

#ifdef FM_WIN
static bool win_reserved(const char *c, size_t n) {
  static const char *const kDev[] = { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$" };
  size_t base = 0;
  while (base < n && c[base] != '.') base++;
  for (int i = 0; i < FM_COUNT(kDev); i++)
    if (strlen(kDev[i]) == base && fm_strnicmp(c, kDev[i], base) == 0) return true;
  if (base == 4 && (fm_strnicmp(c, "COM", 3) == 0 || fm_strnicmp(c, "LPT", 3) == 0) &&
      c[3] >= '1' && c[3] <= '9')
    return true;
  return false;
}
#endif

/* rel ('/'-separated, already safe) appended to dst with native separators;
** Windows gets illegal characters, reserved names and trailing dots fixed. */
static bool out_path(char *out, size_t cap, const char *dst, const char *rel) {
  size_t n = 0, dl = strlen(dst);
  if (dl + 2 > cap) return false;
  memcpy(out, dst, dl);
  n = dl;
  if (n && !fm_is_sep(out[n - 1])) out[n++] = FM_SEP;
  const char *s = rel;
  while (*s) {
    const char *e = s;
    while (*e && *e != '/') e++;
    size_t len = (size_t)(e - s);
    if (len && !(len == 1 && s[0] == '.')) {
      if (n + len + 3 > cap) return false;
#ifdef FM_WIN
      if (win_reserved(s, len)) out[n++] = '_';
      size_t start = n;
      for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        out[n++] = (c < 32 || strchr("<>:\"|?*\\", c)) ? '_' : (char)c;
      }
      /* Win32 drops trailing dots and spaces, so "a." would land on "a". */
      for (size_t k = n; k > start && (out[k - 1] == '.' || out[k - 1] == ' '); k--)
        out[k - 1] = '_';
#else
      memcpy(out + n, s, len);
      n += len;
#endif
      out[n++] = FM_SEP;
    }
    s = *e ? e + 1 : e;
  }
  if (n > dl + 1) n--;            /* drop the last separator */
  out[n] = 0;
  return n > dl;
}

static FmErr make_parent(FmArcSink *s, const char *path) {
  char dir[FM_PATH_MAX];
  fm_strlcpy(dir, path, sizeof dir);
  if (!fm_path_parent(dir)) return FM_OK;
  if (strcmp(dir, s->parent) == 0) return FM_OK;
  FmErr e = plat_is_dir(dir) ? FM_OK : plat_mkdirs(dir);
  if (!e) fm_strlcpy(s->parent, dir, sizeof s->parent);
  return e;
}

static void defer(FmArcSink *s, const char *path, const char *target, const FmArcEntry *e) {
  Deferred d;
  d.path = arena_strdup(&s->arena, path);
  d.target = target ? arena_strdup(&s->arena, target) : NULL;
  d.mtime = e->mtime;
  d.mode = e->mode;
  d.is_dir = e->is_dir;
  FM_VEC_PUSH(s->later, d);
}

FmErr sink_begin(FmArcSink *s, int index, bool *skip) {
  *skip = true;
  if (s->cancelled) return FM_ERR_CANCEL;
  if (s->out || s->link_mode) sink_end(s, FM_ERR_IO);   /* backend forgot sink_end */
  FmArc *a = s->a;
  if (index < 0 || index >= a->n || !s->sel[index]) return FM_OK;
  if (!s->begun[index]) {
    s->begun[index] = 1;
    s->remaining--;
  }
  const FmArcEntry *e = &a->e[index];
  if (s->cb.entry) s->cb.entry(s->cb.ud, e->path);
  if (s->one) {
    fm_strlcpy(s->path, s->one, sizeof s->path);
  } else {
    const char *rel = e->path;
    if (s->strip_len) {
      if (strncmp(rel, s->strip, s->strip_len) == 0) rel += s->strip_len;
      else if (strlen(rel) + 1 == s->strip_len && strncmp(rel, s->strip, s->strip_len - 1) == 0)
        rel = "";                 /* the stripped folder itself */
    }
    if (!rel[0]) return FM_OK;
    if (!fm_path_is_safe_relative(rel) || !out_path(s->path, sizeof s->path, s->dst, rel)) {
      fm_log("archive: refused entry name '%s'", e->path);
      s->refused = true;
      return FM_OK;
    }
  }
  if (e->is_dir) {
    FmErr err = plat_mkdirs(s->path);
    if (err && !plat_is_dir(s->path)) return err;
    defer(s, s->path, NULL, e);
    s->written++;
    return FM_OK;
  }
  FmErr err = make_parent(s, s->path);
  if (err) return err;
  s->cur = index;
#ifdef FM_POSIX
  if (e->is_link && !s->one) {
    s->link_mode = true;
    s->link = (char *)fm_alloc(LINK_MAX + 1);
    s->link_len = 0;
    *skip = false;
    return FM_OK;
  }
#endif
  if (fm_snprintf(s->tmp, sizeof s->tmp, "%s.mmcpart", s->path) >= (int)sizeof s->tmp) {
    s->refused = true;
    return FM_OK;
  }
  s->out = fm_fopen(s->tmp, "wb");
  if (!s->out) return plat_is_dir(s->path) ? FM_ERR_EXISTS : FM_ERR_IO;
  *skip = false;
  return FM_OK;
}

FmErr sink_write(FmArcSink *s, const void *data, size_t n) {
  if (s->cancelled) return FM_ERR_CANCEL;
  if (s->link_mode) {
    if (s->link_len + n > LINK_MAX) return FM_ERR_FORMAT;
    memcpy(s->link + s->link_len, data, n);
    s->link_len += n;
  } else {
    if (!s->out) return FM_ERR_IO;
    if (n && fwrite(data, 1, n, s->out) != n) return FM_ERR_FULL;
  }
  s->done += n;
  if (s->cb.progress && !s->cb.progress(s->cb.ud, s->done, s->total)) {
    s->cancelled = true;
    return FM_ERR_CANCEL;
  }
  return FM_OK;
}

FmErr sink_end(FmArcSink *s, FmErr status) {
  if (s->link_mode) {
    s->link[s->link_len] = 0;
    if (!status) {
      defer(s, s->path, s->link, &s->a->e[s->cur]);
      s->written++;
    }
    fm_free(s->link);
    s->link = NULL;
    s->link_mode = false;
    return status;
  }
  if (!s->out) return status;
  if (fclose(s->out) != 0 && !status) status = FM_ERR_FULL;
  s->out = NULL;
  if (status) {
    plat_remove_file(s->tmp);
    return status;
  }
  if (plat_exists(s->path)) plat_remove_file(s->path);
  status = plat_rename(s->tmp, s->path);
  if (status) {
    plat_remove_file(s->tmp);
    return status;
  }
  const FmArcEntry *e = &s->a->e[s->cur];
  if (e->mtime) plat_set_mtime(s->path, e->mtime);
#ifdef FM_POSIX
  if (e->mode & 0777) plat_set_mode(s->path, e->mode & 0777);
#endif
  s->written++;
  return FM_OK;
}

bool sink_cancelled(FmArcSink *s) { return s->cancelled; }
int sink_remaining(FmArcSink *s) { return s->remaining; }

#ifdef FM_POSIX
/* True when `target`, read from the folder holding `rel`, stays below the
** destination root. */
static bool link_inside(const char *rel, const char *target) {
  if (!target[0] || target[0] == '/') return false;
  int depth = 0;
  for (const char *p = rel; *p; p++) if (*p == '/') depth++;
  const char *s = target;
  while (*s) {
    const char *e = s;
    while (*e && *e != '/') e++;
    size_t len = (size_t)(e - s);
    if (len == 2 && s[0] == '.' && s[1] == '.') {
      if (--depth < 0) return false;
    } else if (len && !(len == 1 && s[0] == '.')) {
      depth++;
    }
    s = *e ? e + 1 : e;
  }
  return true;
}
#endif

static void sink_finish(FmArcSink *s) {
  for (int i = s->later.count - 1; i >= 0; i--) {
    Deferred *d = &s->later.items[i];
    if (d->target) {
#ifdef FM_POSIX
      const char *rel = d->path + strlen(s->dst);
      while (*rel == '/') rel++;
      if (!link_inside(rel, d->target)) {
        fm_log("archive: link '%s' -> '%s' leaves the folder, skipped", rel, d->target);
        s->refused = true;
        continue;
      }
      unlink(d->path);
      if (symlink(d->target, d->path) != 0) fm_log("archive: cannot create link %s", d->path);
#endif
      continue;
    }
    if (d->is_dir) {
#ifdef FM_POSIX
      if (d->mode & 0777) plat_set_mode(d->path, (d->mode & 0777) | 0700);
#endif
      if (d->mtime) plat_set_mtime(d->path, d->mtime);
    }
  }
}

/* Selection with folders expanded to everything under them. */
typedef struct DirSet { const char **name; size_t *len; int cap; } DirSet;

static u32 hash_name(const char *s, size_t n) {
  u32 h = 2166136261u;
  for (size_t i = 0; i < n; i++) h = (h ^ (u8)s[i]) * 16777619u;
  return h;
}

static u8 *expand_selection(const FmArc *a, const u8 *sel) {
  u8 *out = (u8 *)fm_alloc((size_t)a->n + 1);
  int ndirs = 0;
  for (int i = 0; i < a->n; i++) {
    out[i] = sel ? (sel[i] != 0) : 1;
    if (sel && out[i] && a->e[i].is_dir) ndirs++;
  }
  if (!sel || !ndirs) return out;
  DirSet set;
  set.cap = 16;
  while (set.cap < ndirs * 2) set.cap *= 2;
  set.name = (const char **)fm_calloc((size_t)set.cap, sizeof *set.name);
  set.len = (size_t *)fm_calloc((size_t)set.cap, sizeof *set.len);
  for (int i = 0; i < a->n; i++) {
    if (!out[i] || !a->e[i].is_dir) continue;
    size_t n = strlen(a->e[i].path);
    u32 h = hash_name(a->e[i].path, n) & (u32)(set.cap - 1);
    while (set.name[h]) h = (h + 1) & (u32)(set.cap - 1);
    set.name[h] = a->e[i].path;
    set.len[h] = n;
  }
  for (int i = 0; i < a->n; i++) {
    if (out[i]) continue;
    const char *p = a->e[i].path;
    for (size_t k = 0; p[k] && !out[i]; k++) {
      if (p[k] != '/') continue;
      u32 h = hash_name(p, k) & (u32)(set.cap - 1);
      for (; set.name[h]; h = (h + 1) & (u32)(set.cap - 1))
        if (set.len[h] == k && memcmp(set.name[h], p, k) == 0) { out[i] = 1; break; }
    }
  }
  fm_free(set.name);
  fm_free(set.len);
  return out;
}

static FmErr run_extract(FmArc *a, FmArcSink *s, const FmArcCb *cb) {
  if (cb) a->cb = *cb;
  s->a = a;
  s->cb = a->cb;
  s->begun = (u8 *)fm_calloc((size_t)a->n + 1, 1);
  arena_init(&s->arena, 16 * 1024);
  for (int i = 0; i < a->n; i++)
    if (s->sel[i]) {
      s->remaining++;
      s->total += a->e[i].size;
    }
  FmErr e = a->be->extract ? a->be->extract(a, s->sel, s) : FM_ERR_UNSUPPORTED;
  if (s->out || s->link_mode) sink_end(s, e ? e : FM_ERR_IO);
  if (s->cancelled) e = FM_ERR_CANCEL;
  if (e != FM_ERR_CANCEL) sink_finish(s);
  if (!e && s->refused) e = FM_ERR_ACCESS;
  if (s->cb.progress && !e) s->cb.progress(s->cb.ud, s->total, s->total);
  arena_free(&s->arena);
  FM_VEC_FREE(s->later);
  fm_free(s->begun);
  fm_free(s->sel);
  return e;
}

FmErr arc_extract(FmArc *a, const u8 *sel, const char *dst_dir, const char *strip_prefix,
                  const FmArcCb *cb) {
  if (!a || !dst_dir || !dst_dir[0]) return FM_ERR_IO;
  FmErr e = plat_mkdirs(dst_dir);
  if (e && !plat_is_dir(dst_dir)) return e;
  FmArcSink s;
  memset(&s, 0, sizeof s);
  s.dst = dst_dir;
  s.strip = strip_prefix ? strip_prefix : "";
  s.strip_len = strlen(s.strip);
  s.sel = expand_selection(a, sel);
  return run_extract(a, &s, cb);
}

FmErr arc_extract_one(FmArc *a, int index, const char *dst_file, const FmArcCb *cb) {
  if (!a || index < 0 || index >= a->n) return FM_ERR_NOT_FOUND;
  FmArcSink s;
  memset(&s, 0, sizeof s);
  s.one = dst_file;
  s.dst = "";
  s.sel = (u8 *)fm_calloc((size_t)a->n + 1, 1);
  s.sel[index] = 1;
  char dir[FM_PATH_MAX];
  fm_strlcpy(dir, dst_file, sizeof dir);
  if (fm_path_parent(dir) && !plat_is_dir(dir)) plat_mkdirs(dir);
  FmErr e = run_extract(a, &s, cb);
  if (!e && !s.written) e = FM_ERR_NOT_FOUND;
  return e;
}

/* ---- creation ------------------------------------------------------------ */

typedef struct Walk {
  struct { FmArcSrc *items; int count, cap; } v;
  FmArena arena;
  const char *skip1, *skip2;     /* the archive being written */
  u64 total;
} Walk;

static bool same_path(const char *a, const char *b) {
#ifdef FM_WIN
  for (; *a && *b; a++, b++) {
    char x = *a == '/' ? '\\' : *a, y = *b == '/' ? '\\' : *b;
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
  }
  return *a == *b;
#else
  return strcmp(a, b) == 0;
#endif
}

static void walk_add(Walk *w, const char *abs, const char *name, const FmStat *st, bool is_dir) {
  FmArcSrc s;
  s.abs = arena_strdup(&w->arena, abs);
  s.name = arena_strdup(&w->arena, name);
  for (char *p = s.name; *p; p++) if (*p == '\\') *p = '/';
  s.size = is_dir ? 0 : st->size;
  s.mtime = st->mtime;
  s.mode = st->mode & 07777;
  if (!s.mode) s.mode = is_dir || (st->flags & FM_ST_EXEC) ? 0755 : 0644;
  s.is_dir = is_dir;
  w->total += s.size;
  FM_VEC_PUSH(w->v, s);
}

/* abs and name are buffers of FM_PATH_MAX extended in place per level. */
static FmErr walk_dir(Walk *w, char *abs, char *name) {
  FmErr err;
  FmDir *d = plat_dir_open(abs, &err);
  if (!d) return err ? err : FM_ERR_IO;
  size_t al = strlen(abs), nl = strlen(name);
  const char *child;
  FmStat st;
  FmErr e = FM_OK;
  while (!e && plat_dir_next(d, &child, &st)) {
    if (st.flags & FM_ST_BROKEN) continue;
    if (!fm_path_join(abs, FM_PATH_MAX, abs, child) ||
        fm_snprintf(name + nl, FM_PATH_MAX - nl, "/%s", child) >= (int)(FM_PATH_MAX - nl)) {
      fm_log("archive: path too long, skipped: %s", child);
      abs[al] = 0;
      name[nl] = 0;
      continue;
    }
    if (!(w->skip1 && same_path(abs, w->skip1)) && !(w->skip2 && same_path(abs, w->skip2))) {
      bool dir = (st.flags & FM_ST_DIR) != 0;
      if (dir && (st.flags & FM_ST_LINK)) {
        walk_add(w, abs, name, &st, true);       /* linked folders: no loops */
      } else if (dir) {
        walk_add(w, abs, name, &st, true);
        e = walk_dir(w, abs, name);
      } else {
        if (st.flags & FM_ST_LINK) plat_stat(abs, &st);
        walk_add(w, abs, name, &st, false);
      }
    }
    abs[al] = 0;
    name[nl] = 0;
  }
  plat_dir_close(d);
  return e;
}

bool arc_wprogress(FmArcWriteCtx *w, u64 add) {
  w->done += add;
  if (w->cb && w->cb->progress) return w->cb->progress(w->cb->ud, w->done, w->total);
  return true;
}

FmErr arc_create(const char *out_path, const char *const *srcs, int n, const char *base_dir,
                 const FmArcOpts *o, const FmArcCb *cb) {
  if (!o || !arc_fmt_can_create(o->fmt)) return FM_ERR_UNSUPPORTED;
  char part[FM_PATH_MAX];
  if (fm_snprintf(part, sizeof part, "%s.part", out_path) >= (int)sizeof part) return FM_ERR_IO;
  Walk w;
  memset(&w, 0, sizeof w);
  arena_init(&w.arena, 0);
  w.skip1 = out_path;
  w.skip2 = part;
  char *abs = (char *)fm_alloc(FM_PATH_MAX), *name = (char *)fm_alloc(FM_PATH_MAX);
  FmErr e = FM_OK;
  for (int i = 0; i < n && !e; i++) {
    FmStat st;
    if (!plat_stat(srcs[i], &st)) { e = FM_ERR_NOT_FOUND; break; }
    fm_strlcpy(abs, srcs[i], FM_PATH_MAX);
    size_t l = strlen(abs);
    while (l > 1 && fm_is_sep(abs[l - 1]) && !fm_path_is_root(abs)) abs[--l] = 0;
    const char *rel = fm_path_base(abs);
    if (base_dir && base_dir[0] && fm_path_is_inside(abs, base_dir)) {
      const char *r = abs + strlen(base_dir);
      while (*r && fm_is_sep(*r)) r++;
      if (*r) rel = r;
    }
    fm_strlcpy(name, rel, FM_PATH_MAX);
    for (char *p = name; *p; p++) if (*p == '\\') *p = '/';
    while (name[0] && name[strlen(name) - 1] == '/') name[strlen(name) - 1] = 0;
    if (!name[0]) continue;
    if (st.flags & FM_ST_DIR) {
      walk_add(&w, abs, name, &st, true);
      if (!(st.flags & FM_ST_LINK)) e = walk_dir(&w, abs, name);
    } else {
      walk_add(&w, abs, name, &st, false);
    }
  }
  fm_free(abs);
  fm_free(name);
  if (!e) {
    FILE *f = fm_fopen(part, "wb");
    if (!f) {
      e = FM_ERR_IO;
    } else {
      FmArcWriteCtx ctx;
      memset(&ctx, 0, sizeof ctx);
      ctx.src = w.v.items;
      ctx.n = w.v.count;
      ctx.total = w.total;
      ctx.o = o;
      ctx.cb = cb;
      switch (o->fmt) {
        case ARC_ZIP: e = zip_write(f, &ctx); break;
        case ARC_7Z: e = sevenz_write(f, &ctx); break;
        case ARC_TAR: case ARC_TAR_GZ: case ARC_TAR_BZ2: case ARC_TAR_XZ: case ARC_TAR_ZST:
          e = tar_write(f, &ctx);
          break;
        default: e = single_write(f, &ctx); break;
      }
      if (fclose(f) != 0 && !e) e = FM_ERR_FULL;
      if (!e) {
        if (plat_exists(out_path)) e = plat_remove_file(out_path);
        if (!e) e = plat_rename(part, out_path);
      }
      if (e) plat_remove_file(part);
    }
  }
  arena_free(&w.arena);
  FM_VEC_FREE(w.v);
  return e;
}
