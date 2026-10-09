/* fvsrc.c -- online video sources: registry, settings snapshot, helper
** programs, the shared download and the cache (see fvsrc.h).
**
** Design decisions:
**   - Helper programs (yt-dlp, deno/node, ffmpeg) are looked up once per
**     search/resolve on the main thread (vsrc_conf_snapshot): the setting,
**     the app folder, the config folder (where "Get yt-dlp" puts it), PATH.
**     Workers only see the resolved paths, never the live settings.
**   - Saving a pair (video + audio) merges with an ffmpeg executable we run
**     ourselves (-c copy, seconds) instead of letting yt-dlp merge: the same
**     code then serves every adapter and reuses files already in the cache.
**     Without ffmpeg the two files are saved side by side and the status
**     line says so.
**   - The cache only ever deletes files directly inside its own folder whose
**     names start with an adapter key ("youtube-..."), oldest first.
*/
#include "fvsrc.h"
#include "fvsrc_int.h"
#include "fconf.h"
#include "fplat.h"
#include "fnet.h"
#include "fproc.h"
#include "fdec_vid.h"
#include "fsdl.h"

#ifdef FM_WIN
#  include "fwin.h"
#  ifdef __TINYC__
WINBASEAPI int WINAPI GetLocaleInfoA(DWORD, DWORD, LPSTR, int);   /* missing from tcc's winnls.h */
#  endif
#  ifndef LOCALE_USER_DEFAULT
#    define LOCALE_USER_DEFAULT 0x0400
#  endif
#  ifndef LOCALE_SISO3166CTRYNAME
#    define LOCALE_SISO3166CTRYNAME 0x5A
#  endif
#endif

/* ---- registry ----------------------------------------------------------------- */

static const FmVsrc *const kSources[] = {
  &g_vsrc_youtube, &g_vsrc_archive, &g_vsrc_peertube, &g_vsrc_dailymotion, &g_vsrc_bilibili, &g_vsrc_web,
};

int vsrc_count(void) { return FM_COUNT(kSources); }

const FmVsrc *vsrc_at(int i) { return i >= 0 && i < FM_COUNT(kSources) ? kSources[i] : NULL; }

const FmVsrc *vsrc_find(const char *key) {
  if (!key) return NULL;
  for (int i = 0; i < FM_COUNT(kSources); i++)
    if (!strcmp(kSources[i]->key, key)) return kSources[i];
  return NULL;
}

/* ---- pages -------------------------------------------------------------------- */

void vsrc_page_free(FmVsrcPage *p) {
  if (!p) return;
  fm_free(p->items);
  memset(p, 0, sizeof *p);
}

FmVsrcItem *vsrc_page_add(FmVsrcPage *p) {
  if (p->count == p->cap) {
    /* a page normally fits the first allocation exactly */
    p->cap = p->cap ? p->cap * 2 : VSRC_PAGE_SIZE;
    p->items = (FmVsrcItem *)fm_realloc(p->items, (size_t)p->cap * sizeof *p->items);
  }
  FmVsrcItem *it = &p->items[p->count++];
  memset(it, 0, sizeof *it);
  it->views = -1;
  return it;
}

/* ---- small text helpers ---------------------------------------------------------- */

/* Unsigned decimal with an optional fraction, independent of the C locale.
** Returns chars used, 0 when s does not start with a digit. */
static int parse_num(const char *s, double *v) {
  int i = 0;
  double x = 0;
  if (s[0] < '0' || s[0] > '9') return 0;
  while (s[i] >= '0' && s[i] <= '9' && i < 18) x = x * 10 + (s[i++] - '0');
  while (s[i] >= '0' && s[i] <= '9') i++;               /* absurdly long: ignore the rest */
  if (s[i] == '.' || s[i] == ',') {
    double f = 0.1;
    i++;
    while (s[i] >= '0' && s[i] <= '9') { x += (s[i++] - '0') * f; f *= 0.1; }
  }
  *v = x;
  return i;
}

/* Drops an incomplete UTF-8 sequence at the end (left by truncation). */
static void utf8_cut_tail(char *s) {
  size_t n = strlen(s), i = n;
  int back = 0;
  while (i > 0 && back < 4 && ((u8)s[i - 1] & 0xC0) == 0x80) { i--; back++; }
  if (i == 0) { if (back) s[0] = 0; return; }
  u8 lead = (u8)s[i - 1];
  int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
  if (lead >= 0x80 && need != back + 1) s[i - 1] = 0;
}

double vsrc_iso_duration(const char *s) {
  if (!s || (s[0] != 'P' && s[0] != 'p')) return 0;
  double total = 0;
  bool in_time = false, any = false;
  for (s++; *s;) {
    if (*s == 'T' || *s == 't') { in_time = true; s++; continue; }
    double v;
    int n = parse_num(s, &v);
    if (!n) return 0;
    s += n;
    switch (*s) {
    case 'Y': if (in_time) return 0; total += v * 365 * 86400; break;
    case 'W': if (in_time) return 0; total += v * 7 * 86400; break;
    case 'D': if (in_time) return 0; total += v * 86400; break;
    case 'M': total += in_time ? v * 60 : v * 30 * 86400; break;
    case 'H': if (!in_time) return 0; total += v * 3600; break;
    case 'S': if (!in_time) return 0; total += v; break;
    default: return 0;
    }
    s++;
    any = true;
  }
  return any ? total : 0;
}

