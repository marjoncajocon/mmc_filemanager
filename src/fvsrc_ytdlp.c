/* fvsrc_ytdlp.c -- yt-dlp: the shared search/resolve/download runner, and
** the "Any site" source (a pasted page link from any site yt-dlp knows).
**
** Design decisions:
**   - Resolve runs yt-dlp twice in two phases. Phase 1 (-J with -f/-S) picks
**     the formats and writes the info JSON into the cache; phase 2 runs one
**     yt-dlp per stream with --load-info-json, so the video and the audio
**     download in parallel on two threads without extracting the page again
**     (measured on a ~2 MB/s line, 10 min of 360p: 4.4 s to pick, 13.8 s to fetch
**     both; parallel helps when a site throttles each connection).
**   - The streams are never merged for playback (merging needs an ffmpeg
**     executable most users lack); the player opens the pair. Without the
**     FFmpeg libraries the selector prefers VP9/Opus WebM, which Media
**     Foundation decodes, over YouTube's fragmented H.264 MP4, which it does
**     not. With them it prefers H.264/AAC.
**   - Quality is chosen with -S res:H (the smaller side, so portrait clips
**     behave) rather than [height<=H] filters, which find nothing for a
**     1080x1920 short and then fall through to the biggest file.
**   - Cache files are "<site>-<id>-<height>.<v|a|av>.<ext>". yt-dlp writes
**     .part files and renames them when complete, so a final name always
**     means a complete stream and is reused without asking the network.
**     --no-mtime keeps mtimes at "now", which the trim (oldest first) needs.
**   - yt-dlp search pages are "ytsearch<last>:<q>" with -I first:last; the
**     extractor fetches lazily, so page N costs about N continuation
**     requests. Pages stop after VSRC_MAX_PAGES.
**   - Arguments go as a list (no shell) and the page URL always follows
**     "--", so a pasted "--exec ..." cannot become an option.
*/
#include "fvsrc_int.h"
#include "fproc.h"
#include "fplat.h"
#include "fsdl.h"

#define PROG_TMPL "download:@P %(progress.downloaded_bytes)s " \
  "%(progress.total_bytes,progress.total_bytes_estimate)s %(progress.speed)s " \
  "%(progress.fragment_index)s %(progress.fragment_count)s"

/* ---- running yt-dlp ------------------------------------------------------------ */

typedef struct Args { const char *v[64]; int n; } Args;

static void arg(Args *a, const char *s) {
  if (a->n < FM_COUNT(a->v) - 1) a->v[a->n++] = s;
  a->v[a->n] = NULL;
}

static void base_args(Args *a, const FmVsrcConf *c, bool js) {
  a->n = 0;
  arg(a, c->ytdlp);
  arg(a, "--no-config");          /* a user's yt-dlp.conf must not change what we parse */
  arg(a, "--encoding");
  arg(a, "utf-8");                /* titles arrive as UTF-8 whatever the console code page */
  arg(a, "--no-warnings");
  if (js && c->js_runtime[0]) {
    arg(a, "--js-runtimes");
    arg(a, c->js_runtime);
  }
}

/* "ERROR: [youtube] abc: Video unavailable. Use --list..." -> "Video unavailable." */
static void clean_error(const char *line, char *out, size_t cap) {
  const char *s = line;
  if (!strncmp(s, "ERROR:", 6)) s += 6;
  while (*s == ' ') s++;
  if (*s == '[') {
    const char *e = strchr(s, ']');
    if (e) s = e + 1;
    while (*s == ' ') s++;
    const char *colon = strstr(s, ": ");
    if (colon && colon - s < 64 && !memchr(s, ' ', (size_t)(colon - s))) s = colon + 2;
  }
  fm_strlcpy(out, s, cap);
  static const char *const kCut[] = { " Use --list-formats", "; please report", " Use --cookies" };
  for (int i = 0; i < FM_COUNT(kCut); i++) {
    char *p = strstr(out, kCut[i]);
    if (p) *p = 0;
  }
}

typedef struct Run {
  char *buf;
  size_t len, cap, max;
  bool over;
  char err[256];     /* last ERROR line, cleaned */
  char last[256];    /* last other stderr line */
} Run;

static bool run_line(void *user, const char *line, bool is_err) {
  Run *r = (Run *)user;
  if (is_err) {
    if (!strncmp(line, "ERROR:", 6)) clean_error(line, r->err, sizeof r->err);
    else if (line[0]) fm_strlcpy(r->last, line, sizeof r->last);
    return true;
  }
  size_t n = strlen(line);
  if (r->len + n + 2 > r->max) { r->over = true; return false; }
  if (r->len + n + 2 > r->cap) {
    size_t nc = r->cap ? r->cap * 2 : 65536;
    while (nc < r->len + n + 2) nc *= 2;
    r->buf = (char *)fm_realloc(r->buf, nc);
    r->cap = nc;
  }
  memcpy(r->buf + r->len, line, n);
  r->len += n;
  r->buf[r->len++] = '\n';
  r->buf[r->len] = 0;
  return true;
}

