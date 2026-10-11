/* flyrics.c -- lyrics from LRCLIB (see flyrics.h).
**
** Design decisions:
**   - LRCLIB (lrclib.net/api, free, no key, checked 2026-10-11):
**     /get?artist_name=&track_name= answers one song with "syncedLyrics"
**     (LRC: "[mm:ss.xx] line") and "plainLyrics"; when it does not know
**     the exact names, /search?q= lists candidates and the one with synced
**     lyrics and the nearest length wins.
**   - Titles from YouTube and the like carry noise: "Artist - Song (Official
**     Music Video) [HD]", channels named "ArtistVEVO" or "Artist - Topic".
**     lyrics_clean cuts that down to an artist and a track before asking.
**   - One song at a time: a worker fetches it (cancelled when the song
**     changes) and the lines live in one table the main thread reads after
**     the worker is done. The reply is cached on disk (lyrics/<hash>.json),
**     a "not found" too, so a song costs the network once.
**   - Drawing is time-based: the line being sung is found by binary search
**     at the player's position; the list glides to it (ui_anim), which
**     costs frames only while it moves.
*/
#include "flyrics.h"
#include "fjson.h"
#include "fnet.h"
#include "fplat.h"
#include "fsdl.h"
#include "fui.h"
#include "ftheme.h"
#include "ffont.h"
#include "fapp.h"
#include <math.h>

#define LY_MAX_LINES 600

typedef struct LyLine { double t; char *s; } LyLine;

static struct {
  char key[600];             /* artist|title asked for */
  int state;
  char msg[160];
  char song[300];
  LyLine *ln;
  int n;
  char *text;                /* the lines' storage */
  /* the fetch */
  SDL_Thread *thr;
  SDL_atomic_t done;
  volatile int cancel;
  char q_artist[200], q_track[300];
  double q_dur;
  char *reply;               /* the worker's result (JSON), NULL = failed */
  size_t reply_len;
  char reply_err[160];
  /* plain lyrics scroll */
  FmScroll scroll;
} L;

/* ---- cleaning titles ------------------------------------------------------------- */

static bool ieq_prefix(const char *s, const char *p) { return fm_strnicmp(s, p, strlen(p)) == 0; }

/* Brackets with noise words go: (Official Video), [Lyrics], (Audio), ... */
static bool noisy(const char *in, size_t n) {
  static const char *const kNoise[] = { "official", "video", "lyric", "audio", "visualizer", "hd", "4k", "mv",
                                        "remaster", "live", "explicit", "clean", "color coded", "karaoke", "m/v" };
  char b[160];
  size_t m = FM_MIN(n, sizeof b - 1);
  memcpy(b, in, m);
  b[m] = 0;
  for (int i = 0; i < FM_COUNT(kNoise); i++)
    if (fm_stristr(b, kNoise[i])) return true;
  return false;
}

static void trim(char *s) {
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '-' || s[n - 1] == '|' || s[n - 1] == ':')) s[--n] = 0;
  size_t k = 0;
  while (s[k] == ' ' || s[k] == '-' || s[k] == '|') k++;
  if (k) memmove(s, s + k, n - k + 1);
}

/* Cuts "ft. X" / "feat. X" and noisy brackets out of t (in place). */
static void strip_noise(char *t) {
  char out[400];
  size_t o = 0;
  for (const char *p = t; *p && o + 1 < sizeof out;) {
    if (*p == '(' || *p == '[') {
      char close = *p == '(' ? ')' : ']';
      const char *e = strchr(p, close);
      if (e && (noisy(p + 1, (size_t)(e - p - 1)) || ieq_prefix(p + 1, "feat") || ieq_prefix(p + 1, "ft."))) {
        p = e + 1;
        continue;
      }
    }
    if ((p == t || p[-1] == ' ') && (ieq_prefix(p, "feat. ") || ieq_prefix(p, "ft. ") || ieq_prefix(p, "featuring ")))
      break;                                       /* the guests: to the end */
    if (*p == '|') break;                          /* "Song | Album name" */
    out[o++] = *p++;
  }
  out[o] = 0;
  fm_strlcpy(t, out, 400);
  trim(t);
}

