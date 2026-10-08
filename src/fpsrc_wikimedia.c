/* fpsrc_wikimedia.c -- Wikimedia Commons: 100M+ freely licensed files, the
** picture library behind Wikipedia. No key.
**
** Design decisions:
**   - One request: a search generator over the File: namespace with
**     imageinfo (urls, size, mime, licence and author) for every hit, and
**     "filetype:bitmap" so PDFs, videos and sounds stay out. Pages come back
**     keyed by page id; "index" restores the search order.
**   - The thumbnail server only renders standard widths (500, 960, 1280,
**     1920 ...; 2048 answers 400, 3840 is rate limited), so `full` is the
**     original when it is a JPEG/PNG/GIF of at most 1920 px, else the thumb
**     URL with the largest standard width the original allows. Download
**     keeps the original (`original`), which can be 50 MB or a TIFF.
**   - Artist and ObjectName are HTML fragments (links to user pages); they
**     are reduced to plain text.
**   - Wikimedia asks API clients to identify themselves: besides fnet's
**     User-Agent the API calls send Api-User-Agent.
*/
#include "fpsrc_int.h"

#define WM_HEADERS "Api-User-Agent: mmcfm/0.1 (open-source file manager; photo gallery)\r\n"

void psrc_wikimedia_url(const char *q, int offset, char *out, size_t cap) {
  char qq[300], qe[1000];
  fm_snprintf(qq, sizeof qq, "%.250s filetype:bitmap", q ? q : "");
  net_urlencode(qq, qe, sizeof qe);
  fm_snprintf(out, cap,
              "https://commons.wikimedia.org/w/api.php?action=query&format=json&generator=search"
              "&gsrsearch=%s&gsrnamespace=6&gsrlimit=%d&gsroffset=%d&prop=imageinfo"
              "&iiprop=url%%7Csize%%7Cmime%%7Cextmetadata&iiurlwidth=480"
              "&iiextmetadatafilter=LicenseShortName%%7CArtist%%7CObjectName",
              qe, PSRC_PAGE_SIZE, offset < 0 ? 0 : offset);
}

bool psrc_wikimedia_resize(const char *thumb, int w, char *out, size_t cap) {
  /* ".../thumb/a/ab/Name.jpg/500px-Name.jpg" (or "lossy-page1-500px-...") */
  if (cap) out[0] = 0;
  if (!thumb || w <= 0) return false;
  const char *q = strchr(thumb, '?');
  size_t end = q ? (size_t)(q - thumb) : strlen(thumb);
  const char *slash = NULL;
  for (size_t i = 0; i < end; i++)
    if (thumb[i] == '/') slash = thumb + i;
  if (!slash) return false;
  const char *px = strstr(slash, "px-");
  if (!px || (size_t)(px - thumb) > end) return false;
  const char *d = px;
  while (d > slash + 1 && d[-1] >= '0' && d[-1] <= '9') d--;
  if (d == px) return false;
  if (d != slash + 1 && d[-1] != '-') return false;
  int n = fm_snprintf(out, cap, "%.*s%d%s", (int)(d - thumb), thumb, w, px);
  if (n < 0 || (size_t)n >= cap) { out[0] = 0; return false; }
  return true;
}

typedef struct WmHit { const FmJsonNode *page; int index; } WmHit;

static int hit_cmp(const void *a, const void *b) {
  const WmHit *x = (const WmHit *)a, *y = (const WmHit *)b;
  return x->index < y->index ? -1 : x->index > y->index;
}

/* "File:Cat on snow.jpg" -> "Cat on snow" */
static void file_title(const char *t, char *out, size_t cap) {
  if (!strncmp(t, "File:", 5)) t += 5;
  psrc_copy(out, t, cap);
  char *dot = strrchr(out, '.');
  if (dot && strlen(dot) <= 5 && dot != out) *dot = 0;
}

