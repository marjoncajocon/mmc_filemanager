/* fpsrc.c -- online photo sources: registry, settings snapshot, text and URL
** helpers, fetching into the cache or a download folder (see fpsrc.h).
**
** Design decisions:
**   - A page is one allocation of PSRC_MAX_ITEMS items; every parser stops
**     at that many, so it never grows (the realloc path is only a safety net
**     for other callers).
**   - Cached photos are named "<source>-<hash of the URL><ext>": the same
**     picture is never fetched twice, and the cache trim only ever deletes
**     files with an adapter's prefix, oldest first (a reused file is touched
**     so the trim is least-recently-used). About 500 MB are kept.
**   - The extension of a fetched file comes from its first bytes, not the
**     URL: API image URLs often have none ("...jpeg?auto=compress"), and an
**     error page with HTTP 200 must not land on disk as "photo.jpg".
**   - A download writes "<title> - <author>.<ext>" plus a "<same>.txt" with
**     title, author, licence and the source page, so the attribution most
**     licences require (CC BY, Unsplash, Pexels) travels with the file.
**     The name is made safe for every file system (reserved characters and
**     DOS device names) without splitting UTF-8 characters.
*/
#include "fpsrc.h"
#include "fpsrc_int.h"
#include "fvsrc_int.h"     /* vsrc_html_unescape, vsrc_unix_date */
#include "fconf.h"
#include "fplat.h"

/* ---- registry ----------------------------------------------------------------- */

static const FmPsrc *const kSources[] = {
  &g_psrc_openverse, &g_psrc_wikimedia, &g_psrc_nasa, &g_psrc_artic,
  &g_psrc_pexels, &g_psrc_unsplash, &g_psrc_pixabay,
};

int psrc_count(void) { return FM_COUNT(kSources); }

const FmPsrc *psrc_at(int i) { return i >= 0 && i < FM_COUNT(kSources) ? kSources[i] : NULL; }

const FmPsrc *psrc_find(const char *key) {
  if (!key) return NULL;
  for (int i = 0; i < FM_COUNT(kSources); i++)
    if (!strcmp(kSources[i]->key, key)) return kSources[i];
  return NULL;
}

const char *psrc_item_headers(const FmPsrcItem *item) {
  const FmPsrc *s = item ? psrc_find(item->source) : NULL;
  return s && s->img_headers && *s->img_headers ? s->img_headers : NULL;
}

/* ---- pages -------------------------------------------------------------------- */

void psrc_page_free(FmPsrcPage *p) {
  if (!p) return;
  fm_free(p->items);
  memset(p, 0, sizeof *p);
}

FmPsrcItem *psrc_page_add(FmPsrcPage *p) {
  if (p->count == p->cap) {
    p->cap = p->cap ? p->cap * 2 : PSRC_MAX_ITEMS;
    p->items = (FmPsrcItem *)fm_realloc(p->items, (size_t)p->cap * sizeof *p->items);
  }
  FmPsrcItem *it = &p->items[p->count++];
  memset(it, 0, sizeof *it);
  return it;
}

/* ---- settings ----------------------------------------------------------------- */

/* keys are pasted: drop surrounding blanks */
static void copy_key(char *out, const char *k, size_t cap) {
  while (*k == ' ' || *k == '\t') k++;
  fm_strlcpy(out, k, cap);
  size_t n = strlen(out);
  while (n && (u8)out[n - 1] <= ' ') out[--n] = 0;
}

void psrc_conf_snapshot(FmPsrcConf *c) {
  memset(c, 0, sizeof *c);
  copy_key(c->key_pexels, conf.key_pexels, sizeof c->key_pexels);
  copy_key(c->key_unsplash, conf.key_unsplash, sizeof c->key_unsplash);
  copy_key(c->key_pixabay, conf.key_pixabay, sizeof c->key_pixabay);
  char dir[FM_PATH_MAX];
  if (plat_place(PLACE_CACHE, dir, sizeof dir) || plat_place(PLACE_TEMP, dir, sizeof dir))
    fm_path_join(c->cache_dir, sizeof c->cache_dir, dir, "online-photos");
  if (conf.online_dl_dir[0]) fm_strlcpy(c->download_dir, conf.online_dl_dir, sizeof c->download_dir);
  else if (!plat_place(PLACE_DOWNLOADS, c->download_dir, sizeof c->download_dir))
    plat_place(PLACE_HOME, c->download_dir, sizeof c->download_dir);
  c->safe_search = conf.online_safe;
}

