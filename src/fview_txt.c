/* fview_txt.c -- streamed text viewer with a hex mode.
**
** Design decisions:
**   - Nothing reads the whole file. A worker scans it once in 4 MB steps
**     and records the byte offset of every 128th line (TXT_MARK); a line is
**     found by seeking to the mark before it and scanning at most 127
**     lines. 1 GB of logs costs about 1 MB of marks, and the first page
**     shows as soon as the first step is done.
**   - Only the visible lines (plus a margin) are decoded, into an arena that
**     is reset on every refill. Lines longer than TXT_LINE_MAX code points
**     are cut for display.
**   - The scroll position is a line anchor plus a pixel offset, not one
**     float: with millions of lines a float would lose whole lines, and
**     with word wrap the total height is unknown anyway.
**   - Monospace is faked with fixed cells: each code point is drawn at
**     column * cell width (wide CJK/emoji take two cells), so columns and
**     the hex dump line up although the only font is Poppins.
**   - Selection and find work on byte offsets in the file, so copying a
**     range or finding a match far below the screen needs no decoded text.
*/
#include "fview_int.h"
#include "fplat.h"

#define TXT_BUF (256 * 1024)
#define TXT_LINE_MAX 8192
#define TXT_COPY_MAX (16u * 1024u * 1024u)
#define TXT_TAB 4

/* ---- line index ----------------------------------------------------------------- */

static void ix_lock(FmTxtIndex *ix) { if (ix->mx) SDL_LockMutex(ix->mx); }
static void ix_unlock(FmTxtIndex *ix) { if (ix->mx) SDL_UnlockMutex(ix->mx); }

bool txt_index_open(FmTxtIndex *ix, const char *path) {
  memset(ix, 0, sizeof *ix);
  ix->f = fm_fopen(path, "rb");
  if (!ix->f) return false;
  ix->rf = fm_fopen(path, "rb");
  if (!ix->rf) { fclose(ix->f); ix->f = NULL; return false; }
  i64 sz = fm_fsize(ix->f);
  ix->size = sz > 0 ? (u64)sz : 0;
  ix->unit = 1;
  u8 h[8192];
  size_t n = fread(h, 1, sizeof h, ix->f);
  if (n >= 3 && h[0] == 0xEF && h[1] == 0xBB && h[2] == 0xBF) ix->start = 3;
  else if (n >= 2 && h[0] == 0xFF && h[1] == 0xFE) { ix->unit = 2; ix->start = 2; }
  else if (n >= 2 && h[0] == 0xFE && h[1] == 0xFF) { ix->unit = 2; ix->be = true; ix->start = 2; }
  else {
    size_t z_even = 0, z_odd = 0;
    for (size_t i = 0; i < n; i++)
      if (h[i] == 0) { if (i & 1) z_odd++; else z_even++; }
    if (n >= 16 && z_odd > n / 4 && z_even == 0) ix->unit = 2;                      /* UTF-16LE, no BOM */
    else if (n >= 16 && z_even > n / 4 && z_odd == 0) { ix->unit = 2; ix->be = true; }
    else if (z_even + z_odd > 0) ix->binary = true;
  }
  ix->cap = 64;
  ix->marks = (u64 *)fm_alloc(sizeof(u64) * (size_t)ix->cap);
  ix->marks[0] = ix->start;
  ix->nmarks = 1;
  ix->lines = 1;
  ix->scanned = ix->start;
  ix->buf = (u8 *)fm_alloc(TXT_BUF);
  fm_fseek64(ix->f, (i64)ix->start, SEEK_SET);
  if (ix->size <= ix->start) ix->done = true;
  return true;
}

void txt_index_close(FmTxtIndex *ix) {
  if (ix->f) fclose(ix->f);
  if (ix->rf) fclose(ix->rf);
  fm_free(ix->marks);
  fm_free(ix->buf);
  memset(ix, 0, sizeof *ix);
}

/* Line-ending state machine shared by the scanner and the lookups:
** '\n', "\r\n" and a lone '\r' all end a line. Calls back with the offset
** where each new line starts. */
typedef struct LineScan {
  bool pending_cr;
  u64 base;                /* offset of buf[0] */
} LineScan;

static inline u32 unit_at(const u8 *b, size_t i, int unit, bool be) {
  if (unit == 1) return b[i];
  return be ? (u32)(b[i] << 8 | b[i + 1]) : (u32)(b[i] | b[i + 1] << 8);
}

/* Returns the number of line starts found in b[0..n); each start goes to
** out[] (up to max). */
static size_t scan_lines(LineScan *s, const u8 *b, size_t n, int unit, bool be, u64 *out, size_t max,
                         size_t *nout) {
  size_t found = 0;
  *nout = 0;
  for (size_t i = 0; i + (size_t)unit <= n; i += (size_t)unit) {
    u32 c = unit_at(b, i, unit, be);
    u64 pos = s->base + i;
    if (s->pending_cr) {
      s->pending_cr = false;
      if (c == '\n') {
        if (*nout < max) out[(*nout)++] = pos + (u64)unit;
        found++;
        continue;
      }
      if (*nout < max) out[(*nout)++] = pos;
      found++;
    }
    if (c == '\n') {
      if (*nout < max) out[(*nout)++] = pos + (u64)unit;
      found++;
    } else if (c == '\r') {
      s->pending_cr = true;
    }
  }
  s->base += n - (n % (size_t)unit);
  return found;
}

bool txt_index_step(FmTxtIndex *ix, size_t bytes) {
  if (ix->done) return false;
  size_t want = FM_MIN(bytes, (size_t)TXT_BUF);
  if (ix->unit == 2) want = FM_MAX((size_t)2, want & ~(size_t)1);
  size_t n = fread(ix->buf, 1, want, ix->f);
  u64 starts[4096];
  LineScan s = { ix->pending_cr, ix->scanned };
  if (n == 0) {
    ix_lock(ix);
    if (ix->pending_cr) {
      /* a final '\r' ended a line: the next (empty) line starts at EOF */
      if (ix->lines % TXT_MARK == 0) {
        if (ix->nmarks == ix->cap) { ix->cap *= 2; ix->marks = (u64 *)fm_realloc(ix->marks, sizeof(u64) * (size_t)ix->cap); }
        ix->marks[ix->nmarks++] = ix->scanned;
      }
      ix->lines++;
      ix->pending_cr = false;
    }
    ix->done = true;
    ix_unlock(ix);
    return false;
  }
  /* scan in slices so the start buffer never overflows */
  size_t off = 0;
  while (off < n) {
    size_t slice = FM_MIN(n - off, (size_t)4096 * (size_t)ix->unit);
    if (ix->unit == 2) slice &= ~(size_t)1;
    if (slice == 0) break;
    size_t cnt;
    scan_lines(&s, ix->buf + off, slice, ix->unit, ix->be, starts, FM_COUNT(starts), &cnt);
    ix_lock(ix);
    for (size_t i = 0; i < cnt; i++) {
      if (ix->lines % TXT_MARK == 0) {
        if (ix->nmarks == ix->cap) {
          ix->cap *= 2;
          ix->marks = (u64 *)fm_realloc(ix->marks, sizeof(u64) * (size_t)ix->cap);
        }
        ix->marks[ix->nmarks++] = starts[i];
      }
      ix->lines++;
    }
    ix->scanned = s.base;
    ix->pending_cr = s.pending_cr;
    ix_unlock(ix);
    off += slice;
  }
  return true;
}