void lyrics_clean(const char *artist, const char *title, char *a, size_t acap, char *t, size_t tcap) {
  char tt[400], aa[200];
  fm_strlcpy(tt, title ? title : "", sizeof tt);
  fm_strlcpy(aa, artist ? artist : "", sizeof aa);
  /* a file's name: no extension */
  static const char *const kExt[] = { ".mp4", ".mkv", ".webm", ".avi", ".mov", ".m4v", ".mp3", ".m4a", ".flac",
                                      ".ogg", ".opus", ".wav", ".aac", ".wma" };
  for (int i = 0; i < FM_COUNT(kExt); i++) {
    size_t n = strlen(tt), m = strlen(kExt[i]);
    if (n > m && !fm_stricmp(tt + n - m, kExt[i])) { tt[n - m] = 0; break; }
  }
  /* the channel's decorations */
  static const char *const kTail[] = { " - Topic", "VEVO", " Official", "Official" };
  for (int i = 0; i < FM_COUNT(kTail); i++) {
    size_t n = strlen(aa), m = strlen(kTail[i]);
    if (n > m && !fm_stricmp(aa + n - m, kTail[i])) aa[n - m] = 0;
  }
  trim(aa);
  /* "Artist - Song": the dash says who sings */
  const char *dash = strstr(tt, " - ");
  if (!dash) dash = strstr(tt, " \xE2\x80\x93 ");      /* an en dash */
  if (dash) {
    char left[200];
    size_t n = FM_MIN((size_t)(dash - tt), sizeof left - 1);
    memcpy(left, tt, n);
    left[n] = 0;
    const char *right = dash + (dash[1] == '-' ? 3 : 5);
    char r[400];
    fm_strlcpy(r, right, sizeof r);
    fm_strlcpy(aa, left, sizeof aa);
    fm_strlcpy(tt, r, sizeof tt);
  }
  strip_noise(tt);
  strip_noise(aa);
  /* quotes round the song: "Artist "Song"" */
  size_t n = strlen(tt);
  if (n > 2 && tt[0] == '"' && tt[n - 1] == '"') { tt[n - 1] = 0; memmove(tt, tt + 1, n - 1); }
  fm_strlcpy(a, aa, acap);
  fm_strlcpy(t, tt, tcap);
}

/* ---- parsing ---------------------------------------------------------------------- */

static void clear_lines(void) {
  fm_free(L.ln);
  fm_free(L.text);
  L.ln = NULL;
  L.text = NULL;
  L.n = 0;
}

static int cmp_t(const void *a, const void *b) {
  double x = ((const LyLine *)a)->t, y = ((const LyLine *)b)->t;
  return x < y ? -1 : x > y;
}

/* LRC: "[mm:ss.xx]line", several stamps on one line, [ar:...] tags skipped. */
static int parse_lrc(const char *s) {
  size_t len = strlen(s);
  L.text = (char *)fm_alloc(len + 1);
  L.ln = (LyLine *)fm_alloc(sizeof *L.ln * LY_MAX_LINES);
  size_t to = 0;
  const char *p = s;
  while (*p && L.n < LY_MAX_LINES) {
    const char *e = strchr(p, '\n');
    if (!e) e = p + strlen(p);
    double ts[8];
    int nts = 0;
    const char *q = p;
    while (*q == '[') {
      int mm = 0;
      double ss = 0;
      const char *c = strchr(q, ']');
      if (!c || c > e) break;
      if (q[1] >= '0' && q[1] <= '9' && sscanf(q + 1, "%d:%lf", &mm, &ss) == 2 && nts < 8) ts[nts++] = mm * 60.0 + ss;
      q = c + 1;
    }
    if (nts) {
      size_t n = (size_t)(e - q);
      while (n && (q[n - 1] == '\r' || q[n - 1] == ' ')) n--;
      while (n && *q == ' ') { q++; n--; }
      char *line = L.text + to;
      memcpy(line, q, n);
      line[n] = 0;
      to += n + 1;
      for (int k = 0; k < nts && L.n < LY_MAX_LINES; k++) {
        L.ln[L.n].t = ts[k];
        L.ln[L.n].s = line;
        L.n++;
      }
    }
    p = *e ? e + 1 : e;
  }
  qsort(L.ln, (size_t)L.n, sizeof *L.ln, cmp_t);
  return L.n ? LY_SYNCED : LY_MISSING;
}

static int parse_plain(const char *s) {
  size_t len = strlen(s);
  L.text = (char *)fm_alloc(len + 1);
  memcpy(L.text, s, len + 1);
  L.ln = (LyLine *)fm_alloc(sizeof *L.ln * LY_MAX_LINES);
  for (char *p = L.text; *p && L.n < LY_MAX_LINES;) {
    char *e = strchr(p, '\n');
    if (e) *e = 0;
    size_t n = strlen(p);
    if (n && p[n - 1] == '\r') p[n - 1] = 0;
    L.ln[L.n].t = -1;
    L.ln[L.n].s = p;
    L.n++;
    if (!e) break;
    p = e + 1;
  }
  return L.n ? LY_PLAIN : LY_MISSING;
}