/* ---- text helpers ------------------------------------------------------------- */

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

bool psrc_id_ok(const char *s, const char *extra, size_t max) {
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

void psrc_copy(char *out, const char *s, size_t cap) {
  if (!cap) return;
  fm_strlcpy(out, s ? s : "", cap);
  utf8_cut_tail(out);
}

void psrc_html_text(const char *html, char *out, size_t cap) {
  if (!cap) return;
  out[0] = 0;
  if (!html || !*html) return;
  size_t n = strlen(html);
  if (n > 65536) n = 65536;                       /* a credit line, not a page */
  char *tmp = (char *)fm_alloc(n + 1);
  size_t o = 0;
  for (size_t i = 0; i < n;) {
    if (html[i] == '<') {
      size_t j = i + 1;
      while (j < n && html[j] != '>') j++;
      /* line-breaking tags become a blank so words do not run together */
      const char *t = html + i + 1;
      if (*t == '/') t++;
      if (!fm_strnicmp(t, "br", 2) || !fm_strnicmp(t, "p", 1) || !fm_strnicmp(t, "div", 3) ||
          !fm_strnicmp(t, "li", 2) || !fm_strnicmp(t, "td", 2))
        tmp[o++] = ' ';
      i = j < n ? j + 1 : n;
      continue;
    }
    if (html[i] == '&' && !fm_strnicmp(html + i, "&nbsp;", 6)) {   /* not in vsrc_html_unescape */
      tmp[o++] = ' ';
      i += 6;
      continue;
    }
    tmp[o++] = html[i++];
  }
  tmp[o] = 0;
  char *dec = (char *)fm_alloc(o * 4 + 8);
  vsrc_html_unescape(tmp, dec, o * 4 + 8);
  /* collapse whitespace (and no-break spaces) into single blanks */
  size_t w = 0;
  bool blank = true;
  for (const char *s = dec; *s && w + 1 < cap;) {
    bool sp = (u8)*s <= ' ';
    int adv = 1;
    if ((u8)s[0] == 0xC2 && (u8)s[1] == 0xA0) { sp = true; adv = 2; }
    if (sp) {
      if (!blank) { out[w++] = ' '; blank = true; }
    } else {
      out[w++] = *s;
      blank = false;
    }
    s += adv;
  }
  while (w > 0 && out[w - 1] == ' ') w--;
  out[w] = 0;
  utf8_cut_tail(out);
  fm_free(dec);
  fm_free(tmp);
}

u32 psrc_hex_color(const char *s) {
  if (!s || s[0] != '#' || strlen(s) != 7) return 0;
  u32 v = 0;
  for (int i = 1; i < 7; i++) {
    char c = s[i];
    int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
          : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
    if (d < 0) return 0;
    v = v << 4 | (u32)d;
  }
  return v;
}

void psrc_fit_width(int *w, int *h, int max_w) {
  if (*w <= 0 || *h <= 0) { *w = *h = 0; return; }
  if (*w > max_w) {
    *h = (int)((i64)*h * max_w / *w);
    if (*h < 1) *h = 1;
    *w = max_w;
  }
}

int psrc_page_num(const char *tok, int first, int lo, int hi) {
  if (!tok || !*tok) return first;
  int v = 0;
  for (const char *s = tok; *s; s++) {
    if (*s < '0' || *s > '9') return first;
    if (v < 1000000) v = v * 10 + (*s - '0');
  }
  return FM_CLAMP(v, lo, hi);
}

/* ---- URLs --------------------------------------------------------------------- */

bool psrc_url_copy(const char *url, char *out, size_t cap) {
  static const char hx[] = "0123456789ABCDEF";
  if (!cap) return false;
  out[0] = 0;
  if (!url || (strncmp(url, "https://", 8) && strncmp(url, "http://", 7))) return false;
  size_t o = 0;
  for (const char *s = url; *s; s++) {
    u8 c = (u8)*s;
    if (c < ' ' || c == 0x7F || c == '"' || c == '<' || c == '>' || c == '\\' || c == '`') {
      out[0] = 0;
      return false;
    }
    if (c == ' ' || c >= 0x80) {               /* NASA asset names have blanks */
      if (o + 4 > cap) { out[0] = 0; return false; }
      out[o++] = '%';
      out[o++] = hx[c >> 4];
      out[o++] = hx[c & 15];
    } else {
      if (o + 2 > cap) { out[0] = 0; return false; }
      out[o++] = (char)c;
    }
  }
  out[o] = 0;
  return true;
}

void psrc_url_ext(const char *url, char *out, size_t cap) {
  char path[1100];
  if (!cap) return;
  out[0] = 0;
  if (!url) return;
  fm_strlcpy(path, url, sizeof path);
  path[strcspn(path, "?#")] = 0;
  const char *slash = strrchr(path, '/');
  const char *base = slash ? slash + 1 : path;
  const char *dot = strrchr(base, '.');
  if (!dot || strlen(dot) < 2 || strlen(dot) > 5) return;
  size_t o = 0;
  for (const char *s = dot; *s && o + 1 < cap; s++) {
    char c = *s;
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (s > dot && !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) { out[0] = 0; return; }
    out[o++] = c;
  }
  out[o] = 0;
}

bool psrc_url_viewable(const char *url) {
  static const char *const kOk[] = { ".jpg", ".jpeg", ".jpe", ".png", ".gif", ".bmp" };
  char ext[16];
  psrc_url_ext(url, ext, sizeof ext);
  if (!ext[0]) return true;
  for (int i = 0; i < FM_COUNT(kOk); i++)
    if (!strcmp(ext, kOk[i])) return true;
  /* a dotted word that is not a known picture type ("photo.1234") */
  static const char *const kBad[] = { ".webp", ".tif", ".tiff", ".svg", ".heic", ".heif", ".avif",
                                      ".jxl", ".pdf", ".djvu", ".xcf", ".psd", ".mp4", ".webm" };
  for (int i = 0; i < FM_COUNT(kBad); i++)
    if (!strcmp(ext, kBad[i])) return false;
  return true;
}

/* ---- network ------------------------------------------------------------------ */

bool psrc_need_net(char *err, size_t cap) {
  if (net_available()) return true;
#if defined(FM_ANDROID) || defined(FM_WEB)
  fm_strlcpy(err, "Online photos are not available on this platform yet", cap);
#else
  fm_snprintf(err, cap, "No internet support on this system (%s)", net_backend());
#endif
  return false;
}

FmErr psrc_http_get(const char *url, const char *headers, FmNetResp *r, char *err, size_t errcap,
                    volatile int *cancel) {
  memset(r, 0, sizeof *r);
  if (!psrc_need_net(err, errcap)) return FM_ERR_UNSUPPORTED;
  FmErr e = net_get(url, headers, PSRC_MAX_REPLY, r, cancel);
  if (e == FM_ERR_CANCEL) { fm_strlcpy(err, "Cancelled", errcap); return e; }
  if (e != FM_OK) {
    fm_snprintf(err, errcap, "Network error \xE2\x80\x94 check the internet connection (%s)",
                r->error[0] ? r->error : fm_err_str(e));
    net_resp_free(r);
    return e;
  }
  return FM_OK;
}

FmErr psrc_http_error(const char *site, int status, bool keyed, char *err, size_t cap) {
  if ((status == 401 || status == 403) && keyed) {
    fm_snprintf(err, cap, "%s did not accept the API key \xE2\x80\x94 check it in Settings", site);
    return FM_ERR_ACCESS;
  }
  if (status == 429) {
    fm_snprintf(err, cap, "Too many requests \xE2\x80\x94 wait a minute (%s)", site);
    return FM_ERR_IO;
  }
  if (status >= 500)
    fm_snprintf(err, cap, "%s is having trouble (server error %d), try again later", site, status);
  else if (status && (status < 200 || status >= 300))
    fm_snprintf(err, cap, "%s refused the request (HTTP %d)", site, status);
  else {
    fm_snprintf(err, cap, "%s sent a reply this app does not understand", site);
    return FM_ERR_FORMAT;
  }
  return status == 401 || status == 403 ? FM_ERR_ACCESS : FM_ERR_IO;
}

bool psrc_have_key(const char *site, const char *key, char *err, size_t cap) {
  if (key && *key) return true;
  fm_snprintf(err, cap, "Add a free %s key in Settings", site);
  return false;
}

/* ---- file names and attribution ----------------------------------------------- */

static void safe_part(const char *s, char *out, size_t max) {
  size_t o = 0;
  if (!max) return;
  for (; s && *s && o + 1 < max; s++) {
    u8 c = (u8)*s;
    if (c < 0x20 || c == 0x7F || strchr("<>:\"/\\|?*", c)) c = ' ';
    if (c == ' ' && (o == 0 || out[o - 1] == ' ')) continue;
    if (o == 0 && c == '.') continue;                  /* no hidden files on Unix */
    out[o++] = (char)c;
  }
  out[o] = 0;
  utf8_cut_tail(out);
  o = strlen(out);
  while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '.')) o--;
  out[o] = 0;
}