/* Reads lines forward from `from` (a line start) until `count` more line
** starts are seen or `limit` is reached; returns the last start found. */
static u64 scan_forward(FmTxtIndex *ix, u64 from, u64 count, u64 limit, u64 *seen) {
  u8 buf[16384];
  u64 starts[4096];
  LineScan s = { false, from };
  u64 last = from, got = 0;
  *seen = 0;
  if (count == 0) return from;
  if (fm_fseek64(ix->rf, (i64)from, SEEK_SET) != 0) return from;
  while (s.base < limit) {
    size_t want = (size_t)FM_MIN((u64)sizeof buf, limit - s.base);
    if (ix->unit == 2) want &= ~(size_t)1;
    if (want == 0) break;
    size_t n = fread(buf, 1, want, ix->rf);
    if (n == 0) break;
    size_t cnt;
    scan_lines(&s, buf, n, ix->unit, ix->be, starts, FM_COUNT(starts), &cnt);
    for (size_t i = 0; i < cnt; i++) {
      last = starts[i];
      if (++got == count) { *seen = got; return last; }
    }
  }
  /* trailing lone CR at the limit (end of file) */
  if (s.pending_cr && s.base >= ix->size) { last = ix->size; got++; }
  *seen = got;
  return last;
}

u64 txt_line_offset(FmTxtIndex *ix, u64 line) {
  ix_lock(ix);
  u64 known = ix->lines;
  if (line >= known) line = known ? known - 1 : 0;
  u64 k = line / TXT_MARK;
  if (k >= (u64)ix->nmarks) k = (u64)ix->nmarks - 1;
  u64 mark = ix->marks[k];
  u64 limit = ix->done ? ix->size : ix->scanned;
  ix_unlock(ix);
  u64 need = line - k * TXT_MARK, seen;
  if (need == 0) return mark;
  return scan_forward(ix, mark, need, limit, &seen);
}

/* Line number that contains byte offset `off`. */
static u64 txt_line_of(FmTxtIndex *ix, u64 off) {
  ix_lock(ix);
  int lo = 0, hi = ix->nmarks - 1;
  while (lo < hi) {
    int mid = (lo + hi + 1) / 2;
    if (ix->marks[mid] <= off) lo = mid; else hi = mid - 1;
  }
  u64 mark = ix->marks[lo], line = (u64)lo * TXT_MARK;
  ix_unlock(ix);
  /* count line starts in (mark, off] */
  u8 buf[16384];
  u64 starts[4096];
  LineScan s = { false, mark };
  if (fm_fseek64(ix->rf, (i64)mark, SEEK_SET) != 0) return line;
  while (s.base <= off) {
    size_t want = (size_t)FM_MIN((u64)sizeof buf, off + 1 - s.base + (u64)ix->unit);
    if (ix->unit == 2) want &= ~(size_t)1;
    if (want == 0) break;
    size_t n = fread(buf, 1, want, ix->rf);
    if (n == 0) break;
    size_t cnt;
    scan_lines(&s, buf, n, ix->unit, ix->be, starts, FM_COUNT(starts), &cnt);
    for (size_t i = 0; i < cnt; i++)
      if (starts[i] <= off) line++;
    if (n < want) break;
  }
  return line;
}

/* ---- viewer state ------------------------------------------------------------------ */

typedef struct TLine {
  u64 off;              /* file offset of the line start */
  u32 nbytes;           /* bytes without the terminator */
  char *txt;            /* UTF-8 with tabs expanded */
  int ncp;              /* code points in txt */
  u32 *bofs;            /* byte offset in the line of each code point (+1 sentinel) */
  int *col;             /* start column of each code point (+1 sentinel = width) */
  bool cut;
} TLine;

typedef struct VisRow { u64 line; int cp0, cp1; float y; } VisRow;

static struct {
  bool open;
  char path[FM_PATH_MAX];
  char title[256];
  char sub[96];
  FmTxtIndex ix;
  bool ix_ok;
  SDL_Thread *ix_thr;
  SDL_atomic_t quit;
  bool hex, wrap;
  float zoom;
  /* scroll anchor */
  u64 top;
  double top_px;
  float vel;
  bool dragging, sb_drag;
  float drag_last;
  /* decoded lines */
  FmArena arena;
  TLine *lines;
  int nlines, lines_cap;
  u64 cache_first;
  bool cache_ok;
  /* visible rows (hit testing) */
  VisRow rows[512];
  int nrows;
  float cell, lh, text_x;
  int ncols;
  /* selection (byte offsets) */
  u64 sel_a, sel_b;
  bool selecting;
  /* find */
  bool find_open;
  char query[256];
  SDL_Thread *find_thr;
  SDL_atomic_t find_busy, find_cancel;
  SDL_mutex *find_mx;
  i64 find_result;         /* -2 pending, -1 none, else offset */
  u64 match_off, match_len;
  bool find_dir_back;
  /* hex page */
  u8 *hexbuf;
  u64 hex_off;
  size_t hex_len;
  int hex_cols;
  bool info_open;
  float bottom_gap;
} g;

static u64 total_rows(void) {
  if (g.hex) {
    u64 n = (g.ix.size + (u64)g.hex_cols - 1) / (u64)g.hex_cols;
    return n ? n : 1;
  }
  ix_lock(&g.ix);
  u64 n = g.ix.lines;
  ix_unlock(&g.ix);
  return n ? n : 1;
}

static bool index_done(void) {
  ix_lock(&g.ix);
  bool d = g.ix.done;
  ix_unlock(&g.ix);
  return d;
}

/* ---- worker threads -------------------------------------------------------------------- */

static int index_thread(void *u) {
  FM_UNUSED(u);
  u64 last = plat_now_ms();
  while (!SDL_AtomicGet(&g.quit) && txt_index_step(&g.ix, 4u << 20)) {
    u64 now = plat_now_ms();
    if (now - last > 200) { app_wake(); last = now; }
  }
  app_wake();
  return 0;
}

static void start_index(void) {
  if (g.ix_thr || !g.ix_ok || g.ix.done) return;
  g.ix_thr = fm_thread_create(index_thread, "txt-index", NULL);
  if (!g.ix_thr) while (txt_index_step(&g.ix, 4u << 20)) {}
}

typedef struct FindJob {
  char path[FM_PATH_MAX];
  u8 pat[1024];
  int plen;
  int unit;
  u64 start, size, from;
  bool back;
} FindJob;

static inline u8 fold(u8 c) { return (c >= 'A' && c <= 'Z') ? (u8)(c + 32) : c; }

static bool match_at(const u8 *b, const u8 *p, int n) {
  for (int i = 0; i < n; i++)
    if (fold(b[i]) != p[i]) return false;
  return true;
}