double vsrc_clock_seconds(const char *s) {
  if (!s) return 0;
  double total = 0;
  int parts = 0;
  for (;;) {
    double v;
    int n = parse_num(s, &v);
    if (!n || ++parts > 3) return 0;
    total = total * 60 + v;
    s += n;
    if (*s == ':') { s++; continue; }
    return *s ? 0 : total;
  }
}

void vsrc_html_unescape(const char *s, char *out, size_t cap) {
  static const struct { const char *name; char c; } kEnt[] = {
    { "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' }, { "quot;", '"' }, { "apos;", '\'' },
  };
  size_t o = 0;
  if (!cap) return;
  while (s && *s && o + 1 < cap) {
    if (*s == '&') {
      bool done = false;
      for (int i = 0; i < FM_COUNT(kEnt) && !done; i++) {
        size_t n = strlen(kEnt[i].name);
        if (!strncmp(s + 1, kEnt[i].name, n)) { out[o++] = kEnt[i].c; s += 1 + n; done = true; }
      }
      if (!done && s[1] == '#') {
        const char *p = s + 2;
        bool hex = (*p == 'x' || *p == 'X');
        u32 cp = 0;
        int digits = 0;
        if (hex) p++;
        for (; digits < 7; p++, digits++) {
          int d = (*p >= '0' && *p <= '9') ? *p - '0'
                : (hex && *p >= 'a' && *p <= 'f') ? *p - 'a' + 10
                : (hex && *p >= 'A' && *p <= 'F') ? *p - 'A' + 10 : -1;
          if (d < 0) break;
          cp = cp * (hex ? 16 : 10) + (u32)d;
        }
        if (digits && *p == ';' && cp && cp < 0x110000 && (cp < 0xD800 || cp > 0xDFFF)) {
          char u[4];
          int n = utf8_encode(cp, u);
          if (o + (size_t)n + 1 > cap) break;
          memcpy(out + o, u, (size_t)n);
          o += (size_t)n;
          s = p + 1;
          done = true;
        }
      }
      if (done) continue;
    }
    out[o++] = *s++;
  }
  out[o] = 0;
  utf8_cut_tail(out);
}

void vsrc_unix_date(i64 t, char *out, size_t cap) {
  if (t <= 0) { if (cap) out[0] = 0; return; }
  /* days to civil date (H. Hinnant), valid for any positive stamp */
  i64 z = t / 86400 + 719468;
  i64 era = z / 146097;
  i64 doe = z - era * 146097;
  i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  i64 y = yoe + era * 400;
  i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  i64 mp = (5 * doy + 2) / 153;
  int d = (int)(doy - (153 * mp + 2) / 5 + 1);
  int m = (int)(mp < 10 ? mp + 3 : mp - 9);
  if (m <= 2) y++;
  fm_snprintf(out, cap, "%04d-%02d-%02d", (int)y, m, d);
}

void vsrc_iso_date(const char *s, char *out, size_t cap) {
  if (!cap) return;
  out[0] = 0;
  if (!s) return;
  for (int i = 0; i < 10; i++) {
    bool dash = (i == 4 || i == 7);
    if (dash ? s[i] != '-' : (s[i] < '0' || s[i] > '9')) return;
  }
  fm_snprintf(out, cap, "%.10s", s);
}

const char *vsrc_jstr(const FmJsonNode *n, const char *def) {
  if (n && n->type == JSON_ARR) n = json_first(n);
  return json_str(n, def);
}

bool vsrc_id_ok(const char *s, const char *extra, size_t max) {
  size_t n = 0;
  if (!s || !*s) return false;
  for (; s[n]; n++) {
    char c = s[n];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              (extra && strchr(extra, c));
    if (!ok || n >= max) return false;
  }
  return true;
}

bool vsrc_url_ok(const char *s) {
  if (!s || (strncmp(s, "https://", 8) && strncmp(s, "http://", 7))) return false;
  for (; *s; s++)
    if ((u8)*s <= ' ' || *s == 0x7F) return false;
  return true;
}

void vsrc_safe_name(const char *title, char *out, size_t max) {
  size_t o = 0;
  if (!max) return;
  for (const char *s = title ? title : ""; *s && o + 1 < max; s++) {
    u8 c = (u8)*s;
    if (c < 0x20 || strchr("<>:\"/\\|?*", c)) c = '_';
    if (c == ' ' && (o == 0 || out[o - 1] == ' ')) continue;
    out[o++] = (char)c;
  }
  out[o] = 0;
  utf8_cut_tail(out);
  o = strlen(out);
  while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '.')) o--;
  out[o] = 0;
}

/* ---- network ------------------------------------------------------------------ */

bool vsrc_need_net(char *err, size_t cap) {
  if (net_available()) return true;
#if defined(FM_ANDROID) || defined(FM_WEB)
  fm_strlcpy(err, "Online videos are not available on this platform yet", cap);
#else
  fm_snprintf(err, cap, "No internet support on this system (%s)", net_backend());
#endif
  return false;
}