/* One song object: synced first, else plain. */
static int parse_song(const FmJsonNode *o) {
  if (json_bool(json_get(o, "instrumental"), false)) {
    fm_strlcpy(L.msg, "An instrumental: no lyrics", sizeof L.msg);
    return LY_MISSING;
  }
  fm_snprintf(L.song, sizeof L.song, "%s - %s", json_str(json_get(o, "artistName"), ""),
              json_str(json_get(o, "trackName"), ""));
  const char *syn = json_str(json_get(o, "syncedLyrics"), "");
  if (syn[0] && parse_lrc(syn) == LY_SYNCED) return LY_SYNCED;
  clear_lines();
  const char *pl = json_str(json_get(o, "plainLyrics"), "");
  if (pl[0]) return parse_plain(pl);
  fm_strlcpy(L.msg, "No lyrics for this song", sizeof L.msg);
  return LY_MISSING;
}

int lyrics_parse(const char *json, size_t len) {
  clear_lines();
  L.msg[0] = L.song[0] = 0;
  FmJson j;
  if (json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(L.msg, "LRCLIB sent a reply this app does not understand", sizeof L.msg);
    return LY_ERROR;
  }
  const FmJsonNode *root = json_root(&j);
  int st = LY_MISSING;
  if (root->type == JSON_OBJ && json_get(root, "trackName")) {
    st = parse_song(root);
  } else if (root->type == JSON_ARR) {
    /* search results: the first with synced lyrics, else the first with any */
    const FmJsonNode *pick = NULL;
    for (const FmJsonNode *x = json_first(root); x && !pick; x = json_next(x))
      if (json_str(json_get(x, "syncedLyrics"), "")[0]) pick = x;
    for (const FmJsonNode *x = json_first(root); x && !pick; x = json_next(x))
      if (json_str(json_get(x, "plainLyrics"), "")[0]) pick = x;
    if (pick) st = parse_song(pick);
  }
  if (st == LY_MISSING && !L.msg[0]) fm_strlcpy(L.msg, "LRCLIB has no lyrics for this song", sizeof L.msg);
  json_free(&j);
  return st;
}

int lyrics_count(void) { return L.n; }
const char *lyrics_line(int i, double *t) {
  if (i < 0 || i >= L.n) return NULL;
  if (t) *t = L.ln[i].t;
  return L.ln[i].s;
}

/* ---- fetching ----------------------------------------------------------------------- */

static bool cache_file(const char *key, char *out, size_t cap) {
  char dir[FM_PATH_MAX], sub[FM_PATH_MAX], name[40];
  if (!plat_place(PLACE_CACHE, dir, sizeof dir) || !fm_path_join(sub, sizeof sub, dir, "lyrics")) return false;
  plat_mkdirs(sub);
  u64 h = 1469598103934665603ull;
  for (const char *s = key; *s; s++) h = (h ^ (u8)*s) * 1099511628211ull;
  fm_snprintf(name, sizeof name, "%016llx.json", (unsigned long long)h);
  return fm_path_join(out, cap, sub, name);
}

static char *read_file(const char *path, size_t max, size_t *n) {
  FILE *f = fm_fopen(path, "rb");
  if (!f) return NULL;
  i64 sz = fm_fsize(f);
  char *buf = NULL;
  if (sz >= 0 && (u64)sz <= max) {
    buf = (char *)fm_alloc((size_t)sz + 1);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fm_free(buf); buf = NULL; }
    else { buf[sz] = 0; *n = (size_t)sz; }
  }
  fclose(f);
  return buf;
}

static void write_file(const char *path, const char *data, size_t n) {
  FILE *f = fm_fopen(path, "wb");
  if (!f) return;
  if (n) fwrite(data, 1, n, f);
  fclose(f);
}

static bool get(const char *url, char **out, size_t *len, char *err, size_t cap, volatile int *cancel, bool *missing) {
  FmNetResp r;
  memset(&r, 0, sizeof r);
  FmErr e = net_get(url, "Lrclib-Client: mmcfm (MMC File Manager)\r\n", 1u << 20, &r, cancel);
  *missing = e == FM_OK && r.status == 404;
  bool ok = e == FM_OK && r.status == 200 && r.data;
  if (ok) {
    *out = (char *)r.data;
    *len = r.len;
    r.data = NULL;
  } else if (!*missing) {
    if (e != FM_OK) fm_snprintf(err, cap, "No connection to LRCLIB (%s)", r.error[0] ? r.error : fm_err_str(e));
    else fm_snprintf(err, cap, "LRCLIB answered %d", r.status);
  }
  net_resp_free(&r);
  return ok;
}