/* Searches [lo, hi) for the folded pattern; returns the first (or last) hit. */
static i64 find_range(FILE *f, const FindJob *j, u64 lo, u64 hi, bool last, u8 *buf, size_t cap) {
  if (hi <= lo || hi - lo < (u64)j->plen) return -1;
  size_t ov = (size_t)j->plen - 1;
  i64 best = -1;
  if (!last) {
    for (u64 p = lo; p < hi; ) {
      if (SDL_AtomicGet(&g.find_cancel)) return -1;
      size_t want = (size_t)FM_MIN((u64)cap, hi - p);
      if (fm_fseek64(f, (i64)p, SEEK_SET) != 0) return -1;
      size_t n = fread(buf, 1, want, f);
      if (n < (size_t)j->plen) return -1;
      for (size_t i = 0; i + (size_t)j->plen <= n; i++) {
        if (fold(buf[i]) != j->pat[0]) continue;
        if (j->unit == 2 && ((p + i - j->start) & 1)) continue;
        if (match_at(buf + i, j->pat, j->plen)) return (i64)(p + i);
      }
      if (n < want || p + n >= hi) break;
      p += n - ov;
    }
    return -1;
  }
  for (u64 end = hi; end > lo; ) {
    if (SDL_AtomicGet(&g.find_cancel)) return -1;
    u64 p = end - lo > (u64)cap ? end - (u64)cap : lo;
    size_t want = (size_t)(end - p);
    if (fm_fseek64(f, (i64)p, SEEK_SET) != 0) return -1;
    size_t n = fread(buf, 1, want, f);
    for (size_t i = n >= (size_t)j->plen ? n - (size_t)j->plen + 1 : 0; i-- > 0;) {
      if (fold(buf[i]) != j->pat[0]) continue;
      if (j->unit == 2 && ((p + i - j->start) & 1)) continue;
      if (match_at(buf + i, j->pat, j->plen)) { best = (i64)(p + i); return best; }
    }
    if (p == lo) break;
    end = p + ov;
    if (end >= p + n) break;
  }
  return best;
}

static int find_thread(void *u) {
  FindJob *j = (FindJob *)u;
  FILE *f = fm_fopen(j->path, "rb");
  i64 r = -1;
  if (f) {
    size_t cap = 1 << 20;
    u8 *buf = (u8 *)fm_alloc(cap);
    if (!j->back) {
      r = find_range(f, j, j->from, j->size, false, buf, cap);
      if (r < 0) r = find_range(f, j, j->start, FM_MIN(j->size, j->from + (u64)j->plen), false, buf, cap);
    } else {
      r = find_range(f, j, j->start, j->from, true, buf, cap);
      if (r < 0) r = find_range(f, j, j->from, j->size, true, buf, cap);
    }
    fm_free(buf);
    fclose(f);
  }
  SDL_LockMutex(g.find_mx);
  g.find_result = SDL_AtomicGet(&g.find_cancel) ? -1 : r;
  SDL_UnlockMutex(g.find_mx);
  SDL_AtomicSet(&g.find_busy, 0);
  fm_free(j);
  app_wake();
  return 0;
}

static void find_join(void) {
  if (g.find_thr) {
    SDL_AtomicSet(&g.find_cancel, 1);
    SDL_WaitThread(g.find_thr, NULL);
    g.find_thr = NULL;
    SDL_AtomicSet(&g.find_cancel, 0);
  }
}

static void find_start(bool back) {
  if (!g.query[0] || SDL_AtomicGet(&g.find_busy)) return;
  find_join();
  FindJob *j = (FindJob *)fm_calloc(1, sizeof *j);
  fm_strlcpy(j->path, g.path, sizeof j->path);
  j->unit = g.ix.unit;
  /* pattern in the file's encoding, ASCII-folded */
  const char *q = g.query;
  while (*q && j->plen < (int)sizeof j->pat - 4) {
    u32 cp;
    q += utf8_decode(q, &cp);
    if (j->unit == 1) {
      char e[4];
      int k = utf8_encode(cp, e);
      for (int i = 0; i < k; i++) j->pat[j->plen++] = fold((u8)e[i]);
    } else {
      if (cp > 0xFFFF) cp = 0xFFFD;
      u8 lo = (u8)(cp & 255), hi = (u8)(cp >> 8);
      if (g.ix.be) { j->pat[j->plen++] = hi; j->pat[j->plen++] = fold(lo); }
      else { j->pat[j->plen++] = fold(lo); j->pat[j->plen++] = hi; }
    }
  }
  if (j->plen == 0) { fm_free(j); return; }
  j->start = g.ix.start;
  j->size = g.ix.size;
  j->back = back;
  u64 cur = g.match_len ? g.match_off : (g.lines && g.cache_ok ? txt_line_offset(&g.ix, g.top) : j->start);
  if (g.hex) cur = g.match_len ? g.match_off : g.top * (u64)g.hex_cols;
  j->from = back ? cur : (g.match_len ? cur + 1 : cur);
  if (j->from > j->size) j->from = j->size;
  g.find_dir_back = back;
  SDL_LockMutex(g.find_mx);
  g.find_result = -2;
  SDL_UnlockMutex(g.find_mx);
  SDL_AtomicSet(&g.find_busy, 1);
  g.find_thr = fm_thread_create(find_thread, "txt-find", j);
  if (!g.find_thr) { SDL_AtomicSet(&g.find_busy, 0); fm_free(j); }
}

/* ---- decoding lines ------------------------------------------------------------------- */

static bool is_wide(u32 c) {
  return (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF && c != 0x303F) ||
         (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
         (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) ||
         (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x20000 && c <= 0x3FFFD);
}

/* Builds the display form of one raw line. */
static void decode_line(TLine *L, const u8 *raw, size_t n) {
  int cap = (int)FM_MIN(n, (size_t)TXT_LINE_MAX) + 1;
  /* worst case: every code point a tab -> TXT_TAB spaces */
  L->txt = (char *)arena_alloc(&g.arena, (size_t)cap * 4 + (size_t)cap * TXT_TAB + 8);
  L->bofs = (u32 *)arena_alloc(&g.arena, sizeof(u32) * (size_t)(cap + 1));
  L->col = (int *)arena_alloc(&g.arena, sizeof(int) * (size_t)(cap + 1));
  int unit = g.ix.unit, ncp = 0, col = 0;
  size_t o = 0, i = 0;
  while (i < n && ncp < TXT_LINE_MAX) {
    u32 cp;
    size_t k;
    if (unit == 1) {
      char tmp[5] = { 0 };
      size_t m = FM_MIN((size_t)4, n - i);
      memcpy(tmp, raw + i, m);
      k = (size_t)utf8_decode(tmp, &cp);
      if (k > m) { k = 1; cp = 0xFFFD; }
    } else {
      if (i + 1 >= n) break;
      cp = unit_at(raw, i, 2, g.ix.be);
      k = 2;
      if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < n) {
        u32 d = unit_at(raw, i + 2, 2, g.ix.be);
        if (d >= 0xDC00 && d < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (d - 0xDC00); k = 4; }
      }
    }
    L->bofs[ncp] = (u32)i;
    L->col[ncp] = col;
    if (cp == '\t') {
      int sp = TXT_TAB - col % TXT_TAB;
      for (int s = 0; s < sp; s++) L->txt[o++] = ' ';
      col += sp;
    } else {
      if (cp < 0x20 || cp == 0x7F) cp = 0xB7;          /* middle dot for controls */
      char e[4];
      int m = utf8_encode(cp, e);
      memcpy(L->txt + o, e, (size_t)m);
      o += (size_t)m;
      col += is_wide(cp) ? 2 : 1;
    }
    ncp++;
    i += k;
  }
  L->txt[o] = 0;
  L->bofs[ncp] = (u32)FM_MIN(i, n);
  L->col[ncp] = col;
  L->ncp = ncp;
  L->cut = i < n;
}