/* Turns a failed run into one line for the user. */
static FmErr run_fail(const char *site, const FmVsrcConf *c, const Run *r, FmErr e, int code, char *err,
                      size_t errcap) {
  if (e == FM_ERR_CANCEL && r->over) e = FM_ERR_FULL;
  if (e == FM_ERR_CANCEL) { fm_strlcpy(err, "Cancelled", errcap); return e; }
  if (e == FM_ERR_NOT_FOUND || (e != FM_OK && code == -1 && !r->err[0])) {
    fm_snprintf(err, errcap, "yt-dlp could not be started (%s)", c->ytdlp);
    return FM_ERR_NOT_FOUND;
  }
  if (e == FM_ERR_FULL) { fm_strlcpy(err, "yt-dlp sent more data than this app accepts", errcap); return e; }
  const char *msg = r->err[0] ? r->err : r->last;
  char tmp[200];
  if (!*msg) { fm_snprintf(tmp, sizeof tmp, "yt-dlp failed (exit code %d)", code); msg = tmp; }
  if (!strcmp(site, "youtube") && !c->js_runtime[0])
    fm_snprintf(err, errcap, "YouTube needs a JavaScript runtime for yt-dlp: install Deno (https://deno.com) "
                "or Node.js, then try again. (%s)", msg);
  else if (strstr(msg, "Sign in to confirm"))
    fm_snprintf(err, errcap, "%s asks to sign in to confirm you are not a bot; try again later or open it in "
                "the browser", !strcmp(site, "youtube") ? "YouTube" : "The site");
  else if (strstr(msg, "Unsupported URL"))
    fm_strlcpy(err, "yt-dlp does not support this link", errcap);
  else
    fm_strlcpy(err, msg, errcap);
  return e != FM_OK ? e : FM_ERR_IO;
}

static FmErr run_capture(const Args *a, Run *r, int *code, volatile int *cancel) {
  memset(r, 0, sizeof *r);
  r->max = VSRC_MAX_YTJSON;
  FmErr e = proc_run(a->v, run_line, r, code, cancel);
  if (r->over) e = FM_ERR_FULL;
  return e;
}

static bool ready(const FmVsrcConf *c, char *err, size_t cap) {
  if (!proc_available()) {
    fm_strlcpy(err, "This source needs yt-dlp, which cannot run on this platform; use Open in browser", cap);
    return false;
  }
  if (!c->ytdlp[0]) {
    fm_strlcpy(err, "This source needs yt-dlp: use \"Get yt-dlp\" in Settings, or set its path there", cap);
    return false;
  }
  return true;
}

/* ---- search ---------------------------------------------------------------------- */

static void pick_thumb(const FmJsonNode *e, char *out, size_t cap) {
  const char *t = json_str(json_get(e, "thumbnail"), "");
  if (vsrc_url_ok(t)) { fm_strlcpy(out, t, cap); return; }
  /* the one closest to 320 px wide: enough for a grid cell, small to fetch */
  const FmJsonNode *best = NULL;
  double bestd = 1e18;
  int seen = 0;
  for (const FmJsonNode *n = json_first(json_get(e, "thumbnails")); n && seen < 64;
       n = json_next(n), seen++) {
    if (!vsrc_url_ok(json_str(json_get(n, "url"), ""))) continue;
    double w = json_num(json_get(n, "width"), 0);
    double d = w > 0 ? fabs(w - 320) : 1e17;
    if (d <= bestd) { bestd = d; best = n; }
  }
  fm_strlcpy(out, best ? json_str(json_get(best, "url"), "") : "", cap);
}