static void add_hit(const FmJsonNode *pg, FmPsrcPage *out) {
  const FmJsonNode *ii = json_at(json_get(pg, "imageinfo"), 0);
  const char *mime = json_str(json_get(ii, "mime"), "");
  if (strncmp(mime, "image/", 6)) return;
  char thumb[512], orig[1024], full[1024], pagelink[512];
  if (!psrc_url_copy(json_str(json_get(ii, "thumburl"), ""), thumb, sizeof thumb)) return;
  if (!psrc_url_viewable(thumb)) return;
  bool have_orig = psrc_url_copy(json_str(json_get(ii, "url"), ""), orig, sizeof orig);
  double w = json_num(json_get(ii, "width"), 0), h = json_num(json_get(ii, "height"), 0);
  int W = w > 0 && w < 1e6 ? (int)w : 0, H = h > 0 && h < 1e6 ? (int)h : 0;
  bool simple = !strcmp(mime, "image/jpeg") || !strcmp(mime, "image/png") || !strcmp(mime, "image/gif");
  bool vector = !strcmp(mime, "image/svg+xml");
  full[0] = 0;
  int fw = W, fh = H;
  if (have_orig && simple && W > 0 && W <= 1920) {
    fm_strlcpy(full, orig, sizeof full);
  } else {
    static const int kBuckets[] = { 1920, 1280, 960, 500 };
    int pick = vector ? 1280 : 0;
    for (int i = 0; i < FM_COUNT(kBuckets) && !pick; i++)
      if (W >= kBuckets[i]) pick = kBuckets[i];
    if (pick > 500 && psrc_wikimedia_resize(thumb, pick, full, sizeof full)) {
      if (W > 0 && H > 0) { fh = (int)((i64)H * pick / W); fw = pick; }
      if (fh < 1) fh = 1;
    } else {
      fm_strlcpy(full, thumb, sizeof full);
      fw = (int)json_num(json_get(ii, "thumbwidth"), 0);
      fh = (int)json_num(json_get(ii, "thumbheight"), 0);
    }
  }
  if (!psrc_url_viewable(full)) return;
  FmPsrcItem *it = psrc_page_add(out);
  fm_strlcpy(it->source, "wikimedia", sizeof it->source);
  fm_snprintf(it->id, sizeof it->id, "%.0f", json_num(json_get(pg, "pageid"), 0));
  const FmJsonNode *meta = json_get(ii, "extmetadata");
  psrc_html_text(json_str(json_path(meta, "ObjectName.value"), ""), it->title, sizeof it->title);
  if (!it->title[0]) file_title(json_str(json_get(pg, "title"), ""), it->title, sizeof it->title);
  psrc_html_text(json_str(json_path(meta, "Artist.value"), ""), it->author, sizeof it->author);
  psrc_html_text(json_str(json_path(meta, "LicenseShortName.value"), ""), it->license, sizeof it->license);
  fm_strlcpy(it->thumb, thumb, sizeof it->thumb);
  fm_strlcpy(it->full, full, sizeof it->full);
  if (have_orig && strcmp(orig, full)) fm_strlcpy(it->original, orig, sizeof it->original);
  if (psrc_url_copy(json_str(json_get(ii, "descriptionurl"), ""), pagelink, sizeof pagelink))
    fm_strlcpy(it->page, pagelink, sizeof it->page);
  it->width = fw > 0 && fh > 0 ? fw : 0;
  it->height = fw > 0 && fh > 0 ? fh : 0;
}

FmErr psrc_wikimedia_parse(const char *json, size_t len, FmPsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "Wikimedia Commons sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  if (!root || root->type != JSON_OBJ) {
    fm_strlcpy(out->error, "Wikimedia Commons sent a reply this app does not understand", sizeof out->error);
    json_free(&j);
    return FM_ERR_FORMAT;
  }
  const char *info = json_str(json_path(root, "error.info"), "");
  if (*info) {
    fm_snprintf(out->error, sizeof out->error, "Wikimedia Commons: %.200s", info);
    json_free(&j);
    return FM_ERR_FORMAT;
  }
  /* no "query" at all is how the API says "nothing found" */
  const FmJsonNode *pages = json_path(root, "query.pages");
  WmHit hits[PSRC_MAX_ITEMS];
  int n = 0;
  for (const FmJsonNode *p = json_first(pages); p && n < PSRC_MAX_ITEMS; p = json_next(p)) {
    if (p->type != JSON_OBJ) continue;
    hits[n].page = p;
    hits[n].index = (int)json_num(json_get(p, "index"), 1e6);
    n++;
  }
  qsort(hits, (size_t)n, sizeof *hits, hit_cmp);
  for (int i = 0; i < n; i++) add_hit(hits[i].page, out);
  double next = json_num(json_path(root, "continue.gsroffset"), -1);
  if (next > 0 && next < 10000) fm_snprintf(out->next, sizeof out->next, "%d", (int)next);
  json_free(&j);
  return FM_OK;
}

static FmErr wm_search(const FmPsrcConf *c, const char *query, const char *token, FmPsrcPage *out,
                       volatile int *cancel) {
  FM_UNUSED(c);
  out->next[0] = out->error[0] = 0;
  if (!query || !*query) {
    fm_strlcpy(out->error, "Type something to search for", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  int offset = psrc_page_num(token, 0, 0, 9999);
  char url[1600];
  psrc_wikimedia_url(query, offset, url, sizeof url);
  FmNetResp r;
  FmErr e = psrc_http_get(url, WM_HEADERS, &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  if (r.status != 200) {
    e = psrc_http_error("Wikimedia Commons", r.status, false, out->error, sizeof out->error);
    net_resp_free(&r);
    return e;
  }
  e = psrc_wikimedia_parse((const char *)r.data, r.len, out);
  net_resp_free(&r);
  if (e == FM_OK && !out->count && offset == 0 && !out->next[0])
    fm_strlcpy(out->error, "No photos found", sizeof out->error);
  return e;
}

const FmPsrc g_psrc_wikimedia = {
  "wikimedia", "Wikimedia Commons", IC_NETWORK, PSRC_SEARCH, wm_search, NULL,
  "Free photos behind Wikipedia \xC2\xB7 no key needed",
  NULL, NULL,
};