/* Byte length (as stored in the txt string) of code point j: tabs became spaces. */
static int cp_txt_bytes(const TLine *L, int j, const char *p) {
  int span = L->col[j + 1] - L->col[j];
  if (*p == ' ' && span > 1) {
    /* expanded tab: `span` spaces, unless it was a wide char (never a space) */
    return span;
  }
  u32 cp;
  return utf8_decode(p, &cp);
}

static void fetch_lines(u64 first, int count) {
  arena_reset(&g.arena);
  if (count > g.lines_cap) {
    g.lines_cap = count;
    g.lines = (TLine *)fm_realloc(g.lines, sizeof(TLine) * (size_t)count);
  }
  g.nlines = 0;
  g.cache_first = first;
  g.cache_ok = true;
  u64 total = total_rows();
  if (first >= total) return;
  u64 off = txt_line_offset(&g.ix, first);
  FILE *f = g.ix.rf;
  if (fm_fseek64(f, (i64)off, SEEK_SET) != 0) return;
  int unit = g.ix.unit;
  size_t rawcap = (size_t)TXT_LINE_MAX * 4 + 8;
  u8 *raw = (u8 *)fm_alloc(rawcap);
  u8 buf[16384];
  size_t bn = 0, bp = 0;
  u64 pos = off;
  bool eof = false;
  while (g.nlines < count && (u64)g.nlines + first < total) {
    TLine *L = &g.lines[g.nlines];
    memset(L, 0, sizeof *L);
    L->off = pos;
    size_t rn = 0;
    u64 len = 0;
    bool ended = false;
    while (!ended) {
      if (bp + (size_t)unit > bn) {
        if (eof) break;
        size_t keep = bn - bp;
        memmove(buf, buf + bp, keep);
        bn = keep + fread(buf + keep, 1, sizeof buf - keep, f);
        bp = 0;
        if (bn - keep == 0) eof = true;
        if (bp + (size_t)unit > bn) break;
      }
      u32 c = unit_at(buf, bp, unit, g.ix.be);
      if (c == '\n') { bp += (size_t)unit; pos += (u64)unit; ended = true; break; }
      if (c == '\r') {
        bp += (size_t)unit;
        pos += (u64)unit;
        /* CRLF: peek the next unit */
        if (bp + (size_t)unit > bn && !eof) {
          size_t keep = bn - bp;
          memmove(buf, buf + bp, keep);
          bn = keep + fread(buf + keep, 1, sizeof buf - keep, f);
          bp = 0;
          if (bn == keep) eof = true;
        }
        if (bp + (size_t)unit <= bn && unit_at(buf, bp, unit, g.ix.be) == '\n') { bp += (size_t)unit; pos += (u64)unit; }
        ended = true;
        break;
      }
      if (rn + (size_t)unit <= rawcap) { memcpy(raw + rn, buf + bp, (size_t)unit); rn += (size_t)unit; }
      bp += (size_t)unit;
      pos += (u64)unit;
      len += (u64)unit;
    }
    L->nbytes = (u32)FM_MIN(len, (u64)0xFFFFFFFFu);
    decode_line(L, raw, rn);
    if (len > rn) L->cut = true;
    g.nlines++;
    if (!ended && eof) break;
  }
  fm_free(raw);
}

static TLine *line_at(u64 line) {
  if (!g.cache_ok || line < g.cache_first || line >= g.cache_first + (u64)g.nlines) return NULL;
  return &g.lines[line - g.cache_first];
}

/* Makes sure [first, first+count) is decoded (refetching around it). */
static void ensure_lines(u64 first, int count) {
  u64 total = total_rows();
  u64 last = FM_MIN(first + (u64)count, total);
  if (g.cache_ok && first >= g.cache_first && last <= g.cache_first + (u64)g.nlines) return;
  u64 from = first > 32 ? first - 32 : 0;
  fetch_lines(from, count + 96);
}

static int line_rows(const TLine *L) {
  if (!g.wrap || g.ncols <= 0) return 1;
  int w = L->col[L->ncp];
  return w <= 0 ? 1 : (w + g.ncols - 1) / g.ncols;
}

/* ---- scrolling ------------------------------------------------------------------------ */

static void scroll_by(double d) {
  u64 total = total_rows();
  if (!g.wrap || g.hex) {
    double pos = (double)g.top * g.lh + g.top_px + d;
    if (pos < 0) pos = 0;
    double maxpos = (double)total * g.lh - g.lh;           /* refined by the bottom gap below */
    if (pos > maxpos) pos = FM_MAX(0.0, maxpos);
    g.top = (u64)(pos / g.lh);
    g.top_px = pos - (double)g.top * g.lh;
    return;
  }
  g.top_px += d;
  int guard = 0;
  while (g.top_px < 0 && guard++ < 100000) {
    if (g.top == 0) { g.top_px = 0; break; }
    g.top--;
    ensure_lines(g.top, 64);
    TLine *L = line_at(g.top);
    g.top_px += (L ? line_rows(L) : 1) * g.lh;
  }
  while (guard++ < 100000) {
    ensure_lines(g.top, 64);
    TLine *L = line_at(g.top);
    double h = (L ? line_rows(L) : 1) * g.lh;
    if (g.top_px < h) break;
    if (g.top + 1 >= total) { g.top_px = FM_MIN(g.top_px, h - g.lh); break; }
    g.top_px -= h;
    g.top++;
  }
}

static void scroll_to_line(u64 line, bool center, float view_h) {
  g.top = line;
  g.top_px = 0;
  if (center) scroll_by(-(double)view_h * 0.35);
  g.vel = 0;
}

/* ---- open / close ---------------------------------------------------------------------- */

static void txt_close(void) {
  if (!g.open) return;
  SDL_AtomicSet(&g.quit, 1);
  if (g.ix_thr) SDL_WaitThread(g.ix_thr, NULL);
  find_join();
  if (g.ix_ok) {
    SDL_mutex *mx = g.ix.mx;
    txt_index_close(&g.ix);
    if (mx) SDL_DestroyMutex(mx);
  }
  if (g.find_mx) SDL_DestroyMutex(g.find_mx);
  arena_free(&g.arena);
  fm_free(g.lines);
  fm_free(g.hexbuf);
  memset(&g, 0, sizeof g);
}