FmErr vsrc_http_get(const char *url, FmNetResp *r, char *err, size_t errcap, volatile int *cancel) {
  memset(r, 0, sizeof *r);
  if (!vsrc_need_net(err, errcap)) return FM_ERR_UNSUPPORTED;
  FmErr e = net_get(url, NULL, VSRC_MAX_REPLY, r, cancel);
  if (e == FM_ERR_CANCEL) { fm_strlcpy(err, "Cancelled", errcap); return e; }
  if (e != FM_OK) {
    fm_snprintf(err, errcap, "Network error: %s", r->error[0] ? r->error : fm_err_str(e));
    net_resp_free(r);
    return e;
  }
  return FM_OK;
}

void vsrc_http_error(const char *site, int status, char *err, size_t cap) {
  if (status == 429) fm_snprintf(err, cap, "%s: too many requests, wait a minute and try again", site);
  else if (status >= 500)
    fm_snprintf(err, cap, "%s is having trouble (server error %d), try again later", site, status);
  else if (status && (status < 200 || status >= 300))
    fm_snprintf(err, cap, "%s refused the request (HTTP %d)", site, status);
  else fm_snprintf(err, cap, "%s sent a reply this app does not understand", site);
}

/* ---- finding helper programs ------------------------------------------------------ */

static bool is_file(const char *p) {
  FmStat st;
  return p && *p && plat_stat(p, &st) && !(st.flags & FM_ST_DIR);
}

#if !defined(FM_ANDROID) && !defined(FM_WEB)
/* PATH as UTF-8 (wide on Windows so non-ASCII folders survive). */
static char *env_path(void) {
#  ifdef FM_WIN
  DWORD n = GetEnvironmentVariableW(L"PATH", NULL, 0);
  if (!n || n > 65536) return NULL;
  wchar_t *w = (wchar_t *)fm_alloc((size_t)n * sizeof(wchar_t));
  char *out = NULL;
  if (GetEnvironmentVariableW(L"PATH", w, n)) {
    int m = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (m > 0) {
      out = (char *)fm_alloc((size_t)m);
      WideCharToMultiByte(CP_UTF8, 0, w, -1, out, m, NULL, NULL);
    }
  }
  fm_free(w);
  return out;
#  else
  const char *p = getenv("PATH");
  return p ? fm_strdup(p) : NULL;
#  endif
}
#endif

/* name is without ".exe"; tries the app folder, the config folder, extra, PATH */
static bool find_prog(const char *name, const char *extra_dir, char *out, size_t cap) {
#if defined(FM_ANDROID) || defined(FM_WEB)
  FM_UNUSED(name); FM_UNUSED(extra_dir); FM_UNUSED(out); FM_UNUSED(cap);
  return false;
#else
  char file[96], dir[FM_PATH_MAX], p[FM_PATH_MAX];
#  ifdef FM_WIN
  fm_snprintf(file, sizeof file, "%s.exe", name);
#  else
  fm_strlcpy(file, name, sizeof file);
#  endif
  char *base = SDL_GetBasePath();
  if (base) {
    fm_path_join(p, sizeof p, base, file);
    SDL_free(base);
    if (is_file(p)) { fm_strlcpy(out, p, cap); return true; }
  }
  if (plat_place(PLACE_CONFIG, dir, sizeof dir) && fm_path_join(p, sizeof p, dir, file) && is_file(p)) {
    fm_strlcpy(out, p, cap);
    return true;
  }
  if (extra_dir && fm_path_join(p, sizeof p, extra_dir, file) && is_file(p)) {
    fm_strlcpy(out, p, cap);
    return true;
  }
  char *path = env_path();
  bool found = false;
#  ifdef FM_WIN
  const char sep = ';';
#  else
  const char sep = ':';
#  endif
  for (char *s = path; s && *s && !found;) {
    char *e = strchr(s, sep);
    size_t n = e ? (size_t)(e - s) : strlen(s);
    if (n && n < sizeof dir) {
      memcpy(dir, s, n);
      dir[n] = 0;
      if (dir[0] == '"' && n > 2 && dir[n - 1] == '"') { memmove(dir, dir + 1, n - 2); dir[n - 2] = 0; }
      if (fm_path_join(p, sizeof p, dir, file) && is_file(p)) { fm_strlcpy(out, p, cap); found = true; }
    }
    s = e ? e + 1 : s + n;
  }
  fm_free(path);
  return found;
#endif
}