void psrc_file_base(const FmPsrcItem *item, char *out, size_t max) {
  char t[100], a[48];
  if (!max) return;
  safe_part(item ? item->title : "", t, sizeof t);
  safe_part(item ? item->author : "", a, sizeof a);
  if (t[0] && a[0]) fm_snprintf(out, max, "%s - %s", t, a);
  else fm_strlcpy(out, t[0] ? t : a[0] ? a : "photo", max);
  utf8_cut_tail(out);
  size_t o = strlen(out);
  while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '.')) out[--o] = 0;
  if (!out[0]) fm_strlcpy(out, "photo", max);
  /* DOS device names are still reserved in every folder on Windows */
  static const char *const kDev[] = { "CON", "PRN", "AUX", "NUL" };
  size_t stem = strcspn(out, ". ");
  bool dev = false;
  for (int i = 0; i < FM_COUNT(kDev); i++)
    if (stem == 3 && !fm_strnicmp(out, kDev[i], 3)) dev = true;
  if (stem == 4 && (!fm_strnicmp(out, "COM", 3) || !fm_strnicmp(out, "LPT", 3)) && out[3] >= '0' &&
      out[3] <= '9')
    dev = true;
  if (dev && strlen(out) + 2 < max) {
    memmove(out + 1, out, strlen(out) + 1);
    out[0] = '_';
  }
}