static bool txt_open(const char *path, const char *const *list, int n, int index) {
  FM_UNUSED(list); FM_UNUSED(n); FM_UNUSED(index);
  txt_close();
  memset(&g, 0, sizeof g);
  g.open = true;
  fm_strlcpy(g.path, path, sizeof g.path);
  fm_strlcpy(g.title, fm_path_base(path), sizeof g.title);
  g.zoom = 1.0f;
  g.hex_cols = 16;
  arena_init(&g.arena, 64 * 1024);
  g.find_mx = SDL_CreateMutex();
  g.find_result = -2;
  g.ix_ok = txt_index_open(&g.ix, path);
  if (g.ix_ok) {
    g.ix.mx = SDL_CreateMutex();
    g.hex = g.ix.binary;
    if (!g.hex) {
      /* the first step on this thread: the first page shows at once */
      txt_index_step(&g.ix, 64 * 1024);
      start_index();
    }
  }
  ui_redraw();
  return true;
}

/* ---- clipboard ------------------------------------------------------------------------- */

static void copy_range(u64 a, u64 b) {
  if (b <= a) return;
  if (b - a > TXT_COPY_MAX) { b = a + TXT_COPY_MAX; ui_toast("Copied the first 16 MB"); }
  size_t n = (size_t)(b - a);
  u8 *raw = (u8 *)fm_alloc(n + 2);
  if (fm_fseek64(g.ix.rf, (i64)a, SEEK_SET) != 0) { fm_free(raw); return; }
  n = fread(raw, 1, n, g.ix.rf);
  char *s;
  if (g.ix.unit == 1) {
    raw[n] = 0;
    s = (char *)raw;
    raw = NULL;
  } else {
    s = (char *)fm_alloc(n * 2 + 4);
    size_t o = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
      u32 cp = unit_at(raw, i, 2, g.ix.be);
      if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < n) {
        u32 d = unit_at(raw, i + 2, 2, g.ix.be);
        if (d >= 0xDC00 && d < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (d - 0xDC00); i += 2; }
      }
      if (cp == 0) cp = ' ';
      o += (size_t)utf8_encode(cp, s + o);
    }
    s[o] = 0;
  }
  /* NUL bytes would cut the clipboard text short */
  for (size_t i = 0; i < n && g.ix.unit == 1; i++) if (!s[i]) s[i] = ' ';
  ui_clipboard_set(s);
  fm_free(s);
  fm_free(raw);
}

/* ---- hex mode ------------------------------------------------------------------------ */

static const u8 *hex_page(u64 off, size_t len) {
  if (g.hexbuf && off >= g.hex_off && off + len <= g.hex_off + g.hex_len) return g.hexbuf + (off - g.hex_off);
  size_t cap = FM_MAX(len * 2, (size_t)65536);
  fm_free(g.hexbuf);
  g.hexbuf = (u8 *)fm_alloc(cap);
  g.hex_off = off > cap / 4 ? off - cap / 4 : 0;
  g.hex_off -= g.hex_off % (u64)g.hex_cols;
  g.hex_len = 0;
  if (fm_fseek64(g.ix.rf, (i64)g.hex_off, SEEK_SET) == 0) g.hex_len = fread(g.hexbuf, 1, cap, g.ix.rf);
  if (off < g.hex_off || off > g.hex_off + g.hex_len) return NULL;
  return g.hexbuf + (off - g.hex_off);
}

static void draw_hex(FmRect c, float fs) {
  static const char kHex[] = "0123456789ABCDEF";
  float cw = g.cell;
  int digits = g.ix.size > 0xFFFFFFFFull ? 12 : 8;
  /* columns: offset, gap, B*3 (+1 mid gap), gap, B ascii */
  int B = 16;
  float need = (digits + 2 + 16 * 3 + 1 + 1 + 16) * cw + DP(16);
  if (need > c.w) B = 8;
  if (B != g.hex_cols) {
    u64 byte = g.top * (u64)g.hex_cols;
    g.hex_cols = B;
    g.top = byte / (u64)B;
    fm_free(g.hexbuf);
    g.hexbuf = NULL;
  }
  float x0 = c.x + DP(8);
  float xh = x0 + (digits + 2) * cw;
  float xa = xh + (B * 3 + (B > 8 ? 1 : 0) + 1) * cw;
  int vis = (int)(c.h / g.lh) + 2;
  u64 first = g.top;
  const u8 *page = hex_page(first * (u64)B, (size_t)vis * (size_t)B);
  gfx_rect(FM_RECT(c.x, c.y, xh - cw - c.x, c.h), T.surface2);
  float ty = (g.lh - font_line_h(fs)) * 0.5f;
  g.nrows = 0;
  for (int r = 0; r < vis; r++) {
    u64 row = first + (u64)r;
    u64 off = row * (u64)B;
    if (off >= g.ix.size) break;
    float y = c.y + r * g.lh - (float)g.top_px;
    if (y > c.y + c.h) break;
    char ob[24];
    for (int d = 0; d < digits; d++) ob[d] = kHex[(off >> (4 * (digits - 1 - d))) & 15];
    ob[digits] = 0;
    font_draw(FONT_REGULAR, fs, x0, y + ty, ob, digits, T.text3);
    int nb = (int)FM_MIN((u64)B, g.ix.size - off);
    for (int i = 0; i < nb; i++) {
      u8 v = page ? page[(size_t)r * (size_t)B + (size_t)i] : 0;
      u64 at = off + (u64)i;
      float hx = xh + (i * 3 + (i >= 8 && B > 8 ? 1 : 0)) * cw, ax = xa + i * cw;
      bool hit = g.match_len && at >= g.match_off && at < g.match_off + g.match_len;
      if (hit) {
        gfx_rect(FM_RECT(hx - cw * 0.25f, y, cw * 2.5f, g.lh), col_alpha(T.warn, 0.45f));
        gfx_rect(FM_RECT(ax, y, cw, g.lh), col_alpha(T.warn, 0.45f));
      }
      char hb[2] = { kHex[v >> 4], kHex[v & 15] };
      font_draw(FONT_REGULAR, fs, hx, y + ty, hb, 2, v ? T.text : T.text3);
      char ab = (v >= 0x20 && v < 0x7F) ? (char)v : '.';
      font_draw(FONT_REGULAR, fs, ax, y + ty, &ab, 1, (v >= 0x20 && v < 0x7F) ? T.text : T.text3);
    }
  }
  g.bottom_gap = 0;
}

/* ---- text mode ------------------------------------------------------------------------- */

static void sel_range(u64 *a, u64 *b) {
  *a = FM_MIN(g.sel_a, g.sel_b);
  *b = FM_MAX(g.sel_a, g.sel_b);
}

/* File offset under the pointer, from the rows drawn last frame. */
static bool hit_offset(float mx, float my, u64 *out) {
  if (g.nrows == 0) return false;
  int ri = 0;
  for (int i = 0; i < g.nrows; i++)
    if (my >= g.rows[i].y) ri = i;
  VisRow *vr = &g.rows[ri];
  TLine *L = line_at(vr->line);
  if (!L) return false;
  int row_col0 = L->col[vr->cp0];
  int col = (int)floorf((mx - g.text_x) / g.cell + 0.5f) + row_col0;
  int j = vr->cp0;
  while (j < vr->cp1 && L->col[j + 1] <= col) j++;
  *out = L->off + L->bofs[j];
  return true;
}