static int SDLCALL worker(void *u) {
  FM_UNUSED(u);
  char ae[600], te[900], url[1800];
  net_urlencode(L.q_artist, ae, sizeof ae);
  net_urlencode(L.q_track, te, sizeof te);
  char *body = NULL;
  size_t len = 0;
  bool missing = false;
  /* the exact song first (the artist known), then a search */
  if (L.q_artist[0]) {
    fm_snprintf(url, sizeof url, "https://lrclib.net/api/get?artist_name=%s&track_name=%s", ae, te);
    get(url, &body, &len, L.reply_err, sizeof L.reply_err, &L.cancel, &missing);
  }
  if (!body && !L.cancel && (missing || !L.q_artist[0])) {
    char q[600], qe[1800];
    fm_snprintf(q, sizeof q, "%s %s", L.q_artist, L.q_track);
    net_urlencode(q, qe, sizeof qe);
    fm_snprintf(url, sizeof url, "https://lrclib.net/api/search?q=%s", qe);
    get(url, &body, &len, L.reply_err, sizeof L.reply_err, &L.cancel, &missing);
  }
  L.reply = body;
  L.reply_len = len;
  SDL_AtomicSet(&L.done, 1);
  app_wake();
  return 0;
}

static void stop_fetch(void) {
  if (!L.thr) return;
  L.cancel = 1;
  SDL_WaitThread(L.thr, NULL);
  L.thr = NULL;
  fm_free(L.reply);
  L.reply = NULL;
}

int lyrics_want(const char *artist, const char *title, double duration) {
  char a[200], t[300], key[600];
  lyrics_clean(artist, title, a, sizeof a, t, sizeof t);
  if (!t[0]) return LY_NONE;
  fm_snprintf(key, sizeof key, "%s|%s", a, t);
  if (strcmp(key, L.key)) {                         /* another song */
    stop_fetch();
    clear_lines();
    fm_strlcpy(L.key, key, sizeof L.key);
    L.msg[0] = L.song[0] = 0;
    L.scroll.y = 0;
    char cp[FM_PATH_MAX];
    size_t n = 0;
    char *cached = cache_file(key, cp, sizeof cp) ? read_file(cp, 1u << 20, &n) : NULL;
    if (cached) {
      L.state = n ? lyrics_parse(cached, n) : LY_MISSING;
      if (!n) fm_strlcpy(L.msg, "LRCLIB has no lyrics for this song", sizeof L.msg);
      fm_free(cached);
    } else {
      fm_strlcpy(L.q_artist, a, sizeof L.q_artist);
      fm_strlcpy(L.q_track, t, sizeof L.q_track);
      L.q_dur = duration;
      L.reply_err[0] = 0;
      L.cancel = 0;
      SDL_AtomicSet(&L.done, 0);
      L.state = LY_LOADING;
      L.thr = fm_thread_create(worker, "lyrics", NULL);
      if (!L.thr) { L.state = LY_ERROR; fm_strlcpy(L.msg, "Cannot start a thread", sizeof L.msg); }
    }
  }
  if (L.thr && SDL_AtomicGet(&L.done)) {            /* the fetch is in */
    SDL_WaitThread(L.thr, NULL);
    L.thr = NULL;
    char cp[FM_PATH_MAX];
    if (L.reply) {
      L.state = lyrics_parse(L.reply, L.reply_len);
      if (L.state != LY_ERROR && cache_file(L.key, cp, sizeof cp)) write_file(cp, L.reply, L.reply_len);
    } else if (!L.reply_err[0]) {                   /* nobody has it: remembered as an empty file */
      L.state = LY_MISSING;
      fm_strlcpy(L.msg, "LRCLIB has no lyrics for this song", sizeof L.msg);
      if (cache_file(L.key, cp, sizeof cp)) write_file(cp, "", 0);
    } else {
      L.state = LY_ERROR;
      fm_strlcpy(L.msg, L.reply_err, sizeof L.msg);
    }
    fm_free(L.reply);
    L.reply = NULL;
  }
  return L.state;
}

const char *lyrics_message(void) { return L.msg; }
const char *lyrics_song(void) { return L.song; }

/* ---- drawing -------------------------------------------------------------------------- */

static int index_at(double pos) {
  int lo = 0, hi = L.n - 1, k = -1;
  while (lo <= hi) {
    int m = (lo + hi) / 2;
    if (L.ln[m].t <= pos) { k = m; lo = m + 1; } else hi = m - 1;
  }
  return k;
}