const char *psrc_sniff_ext(const u8 *h, size_t n) {
  if (n >= 3 && h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF) return ".jpg";
  if (n >= 8 && !memcmp(h, "\x89PNG\r\n\x1A\n", 8)) return ".png";
  if (n >= 6 && (!memcmp(h, "GIF87a", 6) || !memcmp(h, "GIF89a", 6))) return ".gif";
  if (n >= 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WEBP", 4)) return ".webp";
  if (n >= 4 && (!memcmp(h, "II*\0", 4) || !memcmp(h, "MM\0*", 4))) return ".tif";
  if (n >= 14 && h[0] == 'B' && h[1] == 'M') return ".bmp";
  /* SVG: an <svg element near the start, and not an HTML page */
  size_t m = n < 1024 ? n : 1024;
  bool svg = false, html = false;
  for (size_t i = 0; i + 4 <= m; i++) {
    if (h[i] != '<') continue;
    if (!fm_strnicmp((const char *)h + i, "<svg", 4)) { svg = true; break; }
    if (i + 5 <= m && (!fm_strnicmp((const char *)h + i, "<html", 5) ||
                       (i + 9 <= m && !fm_strnicmp((const char *)h + i, "<!doctype", 9))))
      html = true;
  }
  return svg && !html ? ".svg" : "";
}

void psrc_attribution(const FmPsrcItem *item, const char *site, char *out, size_t cap) {
  char date[16];
  vsrc_unix_date(plat_time_unix(), date, sizeof date);
  fm_snprintf(out, cap,
              "Title: %s\nAuthor: %s\nLicence: %s\nSource: %s\nPage: %s\nImage: %s\nSaved: %s by mmcfm\n"
              "\nCheck the licence on the page before reusing this photo; most need a credit line.\n",
              item->title[0] ? item->title : "(untitled)", item->author[0] ? item->author : "(unknown)",
              item->license[0] ? item->license : "(see the page)", site ? site : item->source,
              item->page[0] ? item->page : "-", item->original[0] ? item->original : item->full, date);
}

void psrc_cache_key(const FmPsrcItem *item, const char *url, char *out, size_t cap) {
  /* FNV-1a 64 of the source key and the URL */
  u64 h = 0xcbf29ce484222325ULL;
  const char *parts[2];
  parts[0] = item->source;
  parts[1] = url ? url : "";
  for (int k = 0; k < 2; k++) {
    for (const u8 *s = (const u8 *)parts[k]; *s; s++) { h ^= *s; h *= 0x100000001b3ULL; }
    h ^= 0xFF; h *= 0x100000001b3ULL;
  }
  const FmPsrc *s = psrc_find(item->source);
  fm_snprintf(out, cap, "%s-%08x%08x", s ? s->key : "photo", (unsigned)(h >> 32), (unsigned)h);
}

/* ---- cache -------------------------------------------------------------------- */

typedef struct CacheEnt { char name[64]; u64 size; i64 mtime; } CacheEnt;

static int cache_cmp(const void *a, const void *b) {
  const CacheEnt *x = (const CacheEnt *)a, *y = (const CacheEnt *)b;
  return x->mtime < y->mtime ? -1 : x->mtime > y->mtime;
}

/* "<key>-<16 hex>" plus an extension or ".part" (also "photo-..." from odd items) */
static bool cache_name_ours(const char *name) {
  const char *dash = strchr(name, '-');
  if (!dash) return false;
  size_t n = (size_t)(dash - name);
  bool key = n == 5 && !strncmp(name, "photo", 5);
  for (int i = 0; i < FM_COUNT(kSources) && !key; i++)
    key = strlen(kSources[i]->key) == n && !strncmp(name, kSources[i]->key, n);
  if (!key) return false;
  for (int i = 1; i <= 16; i++) {
    char c = dash[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return dash[17] == '.' || dash[17] == 0;
}

void psrc_cache_trim(const char *cache_dir, u64 max_bytes, const char *keep) {
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
      if (cap >= 16384) continue;
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

/* ---- fetching ----------------------------------------------------------------- */

static const char *const kExts[] = { ".jpg", ".png", ".gif", ".webp", ".tif", ".svg", ".bmp" };

/* a complete cached copy of key (any extension) */
static bool cache_lookup(const char *dir, const char *key, char *out, size_t cap) {
  char name[96];
  FmStat st;
  for (int i = 0; i < FM_COUNT(kExts); i++) {
    fm_snprintf(name, sizeof name, "%s%s", key, kExts[i]);
    if (fm_path_join(out, cap, dir, name) && plat_stat(out, &st) && !(st.flags & FM_ST_DIR) && st.size > 0)
      return true;
  }
  out[0] = 0;
  return false;
}

static const char *file_sniff(const char *path) {
  u8 head[1024];
  FILE *f = fm_fopen(path, "rb");
  if (!f) return "";
  size_t n = fread(head, 1, sizeof head, f);
  fclose(f);
  return psrc_sniff_ext(head, n);
}

static FmErr copy_file(const char *from, const char *to, volatile int *cancel) {
  FILE *in = fm_fopen(from, "rb");
  if (!in) return FM_ERR_NOT_FOUND;
  char part[FM_PATH_MAX];
  fm_snprintf(part, sizeof part, "%s.part", to);
  FILE *out = fm_fopen(part, "wb");
  if (!out) { fclose(in); return FM_ERR_IO; }
  size_t bufsz = 128 * 1024;
  u8 *buf = (u8 *)fm_alloc(bufsz);
  FmErr e = FM_OK;
  for (;;) {
    if (cancel && *cancel) { e = FM_ERR_CANCEL; break; }
    size_t n = fread(buf, 1, bufsz, in);
    if (!n) { if (ferror(in)) e = FM_ERR_IO; break; }
    if (fwrite(buf, 1, n, out) != n) { e = FM_ERR_FULL; break; }
  }
  fm_free(buf);
  fclose(in);
  if (fclose(out) != 0 && e == FM_OK) e = FM_ERR_IO;
  if (e == FM_OK) e = plat_rename(part, to);
  if (e != FM_OK) plat_remove_file(part);
  return e;
}

/* dir/base[ (n)]<ext> with no file and no "<same>.txt" there yet */
static bool unique_dest(const char *dir, const char *base, const char *ext, char *img, char *txt, size_t cap) {
  char name[300];
  for (int i = 1; i < 1000; i++) {
    if (i == 1) fm_snprintf(name, sizeof name, "%s", base);
    else fm_snprintf(name, sizeof name, "%s (%d)", base, i);
    char a[320], b[320];
    fm_snprintf(a, sizeof a, "%s%s", name, ext);
    fm_snprintf(b, sizeof b, "%s.txt", name);
    if (!fm_path_join(img, cap, dir, a) || !fm_path_join(txt, cap, dir, b)) return false;
    if (!plat_exists(img) && !plat_exists(txt)) return true;
  }
  return false;
}

static bool write_text(const char *path, const char *text) {
  FILE *f = fm_fopen(path, "wb");
  if (!f) return false;
  size_t n = strlen(text);
  bool ok = fwrite(text, 1, n, f) == n;
  return fclose(f) == 0 && ok;
}

/* GET url into path; on failure err says why (HTTP status, not an image) */
static FmErr fetch_to(const char *url, const char *headers, const char *site, const char *path,
                      bool (*progress)(void *, u64, u64), void *user, const char **ext, char *err, size_t errcap,
                      volatile int *cancel) {
  FmNetResp r;
  memset(&r, 0, sizeof r);
  *ext = "";
  if (!psrc_need_net(err, errcap)) return FM_ERR_UNSUPPORTED;
  FmErr e = net_download(url, headers, path, progress, user, &r, cancel);
  if (e == FM_ERR_CANCEL) { fm_strlcpy(err, "Cancelled", errcap); return e; }
  if (e != FM_OK) {
    if (r.status && (r.status < 200 || r.status >= 300)) {
      psrc_http_error(site, r.status, false, err, errcap);
      return r.status == 404 || r.status == 410 ? FM_ERR_NOT_FOUND : FM_ERR_IO;
    }
    fm_snprintf(err, errcap, "Download failed \xE2\x80\x94 %s", r.error[0] ? r.error : fm_err_str(e));
    return e;
  }
  *ext = file_sniff(path);
  if (!**ext) {
    plat_remove_file(path);
    fm_snprintf(err, errcap, "%s sent something that is not a picture (%s)", site,
                r.type[0] ? r.type : "unknown type");
    return FM_ERR_FORMAT;
  }
  return FM_OK;
}

FmErr psrc_fetch(const FmPsrcConf *c, const FmPsrcItem *item, const char *dir, char *out, size_t cap,
                 bool (*progress)(void *user, u64 done, u64 total), void *user, char *err, size_t errcap,
                 volatile int *cancel) {
  char url[1100], key[64], cached[FM_PATH_MAX], tmp[FM_PATH_MAX];
  if (cap) out[0] = 0;
  if (errcap) err[0] = 0;
  if (!c || !item) { fm_strlcpy(err, "Nothing to fetch", errcap); return FM_ERR_NOT_FOUND; }
  const FmPsrc *src = psrc_find(item->source);
  const char *site = src ? src->name : "The site";
  bool save = dir && *dir;
  const char *want = save && item->original[0] ? item->original : item->full;
  if (!psrc_url_copy(want, url, sizeof url)) {
    fm_strlcpy(err, "This photo has no image address", errcap);
    return FM_ERR_NOT_FOUND;
  }
  const char *headers = psrc_item_headers(item);
  psrc_cache_key(item, url, key, sizeof key);
  bool have_cache = c->cache_dir[0] && cache_lookup(c->cache_dir, key, cached, sizeof cached);
  const char *ext = "";

  if (!save) {
    if (have_cache) {
      plat_set_mtime(cached, plat_time_unix());           /* recently used: trimmed last */
      fm_strlcpy(out, cached, cap);
      return FM_OK;
    }
    if (!c->cache_dir[0]) { fm_strlcpy(err, "No cache folder", errcap); return FM_ERR_NOT_FOUND; }
    if (plat_mkdirs(c->cache_dir) != FM_OK && !plat_is_dir(c->cache_dir)) {
      fm_snprintf(err, errcap, "Cannot create the folder %s", c->cache_dir);
      return FM_ERR_IO;
    }
    char name[96];
    fm_snprintf(name, sizeof name, "%s.dl", key);
    fm_path_join(tmp, sizeof tmp, c->cache_dir, name);
    FmErr e = fetch_to(url, headers, site, tmp, progress, user, &ext, err, errcap, cancel);
    if (e != FM_OK) return e;
    fm_snprintf(name, sizeof name, "%s%s", key, ext);
    fm_path_join(cached, sizeof cached, c->cache_dir, name);
    plat_remove_file(cached);
    e = plat_rename(tmp, cached);
    if (e != FM_OK) {
      plat_remove_file(tmp);
      fm_snprintf(err, errcap, "Could not save into the cache: %s", fm_err_str(e));
      return e;
    }
    psrc_cache_trim(c->cache_dir, PSRC_CACHE_MAX, key);
    fm_strlcpy(out, cached, cap);
    return FM_OK;
  }

  /* into a download folder */
  if (plat_mkdirs(dir) != FM_OK && !plat_is_dir(dir)) {
    fm_snprintf(err, errcap, "Cannot create the folder %s", dir);
    return FM_ERR_IO;
  }
  char base[200], img[FM_PATH_MAX], txt[FM_PATH_MAX];
  psrc_file_base(item, base, 150);
  FmErr e;
  if (have_cache) {
    ext = fm_path_ext(cached);
    if (!unique_dest(dir, base, ext, img, txt, sizeof img)) {
      fm_strlcpy(err, "Too many files with this name", errcap);
      return FM_ERR_EXISTS;
    }
    e = copy_file(cached, img, cancel);
    if (e == FM_ERR_CANCEL) { fm_strlcpy(err, "Cancelled", errcap); return e; }
    if (e != FM_OK) { fm_snprintf(err, errcap, "Could not save: %s", fm_err_str(e)); return e; }
  } else {
    /* the extension is known only after the first bytes arrived */
    char name[300];
    fm_snprintf(name, sizeof name, "%s.%llx.download", base, (unsigned long long)plat_now_ms());
    fm_path_join(tmp, sizeof tmp, dir, name);
    e = fetch_to(url, headers, site, tmp, progress, user, &ext, err, errcap, cancel);
    if (e != FM_OK) return e;
    if (!unique_dest(dir, base, ext, img, txt, sizeof img)) {
      plat_remove_file(tmp);
      fm_strlcpy(err, "Too many files with this name", errcap);
      return FM_ERR_EXISTS;
    }
    e = plat_rename(tmp, img);
    if (e != FM_OK) {
      plat_remove_file(tmp);
      fm_snprintf(err, errcap, "Could not save: %s", fm_err_str(e));
      return e;
    }
  }
  char text[3200];
  psrc_attribution(item, site, text, sizeof text);
  write_text(txt, text);                    /* the photo itself is what matters */
  if (src && src->saved) src->saved(c, item, cancel);
  fm_strlcpy(out, img, cap);
  return FM_OK;
}