static void draw_sel(const TLine *L, int cp0, int cp1, float y, u64 a, u64 b, FmColor c) {
  if (b <= L->off || a > L->off + L->nbytes) return;
  int s = cp1, e = cp0;
  for (int j = cp0; j <= cp1; j++) {
    u64 at = L->off + L->bofs[j];
    if (at >= a && j < s) s = j;
    if (at < b) e = j + 1;
  }
  if (e > cp1) e = cp1;
  bool eol = b > L->off + L->nbytes && cp1 == L->ncp && a <= L->off + L->nbytes;
  if (s >= e && !eol) return;
  if (s > e) s = e;
  float x0 = g.text_x + (L->col[s] - L->col[cp0]) * g.cell;
  float x1 = g.text_x + (L->col[e] - L->col[cp0]) * g.cell + (eol ? g.cell * 0.6f : 0);
  gfx_rect(FM_RECT(x0, y, x1 - x0, g.lh), c);
}

static void draw_text(FmRect c, float fs) {
  u64 total = total_rows();
  int digits = 1;
  for (u64 t = total; t >= 10; t /= 10) digits++;
  float gut = (FM_MAX(digits, 3) + 2) * g.cell;
  g.text_x = c.x + gut + DP(6);
  float tw = c.w - gut - DP(14);
  g.ncols = g.wrap ? FM_MAX(8, (int)(tw / g.cell)) : 0;
  gfx_rect(FM_RECT(c.x, c.y, gut, c.h), T.surface2);
  int vis_lines = (int)(c.h / g.lh) + 3;
  ensure_lines(g.top, vis_lines);
  float ty = (g.lh - font_line_h(fs)) * 0.5f;
  u64 sa, sb;
  sel_range(&sa, &sb);
  FmColor selc = T.sel, hitc = col_alpha(T.warn, 0.45f);
  g.nrows = 0;
  float y = c.y - (float)g.top_px;
  u64 li = g.top;
  gfx_clip_push(c);
  while (y < c.y + c.h && li < total) {
    TLine *L = line_at(li);
    if (!L) { ensure_lines(li, vis_lines); L = line_at(li); if (!L) break; }
    int rows = line_rows(L);
    char num[24];
    int nn = fm_snprintf(num, sizeof num, "%llu", (unsigned long long)(li + 1));
    float nx = c.x + gut - DP(6) - nn * g.cell;
    for (int k = 0; k < nn; k++) font_draw(FONT_REGULAR, fs * 0.9f, nx + k * g.cell, y + ty + fs * 0.05f, num + k, 1, T.text3);
    /* rows of this line */
    int cp = 0;
    const char *p = L->txt;
    for (int r = 0; r < rows; r++) {
      int cp0 = cp, cp1 = L->ncp;
      if (g.wrap) {
        int limit = L->col[cp0] + g.ncols;
        cp1 = cp0;
        while (cp1 < L->ncp && L->col[cp1 + 1] <= limit) cp1++;
        if (cp1 == cp0 && cp1 < L->ncp) cp1++;
      }
      float ry = y + r * g.lh;
      if (ry + g.lh >= c.y && ry <= c.y + c.h) {
        if (g.nrows < FM_COUNT(g.rows)) {
          VisRow *vr = &g.rows[g.nrows++];
          vr->line = li;
          vr->cp0 = cp0;
          vr->cp1 = cp1;
          vr->y = ry;
        }
        if (sb > sa) draw_sel(L, cp0, cp1, ry, sa, sb, selc);
        if (g.match_len) draw_sel(L, cp0, cp1, ry, g.match_off, g.match_off + g.match_len, hitc);
        /* glyphs at fixed cells; runs of plain ASCII go in one call per cell */
        const char *q = p;
        for (int j = cp0; j < cp1; j++) {
          int nb = cp_txt_bytes(L, j, q);
          float gx = g.text_x + (L->col[j] - L->col[cp0]) * g.cell;
          if (gx > c.x + c.w) { /* skip the rest of this row */
            for (int k = j; k < cp1; k++) q += cp_txt_bytes(L, k, q);
            break;
          }
          if (*q != ' ') font_draw(FONT_REGULAR, fs, gx, ry + ty, q, nb, T.text);
          q += nb;
        }
        p = q;
      } else {
        for (int j = cp0; j < cp1; j++) p += cp_txt_bytes(L, j, p);
      }
      cp = cp1;
    }
    if (L->cut) {
      float ex = g.text_x + (g.wrap ? 0 : L->col[L->ncp] * g.cell) + DP(4);
      float ey = y + (rows - 1) * g.lh;
      if (g.wrap) ex = g.text_x + (L->col[L->ncp] - L->col[cp]) * g.cell + DP(4);
      font_draw(FONT_BOLD, fs * 0.85f, ex, ey + ty, "\xE2\x80\xA6", -1, T.text3);
    }
    y += rows * g.lh;
    li++;
  }
  gfx_clip_pop();
  g.bottom_gap = li >= total ? FM_MAX(0.0f, c.y + c.h - y) : 0;
}

/* ---- frame ---------------------------------------------------------------------------- */

static void goto_top(void) { g.top = 0; g.top_px = 0; g.vel = 0; }
static void goto_bottom(float view_h) {
  u64 t = total_rows();
  g.top = t > 0 ? t - 1 : 0;
  g.top_px = 0;
  g.vel = 0;
  scroll_by(-(double)view_h + g.lh * 1.5);
}

static void handle_find_result(float view_h) {
  i64 r;
  SDL_LockMutex(g.find_mx);
  r = g.find_result;
  if (r != -2) g.find_result = -2;
  SDL_UnlockMutex(g.find_mx);
  if (r == -2) return;
  if (g.find_thr && !SDL_AtomicGet(&g.find_busy)) { SDL_WaitThread(g.find_thr, NULL); g.find_thr = NULL; }
  if (r < 0) {
    ui_toast("\"%s\" not found", g.query);
    g.match_len = 0;
    return;
  }
  g.match_off = (u64)r;
  int ulen = 0;
  for (const char *q = g.query; *q;) { u32 cp; q += utf8_decode(q, &cp); ulen += g.ix.unit == 1 ? utf8_encode(cp, (char[4]){ 0 }) : 2; }
  g.match_len = (u64)ulen;
  if (g.hex) {
    u64 row = g.match_off / (u64)g.hex_cols;
    if (row < g.top || row >= g.top + (u64)(view_h / g.lh) - 1) scroll_to_line(row, true, view_h);
  } else {
    u64 line = txt_line_of(&g.ix, g.match_off);
    bool visible = false;
    for (int i = 0; i < g.nrows; i++)
      if (g.rows[i].line == line && g.rows[i].y > 0) visible = true;
    if (!visible) scroll_to_line(line, true, view_h);
  }
}

static void info_card(void) {
  char size[32], lines[48], enc[32];
  fm_fmt_size(g.ix.size, size, sizeof size);
  if (g.hex && g.ix.binary) fm_strlcpy(lines, "binary", sizeof lines);
  else fm_snprintf(lines, sizeof lines, "%llu%s", (unsigned long long)total_rows(), index_done() ? "" : " (counting)");
  fm_strlcpy(enc, g.ix.binary ? "binary" : g.ix.unit == 2 ? (g.ix.be ? "UTF-16 BE" : "UTF-16 LE")
                 : g.ix.start == 3 ? "UTF-8 (BOM)" : "UTF-8", sizeof enc);
  const char *keys[] = { "Name", "Size", "Lines", "Encoding", "Path" };
  const char *vals[] = { g.title, size, lines, enc, g.path };
  view_info_dialog(ui_id("txt.info"), "File info", keys, vals, 5, &g.info_open);
}