bool vsrc_find_ytdlp(char *out, size_t cap) {
  out[0] = 0;
  if (!proc_available()) return false;
  if (conf.ytdlp_path[0]) {
    char p[FM_PATH_MAX];
    if (is_file(conf.ytdlp_path)) { fm_strlcpy(out, conf.ytdlp_path, cap); return true; }
#ifdef FM_WIN
    if (fm_path_join(p, sizeof p, conf.ytdlp_path, "yt-dlp.exe") && is_file(p)) {
#else
    if (fm_path_join(p, sizeof p, conf.ytdlp_path, "yt-dlp") && is_file(p)) {
#endif
      fm_strlcpy(out, p, cap);
      return true;
    }
  }
  if (find_prog("yt-dlp", NULL, out, cap)) return true;
#if defined(FM_MACOS)
  if (find_prog("yt-dlp_macos", NULL, out, cap)) return true;
#elif defined(FM_LINUX)
  if (find_prog("yt-dlp_linux", NULL, out, cap)) return true;
#endif
  return false;
}

/* "node:<path>" from a path, judging the runtime by its file name */
static bool runtime_spec(const char *path, char *out, size_t cap) {
  static const char *const kNames[] = { "deno", "node", "bun", "qjs" };
  static const char *const kKeys[] = { "deno", "node", "bun", "quickjs" };
  const char *b = fm_path_base(path);
  for (int i = 0; i < FM_COUNT(kNames); i++)
    if (!fm_strnicmp(b, kNames[i], strlen(kNames[i]))) {
      fm_snprintf(out, cap, "%s:%s", kKeys[i], path);
      return true;
    }
  return false;
}

bool vsrc_find_js_runtime(char *out, size_t cap) {
  out[0] = 0;
  if (!proc_available()) return false;
  const char *s = conf.js_runtime;
  if (s[0]) {
    static const char *const kPre[] = {
        "deno:", "node:", "bun:", "quickjs:", "deno", "node", "bun", "quickjs"};
    for (int i = 0; i < FM_COUNT(kPre); i++)
      if (!strcmp(s, kPre[i]) || (strchr(kPre[i], ':') && !strncmp(s, kPre[i], strlen(kPre[i])))) {
        fm_strlcpy(out, s, cap);    /* already in yt-dlp's form ("node" alone = from PATH) */
        return true;
      }
    if (is_file(s) && runtime_spec(s, out, cap)) return true;
  }
  char home[FM_PATH_MAX], deno_dir[FM_PATH_MAX], p[FM_PATH_MAX];
  deno_dir[0] = 0;
  if (plat_place(PLACE_HOME, home, sizeof home)) {
    fm_path_join(deno_dir, sizeof deno_dir, home, ".deno");
    fm_path_join(deno_dir, sizeof deno_dir, deno_dir, "bin");   /* the deno installer's default */
  }
  if (find_prog("deno", deno_dir[0] ? deno_dir : NULL, p, sizeof p)) {
    fm_snprintf(out, cap, "deno:%s", p);
    return true;
  }
  if (find_prog("node", NULL, p, sizeof p)) {
    fm_snprintf(out, cap, "node:%s", p);
    return true;
  }
  return false;
}

/* ffmpeg's folder: the setting (a folder or the exe itself), then the usual places */
static bool find_ffmpeg_dir(char *out, size_t cap) {
  char p[FM_PATH_MAX];
  out[0] = 0;
  if (!proc_available()) return false;
#ifdef FM_WIN
  const char *exe = "ffmpeg.exe";
#else
  const char *exe = "ffmpeg";
#endif
  if (conf.ffmpeg_dir[0]) {
    if (is_file(conf.ffmpeg_dir)) {
      fm_strlcpy(out, conf.ffmpeg_dir, cap);
      fm_path_parent(out);
      return true;
    }
    if (fm_path_join(p, sizeof p, conf.ffmpeg_dir, exe) && is_file(p)) {
      fm_strlcpy(out, conf.ffmpeg_dir, cap);
      return true;
    }
    fm_path_join(p, sizeof p, conf.ffmpeg_dir, "bin");
    if (fm_path_join(p, sizeof p, p, exe) && is_file(p)) {
      fm_strlcpy(out, p, cap);
      fm_path_parent(out);
      return true;
    }
  }
  if (!find_prog("ffmpeg", NULL, p, sizeof p)) return false;
  fm_strlcpy(out, p, cap);
  fm_path_parent(out);
  return true;
}

bool vsrc_ffmpeg_exe(const FmVsrcConf *c, char *out, size_t cap) {
  out[0] = 0;
  if (!c->ffmpeg_dir[0] || !proc_available()) return false;
#ifdef FM_WIN
  fm_path_join(out, cap, c->ffmpeg_dir, "ffmpeg.exe");
#else
  fm_path_join(out, cap, c->ffmpeg_dir, "ffmpeg");
#endif
  if (is_file(out)) return true;
  out[0] = 0;
  return false;
}

static void user_region(char *out, size_t cap) {
  char buf[16];
  buf[0] = 0;
#ifdef FM_WIN
  if (!GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_SISO3166CTRYNAME, buf, (int)sizeof buf)) buf[0] = 0;
#elif defined(FM_ANDROID)
  /* no LANG there: the phone's language setting ("en_PH") through SDL */
  SDL_Locale *loc = SDL_GetPreferredLocales();
  if (loc) {
    if (loc[0].language && loc[0].country) fm_strlcpy(buf, loc[0].country, sizeof buf);
    SDL_free(loc);
  }
#else
  const char *l = getenv("LC_ALL");
  if (!l || !*l) l = getenv("LC_MESSAGES");
  if (!l || !*l) l = getenv("LANG");
  const char *u = l ? strchr(l, '_') : NULL;
  if (u) fm_snprintf(buf, sizeof buf, "%.2s", u + 1);
#endif
  if (strlen(buf) == 2 && buf[0] >= 'A' && buf[0] <= 'Z' && buf[1] >= 'A' && buf[1] <= 'Z')
    fm_strlcpy(out, buf, cap);
  else
    out[0] = 0;
}