int lyrics_next_ms(double pos) {
  if (L.state != LY_SYNCED || !L.n) return -1;
  int k = index_at(pos);
  if (k + 1 >= L.n) return -1;
  double d = L.ln[k + 1].t - pos;
  return d < 0 ? 0 : (int)(d * 1000.0) + 10;
}

void lyrics_draw(FmRect r, double pos, bool overlay) {
  float fs = overlay ? ui.m.font * 1.15f : ui.m.font * 1.2f, lh = font_line_h(fs) + DP(overlay ? 4 : 10);
  if (L.state == LY_LOADING || L.state == LY_NONE || L.state == LY_MISSING || L.state == LY_ERROR) {
    if (overlay) return;                            /* the video stays clear */
    const char *m = L.state == LY_LOADING ? "Looking for the lyrics\xE2\x80\xA6" : L.msg[0] ? L.msg : "No lyrics";
    if (L.state == LY_LOADING) ui_spinner(FM_RECT(r.x + r.w * 0.5f - DP(12), r.y + r.h * 0.5f - DP(40), DP(24), DP(24)), T.accent);
    font_draw_center(FONT_REGULAR, ui.m.font, r, m, T.text2);
    return;
  }
  if (L.state == LY_PLAIN) {
    if (overlay) return;                            /* no timing: the music player shows them */
    float content = DP(8) + L.n * lh + DP(40);
    ui_scroll(&L.scroll, ui_id("lyrics.plain"), r, content);
    gfx_clip_push(r);
    for (int i = 0; i < L.n; i++) {
      float y = r.y + DP(8) + i * lh - L.scroll.y;
      if (y + lh < r.y || y > r.y + r.h) continue;
      float w = font_width(FONT_REGULAR, fs, L.ln[i].s, -1);
      font_draw(FONT_REGULAR, fs, r.x + FM_MAX(0.0f, (r.w - w) * 0.5f), y, L.ln[i].s, -1, T.text);
    }
    gfx_clip_pop();
    ui_scrollbar(&L.scroll, r, content);
    return;
  }
  int k = index_at(pos);
  if (overlay) {
    /* the line sung and the next, on a dark band at the bottom */
    const char *now = k >= 0 ? L.ln[k].s : "", *nx = k + 1 < L.n ? L.ln[k + 1].s : "";
    if (!now[0] && !nx[0]) return;
    float bh = lh * 2 + DP(14);
    FmRect band = { r.x + r.w * 0.08f, r.y + r.h - bh, r.w * 0.84f, bh };
    gfx_rrect(band, DP(12), FM_RGBA(0, 0, 0, 150));
    float w1 = font_width(FONT_BOLD, fs, now, -1), w2 = font_width(FONT_REGULAR, fs * 0.9f, nx, -1);
    font_draw_ellipsis(FONT_BOLD, fs, band.x + FM_MAX(DP(10), (band.w - w1) * 0.5f), band.y + DP(6), now, band.w - DP(20),
                       FM_HEX(0xFFFFFF));
    font_draw_ellipsis(FONT_REGULAR, fs * 0.9f, band.x + FM_MAX(DP(10), (band.w - w2) * 0.5f), band.y + DP(6) + lh, nx,
                       band.w - DP(20), FM_RGBA(255, 255, 255, 170));
    return;
  }
  /* the list glides so the line sung sits in the middle */
  float target = (float)(k < 0 ? 0 : k) * lh;
  float off = ui_anim(ui_id("lyrics.off"), target, 10.0f);
  gfx_clip_push(r);
  float mid = r.y + r.h * 0.42f;
  for (int i = 0; i < L.n; i++) {
    float y = mid + i * lh - off;
    if (y + lh < r.y || y > r.y + r.h) continue;
    bool cur = i == k;
    float d = fabsf((float)(i - (k < 0 ? 0 : k)));
    FmColor c = cur ? T.accent : col_alpha(T.text, FM_MAX(0.25f, 0.8f - d * 0.12f));
    int face = cur ? FONT_BOLD : FONT_REGULAR;
    const char *s = L.ln[i].s[0] ? L.ln[i].s : "\xE2\x99\xAA";       /* an empty line: a note */
    float w = font_width(face, fs, s, -1);
    font_draw_ellipsis(face, fs, r.x + FM_MAX(0.0f, (r.w - w) * 0.5f), y, s, r.w, c);
  }
  gfx_clip_pop();
}

void lyrics_shutdown(void) {
  stop_fetch();
  clear_lines();
  L.key[0] = 0;
  L.state = LY_NONE;
}