enum { TM_TOP = 1, TM_BOTTOM, TM_WRAP, TM_COPY, TM_SELALL, TM_OPEN, TM_SHARE, TM_INFO };

static void txt_frame(FmRect area) {
  gfx_rect(area, T.surface);
  float fs = ui.m.font * 0.92f * g.zoom;
  g.cell = font_width(FONT_REGULAR, fs, "0", 1);
  g.lh = floorf(font_line_h(fs) * 1.12f);
  if (g.cell <= 0) g.cell = fs * 0.6f;

  /* top bar */
  char sub[96];
  if (!g.ix_ok) sub[0] = 0;
  else if (g.hex) fm_snprintf(sub, sizeof sub, "Hex  \xC2\xB7  %s", fm_fmt_size(g.ix.size, (char[32]){ 0 }, 32));
  else fm_snprintf(sub, sizeof sub, "%llu lines%s", (unsigned long long)total_rows(), index_done() ? "" : "\xE2\x80\xA6");
  FmRect act;
  bool narrow = ui.w < DP(460);
  if (view_topbar(area, VIEW_BAR_THEME, 1.0f, g.title, sub, narrow ? 3 : 4, &act)) { app_close_viewer(); return; }
  u32 mid = ui_id("txt.menu");
  if (view_bar_btn(&act, ui_id("txt.more"), IC_MORE, "More", VIEW_BAR_THEME, 1, false)) {
    u64 a, b;
    sel_range(&a, &b);
    FmMenuItem items[] = {
      { TM_TOP, IC_ARROW_UP, "Go to top", "Ctrl+Home", 0 },
      { TM_BOTTOM, IC_ARROW_DOWN, "Go to bottom", "Ctrl+End", 0 },
      { TM_WRAP, IC_TEXT, "Word wrap", "W", (g.wrap ? UI_MI_CHECKED : 0) | (g.hex ? UI_MI_DISABLED : 0) },
      { 0, IC_NONE, NULL, NULL, UI_MI_SEP },
      { TM_COPY, IC_COPY, "Copy", "Ctrl+C", (b > a && !g.hex) ? 0 : UI_MI_DISABLED },
      { TM_SELALL, IC_SELECT_ALL, "Select all", "Ctrl+A", g.hex ? UI_MI_DISABLED : 0 },
      { 0, IC_NONE, NULL, NULL, UI_MI_SEP },
      { TM_OPEN, IC_OPEN_WITH, "Open with system app", NULL, 0 },
      { TM_SHARE, IC_SHARE, "Share", NULL, 0 },
      { TM_INFO, IC_INFO, "File info", NULL, 0 },
    };
    ui_menu_open(mid, act.x + act.w, area.y + ui.m.bar_h, items, FM_COUNT(items));
  }
  if (view_bar_btn(&act, ui_id("txt.hex"), IC_HEX, "Hex view (H)", VIEW_BAR_THEME, 1, g.hex) && g.ix_ok) {
    g.hex = !g.hex;
    g.top = 0; g.top_px = 0;
    if (!g.hex) start_index();
  }
  if (view_bar_btn(&act, ui_id("txt.find"), IC_SEARCH, "Find (Ctrl+F)", VIEW_BAR_THEME, 1, g.find_open)) {
    g.find_open = !g.find_open;
    if (g.find_open) ui_focus(ui_id("txt.q"));
  }
  if (!narrow && view_bar_btn(&act, ui_id("txt.wrap"), IC_TEXT, "Word wrap (W)", VIEW_BAR_THEME, 1, g.wrap) && !g.hex) {
    g.wrap = !g.wrap;
    g.top_px = 0;
  }

  FmRect body = area;
  rect_cut_top(&body, ui.m.bar_h);
  if (!g.ix_ok) {
    view_message(body, IC_WARN, "Cannot open this file", g.path, T.text, T.text2);
    if (view_key_back()) app_close_viewer();
    return;
  }

  /* find bar */
  if (g.find_open) {
    FmRect fb = rect_cut_bottom(&body, ui.m.bar_h + DP(8));
    gfx_rect(fb, T.surface2);
    ui_divider(fb.x, fb.x + fb.w, fb.y);
    FmRect r = rect_inset2(fb, DP(8), DP(6));
    float bh = r.h;
    bool close = ui_icon_btn(ui_id("txt.fx"), rect_cut_right(&r, bh), IC_CLOSE, T.text2, "Close");
    bool next = ui_icon_btn(ui_id("txt.fn"), rect_cut_right(&r, bh), IC_CHEVRON_DOWN, T.text2, "Next (Enter, F3)");
    bool prev = ui_icon_btn(ui_id("txt.fp"), rect_cut_right(&r, bh), IC_CHEVRON_UP, T.text2, "Previous (Shift+Enter)");
    if (SDL_AtomicGet(&g.find_busy)) ui_spinner(rect_cut_right(&r, bh), T.accent);
    rect_cut_right(&r, DP(4));
    int tf = ui_textfield(ui_id("txt.q"), r, g.query, sizeof g.query, "Find", UI_TF_FOCUS);
    if (tf & UI_TF_CHANGED) g.match_len = 0;
    if (tf & UI_TF_SUBMIT) { if (ui.mod & KMOD_SHIFT) prev = true; else next = true; }
    if ((tf & UI_TF_CANCEL) || close) { g.find_open = false; g.match_len = 0; ui_focus(0); }
    if (next) find_start(false);
    if (prev) find_start(true);
  }
  FmRect c = body;
  handle_find_result(c.h);

  /* keys */
  u32 cid = ui_id("txt.body");
  float page = FM_MAX(g.lh, c.h - g.lh * 2);
  if (ui_key(SDLK_f, KMOD_CTRL)) { g.find_open = true; ui_focus(ui_id("txt.q")); }
  if (ui_key(SDLK_F3, 0)) find_start(false);
  if (ui_key(SDLK_F3, KMOD_SHIFT)) find_start(true);
  if (ui_key(SDLK_DOWN, 0)) scroll_by(g.lh);
  if (ui_key(SDLK_UP, 0)) scroll_by(-g.lh);
  if (ui_key(SDLK_PAGEDOWN, 0) || ui_key(SDLK_SPACE, 0)) scroll_by(page);
  if (ui_key(SDLK_PAGEUP, 0) || ui_key(SDLK_SPACE, KMOD_SHIFT)) scroll_by(-page);
  if (ui_key(SDLK_HOME, KMOD_CTRL) || ui_key(SDLK_HOME, 0)) goto_top();
  if (ui_key(SDLK_END, KMOD_CTRL) || ui_key(SDLK_END, 0)) goto_bottom(c.h);
  if (ui_key(SDLK_w, 0) && !g.hex) { g.wrap = !g.wrap; g.top_px = 0; }
  if (ui_key(SDLK_h, 0)) { g.hex = !g.hex; g.top = 0; g.top_px = 0; if (!g.hex) start_index(); }
  if (ui_key(SDLK_EQUALS, KMOD_CTRL) || ui_key(SDLK_PLUS, KMOD_CTRL) || ui_key(SDLK_KP_PLUS, KMOD_CTRL)) g.zoom = FM_MIN(2.5f, g.zoom * 1.1f);
  if (ui_key(SDLK_MINUS, KMOD_CTRL) || ui_key(SDLK_KP_MINUS, KMOD_CTRL)) g.zoom = FM_MAX(0.6f, g.zoom / 1.1f);
  if (ui_key(SDLK_0, KMOD_CTRL)) g.zoom = 1.0f;
  if (ui_key(SDLK_a, KMOD_CTRL) && !g.hex) { g.sel_a = g.ix.start; g.sel_b = g.ix.size; }
  if (ui_key(SDLK_c, KMOD_CTRL) && !g.hex) { u64 a, b; sel_range(&a, &b); copy_range(a, b); }

  /* menu */
  int mr = ui_menu_result(mid);
  switch (mr) {
    case TM_TOP: goto_top(); break;
    case TM_BOTTOM: goto_bottom(c.h); break;
    case TM_WRAP: if (!g.hex) { g.wrap = !g.wrap; g.top_px = 0; } break;
    case TM_COPY: { u64 a, b; sel_range(&a, &b); copy_range(a, b); ui_toast("Copied"); break; }
    case TM_SELALL: g.sel_a = g.ix.start; g.sel_b = g.ix.size; break;
    case TM_OPEN: plat_open_external(g.path); break;
    case TM_SHARE: plat_share(g.path); break;
    case TM_INFO: g.info_open = true; break;
    default: break;
  }

  /* pointer: wheel, drag scroll (touch), selection (mouse), scrollbar */
  FmRect sbar = { c.x + c.w - DP(ui.touch_mode ? 18 : 12), c.y, DP(ui.touch_mode ? 18 : 12), c.h };
  bool in = ui_input_ok() && rect_has(c, ui.mx, ui.my);
  if (in && ui.wheel != 0) {
    if (ui.mod & KMOD_CTRL) g.zoom = FM_CLAMP(g.zoom * (ui.wheel > 0 ? 1.1f : 1 / 1.1f), 0.6f, 2.5f);
    else scroll_by(-ui.wheel * g.lh * 3);
    g.vel = 0;
    ui.wheel = 0;
  }
  if (in && ui.pinch != 1.0f && ui.nfingers >= 2) g.zoom = FM_CLAMP(g.zoom * ui.pinch, 0.6f, 2.5f);
  int f = ui_hit(cid, c);
  bool touchish = ui.from_touch || ui.touch_mode;
  u64 total = total_rows();
  if ((f & UI_PRESS) && rect_has(sbar, ui.mx, ui.my) && total > 1) { g.sb_drag = true; ui.drag_owner = cid; }
  if (g.sb_drag) {
    if (ui.down) {
      double t = FM_CLAMP((ui.my - c.y) / c.h, 0.0, 1.0);
      g.top = (u64)(t * (double)(total - 1));
      g.top_px = 0;
      g.vel = 0;
    } else {
      g.sb_drag = false;
    }
  } else if (f & UI_PRESS) {
    g.vel = 0;
    g.drag_last = ui.my;
    g.dragging = false;
    if (!touchish && !g.hex) {
      u64 o;
      if (hit_offset(ui.mx, ui.my, &o)) {
        if (ui.mod & KMOD_SHIFT) g.sel_b = o;
        else g.sel_a = g.sel_b = o;
        g.selecting = true;
      }
      if (ui.clicks >= 2 && hit_offset(ui.mx, ui.my, &o)) {
        /* double click: the whole line */
        for (int i = 0; i < g.nrows; i++) {
          TLine *L = line_at(g.rows[i].line);
          if (L && o >= L->off && o <= L->off + L->nbytes) { g.sel_a = L->off; g.sel_b = L->off + L->nbytes; }
        }
        g.selecting = false;
      }
    }
  }
  if ((f & UI_HELD) && g.selecting && !touchish) {
    ui.drag_owner = cid;
    u64 o;
    if (hit_offset(FM_CLAMP(ui.mx, g.text_x, c.x + c.w), FM_CLAMP(ui.my, c.y, c.y + c.h - 1), &o)) g.sel_b = o;
    if (ui.my < c.y + g.lh) { scroll_by(-g.lh * 0.5); ui_animate(); }
    if (ui.my > c.y + c.h - g.lh) { scroll_by(g.lh * 0.5); ui_animate(); }
  }
  if (!ui.down) g.selecting = false;
  if ((f & UI_LONG) && touchish && !g.hex) {
    u64 o;
    if (hit_offset(ui.mx, ui.my, &o)) {
      for (int i = 0; i < g.nrows; i++) {
        TLine *L = line_at(g.rows[i].line);
        if (L && o >= L->off && o <= L->off + L->nbytes) { g.sel_a = L->off; g.sel_b = L->off + L->nbytes; }
      }
      copy_range(FM_MIN(g.sel_a, g.sel_b), FM_MAX(g.sel_a, g.sel_b));
      ui_toast("Line copied");
    }
  }
  if ((f & UI_DRAG) && touchish && !g.sb_drag) {
    ui.drag_owner = cid;
    g.dragging = true;
    float d = g.drag_last - ui.my;
    g.drag_last = ui.my;
    scroll_by(d);
    if (ui.dt > 0) g.vel = g.vel * 0.6f + (d / ui.dt) * 0.4f;
  } else if (g.dragging && !ui.down) {
    g.dragging = false;
  }
  if (!ui.down && !g.dragging && fabsf(g.vel) > 1.0f) {
    scroll_by(g.vel * ui.dt);
    g.vel *= expf(-ui.dt * 3.2f);
    if (fabsf(g.vel) < DP(15)) g.vel = 0;
    ui_animate();
  }

  /* draw */
  if (g.hex) draw_hex(c, fs);
  else draw_text(c, fs);
  if (g.bottom_gap > g.lh * 0.5f && (g.top > 0 || g.top_px > 0) && (g.hex || index_done())) {
    scroll_by(-(double)g.bottom_gap);
    ui_redraw();
  }

  /* scrollbar */
  if (total > 1) {
    double t = ((double)g.top + g.top_px / g.lh) / (double)(total - 1);
    float th = FM_MAX(DP(28), c.h * (float)FM_MIN(1.0, c.h / g.lh / (double)total));
    float y = c.y + (c.h - th) * (float)FM_CLAMP(t, 0.0, 1.0);
    float w = g.sb_drag ? DP(6) : DP(4);
    gfx_rrect(FM_RECT(c.x + c.w - w - DP(3), y, w, th), w * 0.5f, col_alpha(T.text2, g.sb_drag ? 0.8f : 0.45f));
  }
  if (!index_done() && !g.hex) ui_progress(FM_RECT(c.x, c.y, c.w, DP(2)), (float)((double)g.ix.scanned / (double)FM_MAX(1, g.ix.size)));

  if (g.info_open) info_card();
  if (view_key_back()) {
    if (g.find_open) { g.find_open = false; g.match_len = 0; }
    else app_close_viewer();
  }
}

const FmViewer g_view_text = { "text", txt_open, txt_frame, txt_close };