void vsrc_conf_snapshot(FmVsrcConf *c) {
  memset(c, 0, sizeof *c);
  /* keys are pasted: drop surrounding blanks */
  const char *k = conf.yt_api_key;
  while (*k == ' ' || *k == '\t') k++;
  fm_strlcpy(c->api_key_youtube, k, sizeof c->api_key_youtube);
  size_t n = strlen(c->api_key_youtube);
  while (n && (u8)c->api_key_youtube[n - 1] <= ' ') c->api_key_youtube[--n] = 0;
  vsrc_find_ytdlp(c->ytdlp, sizeof c->ytdlp);
  vsrc_find_js_runtime(c->js_runtime, sizeof c->js_runtime);
  find_ffmpeg_dir(c->ffmpeg_dir, sizeof c->ffmpeg_dir);
  char dir[FM_PATH_MAX];
  if (plat_place(PLACE_CACHE, dir, sizeof dir) || plat_place(PLACE_TEMP, dir, sizeof dir))
    fm_path_join(c->cache_dir, sizeof c->cache_dir, dir, "online");
  if (conf.online_dl_dir[0]) fm_strlcpy(c->download_dir, conf.online_dl_dir, sizeof c->download_dir);
  else if (!plat_place(PLACE_DOWNLOADS, c->download_dir, sizeof c->download_dir))
    plat_place(PLACE_HOME, c->download_dir, sizeof c->download_dir);
  c->max_height = conf.online_height >= 144 && conf.online_height <= 4320 ? conf.online_height : 720;
  c->have_ffmpeg_libs = ff_available();
#if defined(FM_ANDROID)
  /* MediaCodec streams through FmNetStream from Android 9; before that
  ** URLs go to the built-in decoder (VP9/Opus WebM only) */
  c->os_mp4 = c->os_dash = vid_os_streams();
#elif defined(FM_WIN) && !defined(__TINYC__)
  c->os_mp4 = true;          /* Media Foundation (fdec_vid_int.h) */
#endif
  /* send_headers stays false: vid_open has no headers argument yet, so
  ** formats that need a Referer/cookie go through the cache instead */
  user_region(c->region, sizeof c->region);
  c->safe_search = conf.online_safe;
}

/* ---- saving streams -------------------------------------------------------------- */

typedef struct Prog {
  FmVsrcProgress cb;
  void *user;
  const char *what;          /* "Downloading", "Copying" */
  float base, span;          /* overall fraction = base + span * part */
  u64 last_ms;
} Prog;

static bool prog_report(Prog *p, u64 done, u64 total) {
  if (!p->cb) return true;
  u64 now = plat_now_ms();
  if (now - p->last_ms < 150 && done != total) return true;   /* about 7 updates a second */
  p->last_ms = now;
  char status[96], a[24];
  float f = total ? (float)((double)done / (double)total) : -1.0f;
  if (f >= 0)
    fm_snprintf(status, sizeof status, "%s %d%% \xC2\xB7 %s", p->what, (int)(f * 100),
                fm_fmt_size(done, a, sizeof a));
  else fm_snprintf(status, sizeof status, "%s %s", p->what, fm_fmt_size(done, a, sizeof a));
  return p->cb(p->user, f >= 0 ? p->base + p->span * f : -1.0f, status);
}

static bool net_prog(void *user, u64 done, u64 total) { return prog_report((Prog *)user, done, total); }

static FmErr copy_file(const char *from, const char *to, Prog *pg, volatile int *cancel) {
  FILE *in = fm_fopen(from, "rb");
  if (!in) return FM_ERR_NOT_FOUND;
  char part[FM_PATH_MAX];
  fm_snprintf(part, sizeof part, "%s.part", to);
  FILE *out = fm_fopen(part, "wb");
  if (!out) { fclose(in); return FM_ERR_IO; }
  i64 total = fm_fsize(in);
  u64 done = 0;
  size_t bufsz = 256 * 1024;
  u8 *buf = (u8 *)fm_alloc(bufsz);
  FmErr e = FM_OK;
  for (;;) {
    if (cancel && *cancel) { e = FM_ERR_CANCEL; break; }
    size_t n = fread(buf, 1, bufsz, in);
    if (!n) { if (ferror(in)) e = FM_ERR_IO; break; }
    if (fwrite(buf, 1, n, out) != n) { e = FM_ERR_FULL; break; }
    done += n;
    if (!prog_report(pg, done, total > 0 ? (u64)total : 0)) { e = FM_ERR_CANCEL; break; }
  }
  fm_free(buf);
  fclose(in);
  if (fclose(out) != 0 && e == FM_OK) e = FM_ERR_IO;
  if (e == FM_OK) {
    plat_remove_file(to);
    e = plat_rename(part, to);
  }
  if (e != FM_OK) plat_remove_file(part);
  return e;
}