static void add_entry(const FmJsonNode *e, const char *site, FmVsrcPage *out) {
  bool yt = !strcmp(site, "youtube");
  const char *id = json_str(json_get(e, "id"), "");
  const char *title = json_str(json_get(e, "title"), "");
  if (yt) {
    const char *ie = json_str(json_get(e, "ie_key"), "Youtube");
    if (strcmp(ie, "Youtube") || !vsrc_id_ok(id, "-_", 32)) return;   /* channels, playlists */
  }
  const char *page = json_str(json_get(e, "webpage_url"), "");
  if (!vsrc_url_ok(page)) page = json_str(json_get(e, "url"), "");
  if (!vsrc_url_ok(page)) page = json_str(json_get(e, "original_url"), "");
  if (!yt && !vsrc_url_ok(page)) return;                              /* nothing to resolve later */
  if (out->count >= VSRC_MAX_ITEMS) return;
  FmVsrcItem *it = vsrc_page_add(out);
  fm_strlcpy(it->id, id, sizeof it->id);
  fm_strlcpy(it->title, title[0] ? title : id, sizeof it->title);
  const char *ch = json_str(json_get(e, "channel"), "");
  if (!ch[0]) ch = json_str(json_get(e, "uploader"), "");
  if (!ch[0]) ch = json_str(json_get(e, "creator"), "");
  if (!ch[0]) ch = json_str(json_get(e, "uploader_id"), "");
  fm_strlcpy(it->channel, ch, sizeof it->channel);
  if (yt) {
    fm_snprintf(it->thumb, sizeof it->thumb, "https://i.ytimg.com/vi/%s/mqdefault.jpg", id);
    fm_snprintf(it->page, sizeof it->page, "https://www.youtube.com/watch?v=%s", id);
  } else {
    pick_thumb(e, it->thumb, sizeof it->thumb);
    fm_strlcpy(it->page, page, sizeof it->page);
  }
  double d = json_num(json_get(e, "duration"), 0);
  it->duration = d > 0 && d < 1e7 ? d : 0;
  double v = json_num(json_get(e, "view_count"), -1);
  it->views = v >= 0 && v < 9e15 ? (i64)v : -1;
  const char *ls = json_str(json_get(e, "live_status"), "");
  it->live = !strcmp(ls, "is_live") || json_bool(json_get(e, "is_live"), false);
  const char *ud = json_str(json_get(e, "upload_date"), "");
  if (strlen(ud) == 8 && vsrc_id_ok(ud, NULL, 8))
    fm_snprintf(it->published, sizeof it->published, "%.4s-%.2s-%.2s", ud, ud + 4, ud + 6);
  else {
    double ts = json_num(json_get(e, "timestamp"), 0);
    if (ts <= 0) ts = json_num(json_get(e, "release_timestamp"), 0);
    if (ts > 0 && ts < 1e11) vsrc_unix_date((i64)ts, it->published, sizeof it->published);
  }
}