/* ".webm" from a path or URL (query and fragment ignored); def when odd */
static void media_ext(const char *s, const char *def, char *out, size_t cap) {
  char tmp[FM_PATH_MAX];
  fm_strlcpy(tmp, s, sizeof tmp);
  tmp[strcspn(tmp, "?#")] = 0;
  const char *e = fm_path_ext(tmp);
  bool ok = e[0] == '.' && strlen(e) >= 2 && strlen(e) <= 6 && vsrc_id_ok(e + 1, NULL, 5);
  fm_strlcpy(out, ok ? e : def, cap);
  for (char *p = out; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
}

/* dir/base+tag+ext, adding " (2)", " (3)" ... when taken */
static void unique_path(const char *dir, const char *base, const char *tag, const char *ext, char *out,
                        size_t cap) {
  char name[512];
  for (int i = 1; i < 100; i++) {
    if (i == 1) fm_snprintf(name, sizeof name, "%s%s%s", base, tag, ext);
    else fm_snprintf(name, sizeof name, "%s (%d)%s%s", base, i, tag, ext);
    fm_path_join(out, cap, dir, name);
    if (!plat_exists(out)) return;
  }
}

typedef struct LastErr { char text[256]; } LastErr;

static bool keep_last_err(void *user, const char *line, bool is_err) {
  LastErr *l = (LastErr *)user;
  if (is_err && line[0]) fm_strlcpy(l->text, line, sizeof l->text);
  return true;
}

/* ffmpeg -c copy of picture + sound into one file */
static FmErr merge_pair(const char *ffmpeg, const char *v, const char *a, const char *dest, char *err,
                        size_t errcap, volatile int *cancel) {
  const char *ext = fm_path_ext(dest);
  const char *mux = !fm_stricmp(ext, ".webm") ? "webm" : !fm_stricmp(ext, ".mp4") ? "mp4" : "matroska";
  char part[FM_PATH_MAX];
  fm_snprintf(part, sizeof part, "%s.part", dest);
  const char *argv[24];
  int n = 0;
  argv[n++] = ffmpeg;
  argv[n++] = "-hide_banner"; argv[n++] = "-nostdin"; argv[n++] = "-loglevel"; argv[n++] = "error";
  argv[n++] = "-y"; argv[n++] = "-i"; argv[n++] = v; argv[n++] = "-i"; argv[n++] = a;
  argv[n++] = "-map"; argv[n++] = "0:v:0"; argv[n++] = "-map"; argv[n++] = "1:a:0";
  argv[n++] = "-c"; argv[n++] = "copy";
  if (!strcmp(mux, "mp4")) { argv[n++] = "-movflags"; argv[n++] = "+faststart"; }
  argv[n++] = "-f"; argv[n++] = mux; argv[n++] = part;
  argv[n] = NULL;
  LastErr le;
  le.text[0] = 0;
  int code = -1;
  FmErr e = proc_run(argv, keep_last_err, &le, &code, cancel);
  if (e == FM_OK && code != 0) e = FM_ERR_FORMAT;
  if (e == FM_OK) {
    plat_remove_file(dest);
    e = plat_rename(part, dest);
  }
  if (e != FM_OK) {
    plat_remove_file(part);
    if (e != FM_ERR_CANCEL)
      fm_snprintf(err, errcap, "ffmpeg could not merge: %s", le.text[0] ? le.text : fm_err_str(e));
  }
  return e;
}

/* local file, or a URL fetched into tmp_dir; *fetched says to delete it after */
static FmErr local_copy_of(const char *src, bool local, const char *tmp_path, Prog *pg, char *out, size_t cap,
                           bool *fetched, char *err, size_t errcap, volatile int *cancel) {
  *fetched = false;
  if (local) { fm_strlcpy(out, src, cap); return FM_OK; }
  FmNetResp r;
  memset(&r, 0, sizeof r);
  FmErr e = net_download(src, NULL, tmp_path, net_prog, pg, &r, cancel);
  if (e != FM_OK) {
    if (e != FM_ERR_CANCEL)
      fm_snprintf(err, errcap, "Download failed: %s", r.error[0] ? r.error : fm_err_str(e));
    return e;
  }
  fm_strlcpy(out, tmp_path, cap);
  *fetched = true;
  return FM_OK;
}

FmErr vsrc_save_stream(const FmVsrcConf *c, const FmVsrcItem *item, const FmVsrcStream *st, const char *dir,
                       char *out_path, size_t cap, FmVsrcProgress cb, void *user, char *err, size_t errcap,
                       volatile int *cancel) {
  char title[160], base[256], ext[16], aext[16];
  out_path[0] = 0;
  if (plat_mkdirs(dir) != FM_OK && !plat_is_dir(dir)) {
    fm_snprintf(err, errcap, "Cannot create the folder %s", dir);
    return FM_ERR_IO;
  }
  vsrc_safe_name(item->title[0] ? item->title : "video", title, 151);
  if (vsrc_id_ok(item->id, "-_.", 63)) fm_snprintf(base, sizeof base, "%s [%s]", title, item->id);
  else fm_strlcpy(base, title[0] ? title : "video", sizeof base);
  media_ext(st->video, ".mp4", ext, sizeof ext);
  Prog pg;
  memset(&pg, 0, sizeof pg);
  pg.cb = cb;
  pg.user = user;
  pg.what = st->local ? "Saving" : "Downloading";
  pg.span = 1;

  char dest[FM_PATH_MAX];
  if (!st->audio[0]) {                                    /* one file */
    unique_path(dir, base, "", ext, dest, sizeof dest);
    FmErr e;
    if (st->local) {
      e = copy_file(st->video, dest, &pg, cancel);
      if (e != FM_OK && e != FM_ERR_CANCEL) fm_snprintf(err, errcap, "Could not save: %s", fm_err_str(e));
    } else {
      FmNetResp r;
      memset(&r, 0, sizeof r);
      e = net_download(st->video, NULL, dest, net_prog, &pg, &r, cancel);
      if (e != FM_OK && e != FM_ERR_CANCEL)
        fm_snprintf(err, errcap, "Download failed: %s", r.error[0] ? r.error : fm_err_str(e));
    }
    if (e == FM_OK) fm_strlcpy(out_path, dest, cap);
    if (e == FM_ERR_CANCEL) fm_strlcpy(err, "Cancelled", errcap);
    return e;
  }

  media_ext(st->audio, ".m4a", aext, sizeof aext);
  char ffmpeg[FM_PATH_MAX];
  if (vsrc_ffmpeg_exe(c, ffmpeg, sizeof ffmpeg)) {       /* merge into one file */
    char v[FM_PATH_MAX], a[FM_PATH_MAX], tv[FM_PATH_MAX], ta[FM_PATH_MAX];
    bool fv = false, fa = false;
    fm_snprintf(tv, sizeof tv, "%s%smerge-%llu.v%s", c->cache_dir, FM_SEP_STR,
                (unsigned long long)plat_now_ms(), ext);
    fm_snprintf(ta, sizeof ta, "%s%smerge-%llu.a%s", c->cache_dir, FM_SEP_STR,
                (unsigned long long)plat_now_ms(), aext);
    if (!st->local) plat_mkdirs(c->cache_dir);
    pg.span = 0.5f;
    FmErr e = local_copy_of(st->video, st->local, tv, &pg, v, sizeof v, &fv, err, errcap, cancel);
    pg.base = 0.5f;
    if (e == FM_OK) e = local_copy_of(st->audio, st->local, ta, &pg, a, sizeof a, &fa, err, errcap, cancel);
    if (e == FM_OK) {
      const char *mext = (!strcmp(ext, ".webm") && !strcmp(aext, ".webm")) ? ".webm"
                       : (!strcmp(ext, ".mp4") && (!strcmp(aext, ".m4a") || !strcmp(aext, ".mp4"))) ? ".mp4"
                       : ".mkv";
      unique_path(dir, base, "", mext, dest, sizeof dest);
      if (cb && !cb(user, -1.0f, "Merging video and audio\xE2\x80\xA6")) e = FM_ERR_CANCEL;
      if (e == FM_OK) e = merge_pair(ffmpeg, v, a, dest, err, errcap, cancel);
    }
    if (fv) plat_remove_file(tv);
    if (fa) plat_remove_file(ta);
    if (e == FM_OK) {
      fm_strlcpy(out_path, dest, cap);
      if (cb) cb(user, 1.0f, "Saved");
      return FM_OK;
    }
    if (e == FM_ERR_CANCEL) { fm_strlcpy(err, "Cancelled", errcap); return e; }
    if (!st->local) return e;
    /* a failed merge of local files still leaves the two-file fallback */
    err[0] = 0;
  }

  /* no ffmpeg: video and sound side by side */
  char adest[FM_PATH_MAX];
  unique_path(dir, base, ".video", ext, dest, sizeof dest);
  unique_path(dir, base, ".audio", aext, adest, sizeof adest);
  pg.base = 0;
  pg.span = 0.5f;
  FmErr e;
  FmNetResp r;
  memset(&r, 0, sizeof r);
  if (st->local) e = copy_file(st->video, dest, &pg, cancel);
  else e = net_download(st->video, NULL, dest, net_prog, &pg, &r, cancel);
  pg.base = 0.5f;
  if (e == FM_OK) {
    if (st->local) e = copy_file(st->audio, adest, &pg, cancel);
    else e = net_download(st->audio, NULL, adest, net_prog, &pg, &r, cancel);
    if (e != FM_OK) plat_remove_file(dest);
  }
  if (e == FM_ERR_CANCEL) { fm_strlcpy(err, "Cancelled", errcap); return e; }
  if (e != FM_OK) {
    fm_snprintf(err, errcap, "Could not save: %s", r.error[0] ? r.error : fm_err_str(e));
    return e;
  }
  fm_strlcpy(out_path, dest, cap);
  if (cb) cb(user, 1.0f, "Saved as 2 files (video + audio) \xE2\x80\x94 install FFmpeg to merge");
  return FM_OK;
}

FmErr vsrc_download(const FmVsrc *s, const FmVsrcConf *c, const FmVsrcItem *item, const char *dir,
                    char *out_path, size_t cap, FmVsrcProgress cb, void *user, char *err, size_t errcap,
                    volatile int *cancel) {
  if (cap) out_path[0] = 0;
  if (errcap) err[0] = 0;
  if (!s || !s->resolve || !item) {
    fm_strlcpy(err, "This source cannot download", errcap);
    return FM_ERR_UNSUPPORTED;
  }
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
  FmErr e = s->resolve(c, item, st, cb, user, err, errcap, cancel);
  if (e == FM_OK)
    e = vsrc_save_stream(c, item, st, dir && *dir ? dir : c->download_dir, out_path, cap, cb, user, err,
                         errcap, cancel);
  fm_free(st);
  return e;
}

/* ---- cache ----------------------------------------------------------------------- */

typedef struct CacheEnt { char name[120]; u64 size; i64 mtime; } CacheEnt;

static int cache_cmp(const void *a, const void *b) {
  const CacheEnt *x = (const CacheEnt *)a, *y = (const CacheEnt *)b;
  return x->mtime < y->mtime ? -1 : x->mtime > y->mtime;
}

static bool cache_name_ours(const char *name) {
  if (!strncmp(name, "merge-", 6)) return true;
  for (int i = 0; i < FM_COUNT(kSources); i++) {
    size_t n = strlen(kSources[i]->key);
    if (!strncmp(name, kSources[i]->key, n) && name[n] == '-') return true;
  }
  return false;
}

void vsrc_cache_trim(const char *cache_dir, u64 max_bytes, const char *keep) {
  if (!cache_dir || !*cache_dir) return;
  FmErr e;
  FmDir *d = plat_dir_open(cache_dir, &e);
  if (!d) return;
  int cap = 256, n = 0;
  CacheEnt *v = (CacheEnt *)fm_alloc((size_t)cap * sizeof *v);
  u64 total = 0;
  const char *name;
  FmStat st;
  i64 now = plat_time_unix();
  while (plat_dir_next(d, &name, &st)) {
    if (st.flags & FM_ST_DIR) continue;
    total += st.size;
    if (!cache_name_ours(name) || strlen(name) >= sizeof v->name) continue;
    if (keep && *keep && !strncmp(name, keep, strlen(keep))) continue;
    if (strstr(name, ".part") && st.mtime > now - 600) continue;   /* maybe still downloading */
    if (n == cap) {
      if (cap >= 8192) continue;
      cap *= 2;
      v = (CacheEnt *)fm_realloc(v, (size_t)cap * sizeof *v);
    }
    fm_strlcpy(v[n].name, name, sizeof v[n].name);
    v[n].size = st.size;
    v[n].mtime = st.mtime;
    n++;
  }
  plat_dir_close(d);
  qsort(v, (size_t)n, sizeof *v, cache_cmp);
  char p[FM_PATH_MAX];
  for (int i = 0; i < n && total > max_bytes; i++) {
    if (fm_path_join(p, sizeof p, cache_dir, v[i].name) && plat_remove_file(p) == FM_OK)
      total -= v[i].size < total ? v[i].size : total;
  }
  fm_free(v);
}

/* ---- installing yt-dlp ----------------------------------------------------------- */

typedef struct InstProg { FmVsrcProgress cb; void *user; u64 last_ms; } InstProg;

static bool inst_prog(void *user, u64 done, u64 total) {
  InstProg *p = (InstProg *)user;
  if (!p->cb) return true;
  u64 now = plat_now_ms();
  if (now - p->last_ms < 150 && done != total) return true;
  p->last_ms = now;
  char status[96], a[24];
  float f = total ? (float)((double)done / (double)total) : -1.0f;
  if (f >= 0) fm_snprintf(status, sizeof status, "Downloading yt-dlp %d%% \xC2\xB7 %s", (int)(f * 100),
                          fm_fmt_size(done, a, sizeof a));
  else fm_snprintf(status, sizeof status, "Downloading yt-dlp %s", fm_fmt_size(done, a, sizeof a));
  return p->cb(p->user, f, status);
}

FmErr vsrc_install_ytdlp(char *out, size_t cap, FmVsrcProgress cb, void *user, volatile int *cancel) {
  char dir[FM_PATH_MAX], path[FM_PATH_MAX], err[160];
  if (cap) out[0] = 0;
  if (!proc_available() || !vsrc_need_net(err, sizeof err)) return FM_ERR_UNSUPPORTED;
  if (!plat_place(PLACE_CONFIG, dir, sizeof dir)) return FM_ERR_NOT_FOUND;
  plat_mkdirs(dir);
  /* the standalone builds need no Python on the machine */
#if defined(FM_WIN)
  const char *asset = "yt-dlp.exe", *name = "yt-dlp.exe";
#elif defined(FM_MACOS)
  const char *asset = "yt-dlp_macos", *name = "yt-dlp";
#elif defined(FM_LINUX) && defined(FM_X64)
  const char *asset = "yt-dlp_linux", *name = "yt-dlp";
#elif defined(FM_LINUX) && defined(FM_ARM64)
  const char *asset = "yt-dlp_linux_aarch64", *name = "yt-dlp";
#else
  const char *asset = "yt-dlp", *name = "yt-dlp";      /* the Python zipapp: needs python3 */
#endif
  char url[256];
  fm_snprintf(url, sizeof url, "https://github.com/yt-dlp/yt-dlp/releases/latest/download/%s", asset);
  fm_path_join(path, sizeof path, dir, name);
  InstProg ip;
  memset(&ip, 0, sizeof ip);
  ip.cb = cb;
  ip.user = user;
  FmNetResp r;
  FmErr e = net_download(url, NULL, path, inst_prog, &ip, &r, cancel);
  if (e != FM_OK) return e;
  FmStat st;
  if (!plat_stat(path, &st) || st.size < 512 * 1024) {    /* an error page, not the program */
    plat_remove_file(path);
    return FM_ERR_FORMAT;
  }
#ifndef FM_WIN
  plat_set_mode(path, 0755);
#endif
  const char *argv[] = { path, "--version", NULL };
  char *ver = NULL;
  int code = -1;
  if (cb) cb(user, -1.0f, "Checking yt-dlp\xE2\x80\xA6");
  e = proc_capture(argv, 4096, &ver, NULL, &code, cancel);
  fm_free(ver);
  if (e != FM_OK || code != 0) return e != FM_OK ? e : FM_ERR_FORMAT;
  fm_strlcpy(out, path, cap);
  return FM_OK;
}