FmErr vsrc_ytdlp_parse_search(const char *json, size_t len, const char *site, int first, int want,
                              FmVsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "yt-dlp sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  if (!root || root->type != JSON_OBJ) {
    json_free(&j);
    fm_strlcpy(out->error, "yt-dlp sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const char *type = json_str(json_get(root, "_type"), "video");
  if (!strcmp(type, "playlist")) {
    const FmJsonNode *ents = json_get(root, "entries");
    int n = 0;
    for (const FmJsonNode *e = json_first(ents); e && n < VSRC_MAX_ITEMS; e = json_next(e), n++)
      if (e->type == JSON_OBJ) add_entry(e, site, out);
    /* a full page means there may be more */
    if (n >= want && first + want <= VSRC_MAX_PAGES * want)
      fm_snprintf(out->next, sizeof out->next, "y:%d", first + want);
  } else {
    add_entry(root, site, out);
  }
  json_free(&j);
  return FM_OK;
}

FmErr vsrc_ytdlp_search(const FmVsrcConf *c, const char *site, const char *search_key, const char *target,
                        const char *page_token, FmVsrcPage *out, volatile int *cancel) {
  out->next[0] = 0;
  out->error[0] = 0;
  if (!ready(c, out->error, sizeof out->error))
    return proc_available() ? FM_ERR_NOT_FOUND : FM_ERR_UNSUPPORTED;
  if (!target || !*target) {
    fm_strlcpy(out->error, search_key ? "Type something to search for" : "Paste a video page link",
               sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  if (!search_key && !vsrc_url_ok(target)) {
    fm_strlcpy(out->error, "That is not a web link (it should start with https://)", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  int first = 1;
  if (page_token && !strncmp(page_token, "y:", 2)) first = atoi(page_token + 2);
  first = FM_CLAMP(first, 1, VSRC_MAX_PAGES * VSRC_PAGE_SIZE);
  int last = first + VSRC_PAGE_SIZE - 1;
  char range[32], tgt[1400];
  fm_snprintf(range, sizeof range, "%d:%d", first, last);
  if (search_key) fm_snprintf(tgt, sizeof tgt, "%s%d:%s", search_key, last, target);
  else fm_strlcpy(tgt, target, sizeof tgt);
  Args a;
  base_args(&a, c, !search_key);
  arg(&a, "--flat-playlist");
  arg(&a, "-J");
  arg(&a, "-I");
  arg(&a, range);
  if (!search_key) {
    arg(&a, "--no-playlist");              /* watch?v=X&list=Y means the video */
    arg(&a, "--ignore-no-formats-error");  /* metadata is enough here */
  }
  arg(&a, "--");
  arg(&a, tgt);
  Run r;
  int code = -1;
  FmErr e = run_capture(&a, &r, &code, cancel);
  if (e != FM_OK || code != 0 || !r.buf) {
    e = run_fail(site, c, &r, e, code, out->error, sizeof out->error);
    fm_free(r.buf);
    return e;
  }
  e = vsrc_ytdlp_parse_search(r.buf, r.len, site, first, VSRC_PAGE_SIZE, out);
  fm_free(r.buf);
  if (e == FM_OK && !out->count && first == 1)
    fm_strlcpy(out->error, search_key ? "No videos found" : "No video found at this link", sizeof out->error);
  return e;
}

/* ---- cache names ------------------------------------------------------------------- */

void vsrc_cache_key(const char *site, const FmVsrcItem *item, int height, char *out, size_t cap) {
  char id[52];
  size_t n = 0;
  for (const char *s = item->id; *s && n < sizeof id - 1; s++) {
    char ch = *s;
    bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
              ch == '_';
    id[n++] = ok ? ch : '_';
  }
  id[n] = 0;
  if (!strcmp(site, "web") || !n) {
    /* ids from different sites may collide: add the page URL's hash */
    u32 h = 2166136261u;
    for (const char *s = item->page; *s; s++) h = (h ^ (u8)*s) * 16777619u;
    fm_snprintf(out, cap, "%s-%s%s%08x-%d", site, id, n ? "-" : "", h, height);
  } else {
    fm_snprintf(out, cap, "%s-%s-%d", site, id, height);
  }
}

typedef struct Cached { char v[FM_PATH_MAX], a[FM_PATH_MAX], av[FM_PATH_MAX]; } Cached;

/* complete streams for key already in the cache */
static bool cache_find(const char *dir, const char *key, Cached *c) {
  memset(c, 0, sizeof *c);
  FmErr e;
  FmDir *d = plat_dir_open(dir, &e);
  if (!d) return false;
  size_t kn = strlen(key);
  const char *name;
  FmStat st;
  while (plat_dir_next(d, &name, &st)) {
    if ((st.flags & FM_ST_DIR) || !st.size || strncmp(name, key, kn) || name[kn] != '.') continue;
    const char *rest = name + kn + 1;
    char *slot = !strncmp(rest, "av.", 3) ? c->av
        : !strncmp(rest, "v.", 2)         ? c->v
        : !strncmp(rest, "a.", 2)         ? c->a
                                          : NULL;
    if (!slot) continue;
    const char *ext = strchr(rest, '.') + 1;
    if (strchr(ext, '.') || !vsrc_id_ok(ext, NULL, 8)) continue;   /* .part, .ytdl, .info.json */
    fm_path_join(slot, FM_PATH_MAX, dir, name);
  }
  plat_dir_close(d);
  return c->av[0] || (c->v[0] && c->a[0]);
}

/* Deletes key's leftovers (.part, HLS fragments, .ytdl, info JSON; with
** `all` the streams too). Returns how many could not be deleted yet: on
** Windows a killed yt-dlp.exe's child process holds its .part for up to a
** second (yt-dlp.exe is a bootloader plus the real process). */
static int cache_clean(const char *dir, const char *key, bool all) {
  size_t kn = strlen(key);
  char p[FM_PATH_MAX];
  char names[16][128];
  int failed = 0;
  /* names are collected before deleting; HLS can leave many fragments, so
  ** go round while each round still removes something */
  for (int round = 0; round < 64; round++) {
    FmErr e;
    FmDir *d = plat_dir_open(dir, &e);
    if (!d) return 0;
    const char *name;
    FmStat st;
    int n = 0;
    while (n < 16 && plat_dir_next(d, &name, &st)) {
      if ((st.flags & FM_ST_DIR) || strncmp(name, key, kn) || name[kn] != '.' || strlen(name) >= 128)
        continue;
      if (all || strstr(name, ".part") || strstr(name, ".ytdl") || strstr(name, ".info.json"))
        fm_strlcpy(names[n++], name, sizeof names[0]);
    }
    plat_dir_close(d);
    int removed = 0;
    failed = 0;
    for (int i = 0; i < n; i++) {
      if (fm_path_join(p, sizeof p, dir, names[i]) && plat_remove_file(p) == FM_OK) removed++;
      else failed++;
    }
    if (n < 16 || !removed) break;
  }
  return failed;
}

/* ---- parallel stream downloads ------------------------------------------------------ */

typedef struct Part {
  const FmVsrcConf *c;
  const char *info;               /* info JSON from phase 1 */
  char fid[64];
  char out[FM_PATH_MAX];
  char tmpl[FM_PATH_MAX * 2];     /* out with '%' escaped for yt-dlp's template */
  volatile int *cancel;
  SDL_SpinLock lock;
  double done, total, frac, speed;
  char err[256];
  int code;
  FmErr e;
  SDL_atomic_t finished;
} Part;

/* "123" or "12.5" -> value, "NA" -> -1 */
static double tok_num(const char **s) {
  while (**s == ' ') (*s)++;
  double v = -1, f = 0.1;
  bool any = false;
  for (; **s >= '0' && **s <= '9'; (*s)++) { v = (any ? v : 0) * 10 + (**s - '0'); any = true; }
  if (any && **s == '.')
    for ((*s)++; **s >= '0' && **s <= '9'; (*s)++) { v += (**s - '0') * f; f *= 0.1; }
  while (**s && **s != ' ') (*s)++;
  return any ? v : -1;
}

static bool part_line(void *user, const char *line, bool is_err) {
  Part *p = (Part *)user;
  if (is_err) {
    if (!strncmp(line, "ERROR:", 6)) clean_error(line, p->err, sizeof p->err);
    return true;
  }
  if (strncmp(line, "@P ", 3)) return true;
  const char *s = line + 3;
  double done = tok_num(&s), total = tok_num(&s), speed = tok_num(&s), fi = tok_num(&s), fc = tok_num(&s);
  double frac = total > 0 && done >= 0 ? done / total : fc > 0 && fi >= 0 ? fi / fc : -1;
  SDL_AtomicLock(&p->lock);
  if (done >= 0) p->done = done;
  if (total > 0) p->total = total;
  if (frac >= 0) p->frac = frac > 1 ? 1 : frac;
  if (speed >= 0) p->speed = speed;
  SDL_AtomicUnlock(&p->lock);
  return true;
}

static int part_thread(void *user) {
  Part *p = (Part *)user;
  Args a;
  base_args(&a, p->c, false);
  arg(&a, "--newline");
  arg(&a, "--no-mtime");
  arg(&a, "-N");                      /* 4 HLS fragments at once (64 s Dailymotion: 19 s -> 11 s) */
  arg(&a, "4");
  arg(&a, "--progress-template");
  arg(&a, PROG_TMPL);
  if (p->c->ffmpeg_dir[0]) {
    arg(&a, "--ffmpeg-location");     /* lets yt-dlp fix MPEG-TS-in-MP4 from HLS sites */
    arg(&a, p->c->ffmpeg_dir);
  }
  arg(&a, "--load-info-json");
  arg(&a, p->info);
  arg(&a, "-f");
  arg(&a, p->fid);
  arg(&a, "-o");
  arg(&a, p->tmpl);
  p->e = proc_run(a.v, part_line, p, &p->code, p->cancel);
  SDL_AtomicSet(&p->finished, 1);
  return 0;
}

/* Runs the parts in parallel and reports combined progress from this thread. */
static FmErr run_parts(Part *parts, int n, FmVsrcProgress cb, void *user, volatile int *cancel) {
  volatile int stop = 0;
  SDL_Thread *th[2] = { NULL, NULL };
  for (int i = 0; i < n; i++) {
    parts[i].cancel = &stop;
    SDL_AtomicSet(&parts[i].finished, 0);
    th[i] = fm_thread_create(part_thread, "ytdlp-dl", &parts[i]);
  }
  for (int i = 0; i < n; i++)
    if (!th[i]) { parts[i].cancel = cancel; part_thread(&parts[i]); }   /* no thread: one after the other */
  bool user_stop = false;
  for (;;) {
    bool all = true, failed = false;
    double done = 0, total = 0, fsum = 0, speed = 0;
    bool totals = true;
    for (int i = 0; i < n; i++) {
      if (!SDL_AtomicGet(&parts[i].finished)) all = false;
      else if (parts[i].e != FM_OK || parts[i].code != 0) failed = true;
      SDL_AtomicLock(&parts[i].lock);
      done += parts[i].done;
      total += parts[i].total;
      if (parts[i].total <= 0) totals = false;
      fsum += SDL_AtomicGet(&parts[i].finished) ? 1.0 : parts[i].frac;
      speed += SDL_AtomicGet(&parts[i].finished) ? 0 : parts[i].speed;
      SDL_AtomicUnlock(&parts[i].lock);
    }
    if (all) break;
    if (failed) stop = 1;                /* one stream failing makes the other useless */
    if (cancel && *cancel) { stop = 1; user_stop = true; }
    if (cb && !stop) {
      double f = totals && total > 0 ? done / total : fsum / n;
      char status[128], sp[24];
      const char *what = n == 2 ? "Downloading video + audio" : "Downloading";
      if (speed > 0) fm_snprintf(status, sizeof status, "%s %d%% \xC2\xB7 %s/s", what, (int)(f * 100),
                                 fm_fmt_size((u64)speed, sp, sizeof sp));
      else fm_snprintf(status, sizeof status, "%s %d%%", what, (int)(f * 100));
      if (!cb(user, (float)f, status)) { stop = 1; user_stop = true; }
    }
    SDL_Delay(120);
  }
  for (int i = 0; i < n; i++)
    if (th[i]) SDL_WaitThread(th[i], NULL);
  return user_stop ? FM_ERR_CANCEL : FM_OK;   /* the caller reads each part's result */
}

/* ---- resolve -------------------------------------------------------------------- */

const char *vsrc_ytdlp_selector(bool have_ffmpeg_libs, bool prefer_single) {
  if (have_ffmpeg_libs)
    return prefer_single ? "b[ext=mp4][protocol^=http]/b[ext=webm][protocol^=http]/"
                           "bv*[ext=mp4][vcodec^=avc1]+ba[ext=m4a]/b[ext=mp4]/bv*+ba/b"
                         : "bv*[ext=mp4][vcodec^=avc1]+ba[ext=m4a]/b[ext=mp4]/bv*+ba/b";
  /* Media Foundation: WebM VP9+Opus pairs and progressive MP4 play;
  ** YouTube's fragmented MP4 does not, so it only comes as a last resort */
  return prefer_single ? "b[ext=mp4][protocol^=http]/b[ext=webm][protocol^=http]/"
                         "bv*[ext=webm]+ba[ext=webm]/b[ext=mp4]/bv*+ba/b"
                       : "bv*[ext=webm]+ba[ext=webm]/b[ext=mp4]/bv*+ba/b";
}

typedef struct Fmt { char fid[64], ext[12], proto[24]; bool video; } Fmt;

static void read_fmt(const FmJsonNode *n, Fmt *f) {
  fm_strlcpy(f->fid, json_str(json_get(n, "format_id"), ""), sizeof f->fid);
  fm_strlcpy(f->ext, json_str(json_get(n, "ext"), ""), sizeof f->ext);
  fm_strlcpy(f->proto, json_str(json_get(n, "protocol"), ""), sizeof f->proto);
  f->video = strcmp(json_str(json_get(n, "vcodec"), "x"), "none") != 0;
}

static bool fmt_ok(const Fmt *f) {
  return vsrc_id_ok(f->fid, "-_.", 60) && vsrc_id_ok(f->ext, NULL, 8);
}

static FmErr ytdlp_fetch(const char *site, const FmVsrcConf *c, const FmVsrcItem *item, bool prefer_single,
                         FmVsrcStream *out, FmVsrcProgress cb, void *user, char *err, size_t errcap,
                         volatile int *cancel) {
  memset(out, 0, sizeof *out);
  if (errcap) err[0] = 0;
  if (!ready(c, err, errcap)) return proc_available() ? FM_ERR_NOT_FOUND : FM_ERR_UNSUPPORTED;
  if (!vsrc_url_ok(item->page)) {
    fm_strlcpy(err, "This item has no web page to play from", errcap);
    return FM_ERR_NOT_FOUND;
  }
  if (!c->cache_dir[0] || (plat_mkdirs(c->cache_dir) != FM_OK && !plat_is_dir(c->cache_dir))) {
    fm_strlcpy(err, "The cache folder cannot be created", errcap);
    return FM_ERR_IO;
  }
  int h = c->max_height >= 144 && c->max_height <= 4320 ? c->max_height : 720;
  char key[160];
  vsrc_cache_key(site, item, h, key, sizeof key);

  Cached *hit = (Cached *)fm_alloc(sizeof *hit);
  if (cache_find(c->cache_dir, key, hit)) {
    i64 now = plat_time_unix();
    if (hit->av[0]) {
      fm_strlcpy(out->video, hit->av, sizeof out->video);
      plat_set_mtime(hit->av, now);
    } else {
      fm_strlcpy(out->video, hit->v, sizeof out->video);
      fm_strlcpy(out->audio, hit->a, sizeof out->audio);
      plat_set_mtime(hit->v, now);
      plat_set_mtime(hit->a, now);
    }
    out->local = true;
    out->duration = item->duration;
    fm_free(hit);
    return FM_OK;
  }
  fm_free(hit);

  /* phase 1: choose the formats, keep the info JSON for phase 2 */
  if (cb && !cb(user, -1.0f, "Finding the video\xE2\x80\xA6")) {
    fm_strlcpy(err, "Cancelled", errcap);
    return FM_ERR_CANCEL;
  }
  char sort[32];
  fm_snprintf(sort, sizeof sort, "res:%d,proto", h);     /* then https before HLS at equal size */
  Args a;
  base_args(&a, c, true);
  arg(&a, "--no-playlist");
  arg(&a, "-S");
  arg(&a, sort);
  arg(&a, "-f");
  arg(&a, vsrc_ytdlp_selector(c->have_ffmpeg_libs, prefer_single));
  arg(&a, "-J");
  arg(&a, "--");
  arg(&a, item->page);
  Run r;
  int code = -1;
  FmErr e = run_capture(&a, &r, &code, cancel);
  if (e != FM_OK || code != 0 || !r.buf) {
    e = run_fail(site, c, &r, e, code, err, errcap);
    fm_free(r.buf);
    return e;
  }
  char info[FM_PATH_MAX];
  fm_snprintf(info, sizeof info, "%s%s%s.info.json", c->cache_dir, FM_SEP_STR, key);
  FILE *f = fm_fopen(info, "wb");
  bool wrote = f && fwrite(r.buf, 1, r.len, f) == r.len;
  if (f && fclose(f) != 0) wrote = false;

  Fmt fm[2];
  int nf = 0;
  bool live = false;
  FmJson j;
  if (json_parse(&j, r.buf, r.len) == FM_OK && json_root(&j) && json_root(&j)->type == JSON_OBJ) {
    const FmJsonNode *root = json_root(&j);
    const char *ls = json_str(json_get(root, "live_status"), "");
    live =
        json_bool(json_get(root, "is_live"), false) || !strcmp(ls, "is_live") || !strcmp(ls, "is_upcoming");
    const FmJsonNode *rf = json_get(root, "requested_formats");
    if (rf && rf->type == JSON_ARR && rf->count > 0) {
      for (const FmJsonNode *n = json_first(rf); n && nf < 2; n = json_next(n)) read_fmt(n, &fm[nf++]);
    } else {
      read_fmt(root, &fm[0]);
      nf = 1;
    }
    double d = json_num(json_get(root, "duration"), 0);
    out->duration = d > 0 && d < 1e7 ? d : item->duration;
    out->width = (int)json_num(json_get(root, "width"), 0);
    out->height = (int)json_num(json_get(root, "height"), 0);
  } else {
    nf = -1;
  }
  json_free(&j);
  fm_free(r.buf);
  if (nf < 0) {
    fm_strlcpy(err, "yt-dlp sent a reply this app does not understand", errcap);
    e = FM_ERR_FORMAT;
  } else if (live) {
    fm_strlcpy(err, "Live streams cannot be played here yet; use Open in browser", errcap);
    e = FM_ERR_UNSUPPORTED;
  } else if (!wrote) {
    fm_strlcpy(err, "Cannot write to the cache folder", errcap);
    e = FM_ERR_IO;
  }
  for (int i = 0; i < nf && e == FM_OK; i++)
    if (!fmt_ok(&fm[i])) {
      fm_strlcpy(err, "yt-dlp chose a format this app cannot name", errcap);
      e = FM_ERR_FORMAT;
    }
  if (e != FM_OK) { plat_remove_file(info); return e; }

  /* phase 2: one yt-dlp per stream, in parallel */
  if (nf == 2 && !fm[0].video && fm[1].video) { Fmt t = fm[0]; fm[0] = fm[1]; fm[1] = t; }
  Part *parts = (Part *)fm_calloc((size_t)nf, sizeof *parts);
  for (int i = 0; i < nf; i++) {
    Part *p = &parts[i];
    p->c = c;
    p->info = info;
    fm_strlcpy(p->fid, fm[i].fid, sizeof p->fid);
    fm_snprintf(p->out, sizeof p->out, "%s%s%s.%s.%s", c->cache_dir, FM_SEP_STR, key,
                nf == 1 ? "av" : i == 0 ? "v" : "a", fm[i].ext);
    size_t o = 0;
    for (const char *s = p->out; *s && o + 3 < sizeof p->tmpl; s++) {
      if (*s == '%') p->tmpl[o++] = '%';
      p->tmpl[o++] = *s;
    }
    p->tmpl[o] = 0;
  }
  e = run_parts(parts, nf, cb, user, cancel);
  if (e == FM_OK) {
    /* the culprit is the part that failed by itself, not the one stopped for it */
    int bad = -1;
    for (int i = 0; i < nf; i++) {
      FmStat st;
      bool ok = parts[i].e == FM_OK && parts[i].code == 0 && plat_stat(parts[i].out, &st) && st.size > 0;
      if (!ok && (bad < 0 || parts[bad].e == FM_ERR_CANCEL)) bad = i;
    }
    if (bad >= 0) {
      Part *p = &parts[bad];
      e = p->e != FM_OK && p->e != FM_ERR_CANCEL ? p->e : FM_ERR_IO;
      if (p->err[0]) fm_strlcpy(err, p->err, errcap);
      else if (p->e == FM_ERR_NOT_FOUND)
        fm_snprintf(err, errcap, "yt-dlp could not be started (%s)", c->ytdlp);
      else fm_snprintf(err, errcap, "The download failed (yt-dlp exit code %d)", p->code);
    }
  }
  if (e == FM_ERR_CANCEL) fm_strlcpy(err, "Cancelled", errcap);
  if (e == FM_OK) {
    fm_strlcpy(out->video, parts[0].out, sizeof out->video);
    if (nf == 2) fm_strlcpy(out->audio, parts[1].out, sizeof out->audio);
    out->local = true;
    cache_clean(c->cache_dir, key, false);
  } else {
    /* measured: the orphaned child exits within ~1 s of the kill */
    for (int i = 0; i < 15 && cache_clean(c->cache_dir, key, true) > 0; i++) SDL_Delay(200);
  }
  fm_free(parts);
  vsrc_cache_trim(c->cache_dir, VSRC_CACHE_MAX, key);
  return e;
}

FmErr vsrc_ytdlp_resolve(const char *site, const FmVsrcConf *c, const FmVsrcItem *item, FmVsrcStream *out,
                         FmVsrcProgress cb, void *user, char *err, size_t errcap, volatile int *cancel) {
  return ytdlp_fetch(site, c, item, false, out, cb, user, err, errcap, cancel);
}

FmErr vsrc_ytdlp_download(const char *site, const FmVsrcConf *c, const FmVsrcItem *item, const char *dir,
                          char *out_path, size_t cap, FmVsrcProgress cb, void *user, char *err, size_t errcap,
                          volatile int *cancel) {
  char ff[FM_PATH_MAX];
  if (cap) out_path[0] = 0;
  /* with ffmpeg any pair merges into one file; without it a single-file
  ** format (when the site has one) beats two loose files */
  bool merge = vsrc_ffmpeg_exe(c, ff, sizeof ff);
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
  FmErr e = ytdlp_fetch(site, c, item, !merge, st, cb, user, err, errcap, cancel);
  if (e == FM_OK)
    e = vsrc_save_stream(c, item, st, dir && *dir ? dir : c->download_dir, out_path, cap, cb, user, err,
                         errcap, cancel);
  fm_free(st);
  return e;
}

/* ---- the "Any site" source ------------------------------------------------------------ */

static FmErr web_search(const FmVsrcConf *c, const char *query, const char *page_token, FmVsrcPage *out,
                        volatile int *cancel) {
  char url[1200];
  const char *q = query ? query : "";
  while (*q == ' ' || *q == '\t') q++;
  fm_strlcpy(url, q, sizeof url);
  size_t n = strlen(url);
  while (n && (u8)url[n - 1] <= ' ') url[--n] = 0;
  /* "www.site.com/..." pasted without the scheme */
  if (n && strncmp(url, "http://", 7) && strncmp(url, "https://", 8) && strchr(url, '.') &&
      !strchr(url, ' ')) {
    char t[1200];
    fm_snprintf(t, sizeof t, "https://%s", url);
    fm_strlcpy(url, t, sizeof url);
  }
  return vsrc_ytdlp_search(c, "web", NULL, url, page_token, out, cancel);
}

static FmErr web_resolve(const FmVsrcConf *c, const FmVsrcItem *item, FmVsrcStream *out, FmVsrcProgress cb,
                         void *user, char *err, size_t errcap, volatile int *cancel) {
  return vsrc_ytdlp_resolve("web", c, item, out, cb, user, err, errcap, cancel);
}

static FmErr web_download(const FmVsrcConf *c, const FmVsrcItem *item, const char *dir, char *out_path,
                          size_t cap, FmVsrcProgress cb, void *user, char *err, size_t errcap,
                          volatile int *cancel) {
  return vsrc_ytdlp_download("web", c, item, dir, out_path, cap, cb, user, err, errcap, cancel);
}

const FmVsrc g_vsrc_web = {
  "web", "Any site", IC_LINK, VSRC_SEARCH | VSRC_URL | VSRC_YTDLP,
  web_search, web_resolve, web_download, NULL,
  "Paste a video page link from any of ~1800 sites yt-dlp supports",
};
