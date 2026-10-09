/* fpsrc_adult.c -- adult photo sources (the "adult" git branch only):
** RedGifs (photos of people), Yande.re (anime art), e621 (furry art).
** Checked from this PC (2026-10-09): all three answer without an account.
**
** Safe search (Online videos settings, on by default) is the age gate:
**   - RedGifs is adult only: it refuses while Safe search is on.
**   - Yande.re and e621 also hold safe art: with Safe search on they search
**     "rating:s" only; off, everything.
**
** Design decisions:
**   - RedGifs: a temporary token (GET /v2/auth/temporary, no account) per
**     search, then /v2/gifs/search?type=i (images). Its urls.thumbnail /
**     urls.hd are plain JPEGs; avgColor gives the placeholder colour.
**   - Yande.re: /post.json?tags=&page=&limit= (Moebooru). preview_url for
**     the grid, sample_url (~1500 px) for the viewer, file_url to download.
**   - e621: /posts.json?tags=&page=&limit= (wants a User-Agent that names
**     the app). preview/sample/file the same way; only still pictures are
**     kept (no webm/swf).
**   - Numbers may come as JSON strings (Yande.re): num() reads both.
*/
#include "fpsrc_int.h"

static bool adult_only(const FmPsrcConf *c, const char *site, char *err, size_t cap) {
  if (!c->safe_search) return true;
  fm_snprintf(err, cap, "Adult content: turn off Safe search in the Online video settings to use %s", site);
  return false;
}

static double num(const FmJsonNode *n) {
  const char *s = json_str(n, NULL);
  return s ? atof(s) : json_num(n, 0);
}

static u32 hex_color(const char *s) {                /* "#aabbcc" */
  if (!s || s[0] != '#' || strlen(s) < 7) return 0;
  return (u32)strtoul(s + 1, NULL, 16) & 0xFFFFFF;
}

/* q plus an optional extra tag, URL-encoded */
static void tags_param(const char *q, const char *extra, char *out, size_t cap) {
  char t[300];
  fm_snprintf(t, sizeof t, "%s%s%s", q ? q : "", q && *q && extra ? " " : "", extra ? extra : "");
  net_urlencode(t, out, cap);
}

/* ---- RedGifs ------------------------------------------------------------------- */

FmErr psrc_redgifs_parse(const char *json, size_t len, int page, FmPsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "RedGifs sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  const FmJsonNode *gifs = json_get(root, "gifs");
  if (!gifs || gifs->type != JSON_ARR) {
    json_free(&j);
    fm_strlcpy(out->error, "RedGifs sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  int seen = 0;
  for (const FmJsonNode *g = json_first(gifs); g && seen < PSRC_MAX_ITEMS; g = json_next(g), seen++) {
    const char *id = json_str(json_get(g, "id"), "");
    const FmJsonNode *u = json_get(g, "urls");
    const char *hd = json_str(json_get(u, "hd"), "");
    if (!*hd) hd = json_str(json_get(u, "sd"), "");
    const char *th = json_str(json_get(u, "thumbnail"), "");
    if (!*th) th = json_str(json_get(u, "sd"), hd);
    if (!psrc_id_ok(id, "", 90) || !psrc_url_viewable(hd)) continue;   /* images only, never the clips */
    FmPsrcItem *it = psrc_page_add(out);
    fm_strlcpy(it->source, "redgifs", sizeof it->source);
    psrc_copy(it->id, id, sizeof it->id);
    const char *desc = json_str(json_get(g, "description"), "");
    const char *user = json_str(json_get(g, "userName"), "");
    psrc_copy(it->title, *desc ? desc : id, sizeof it->title);
    psrc_copy(it->author, user, sizeof it->author);
    fm_strlcpy(it->license, "RedGifs (the creator's)", sizeof it->license);
    psrc_url_copy(th, it->thumb, sizeof it->thumb);
    psrc_url_copy(hd, it->full, sizeof it->full);
    fm_snprintf(it->page, sizeof it->page, "https://www.redgifs.com/watch/%s", it->id);
    it->width = (int)num(json_get(g, "width"));
    it->height = (int)num(json_get(g, "height"));
    it->color = hex_color(json_str(json_get(g, "avgColor"), ""));
  }
  int pages = (int)num(json_get(root, "pages"));
  if (seen > 0 && page < pages && page < 500) fm_snprintf(out->next, sizeof out->next, "%d", page + 1);
  json_free(&j);
  return FM_OK;
}

static FmErr rg_search(const FmPsrcConf *c, const char *query, const char *page_token, FmPsrcPage *out,
                       volatile int *cancel) {
  out->next[0] = out->error[0] = 0;
  if (!adult_only(c, "RedGifs", out->error, sizeof out->error)) return FM_ERR_UNSUPPORTED;
  if (!query || !*query) {
    fm_strlcpy(out->error, "Type something to search for", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  FmNetResp r;
  FmErr e = psrc_http_get("https://api.redgifs.com/v2/auth/temporary", NULL, &r, out->error, sizeof out->error,
                          cancel);
  if (e != FM_OK) return e;
  char hdr[2048];
  hdr[0] = 0;
  if (r.status == 200 && r.data) {
    FmJson j;
    if (json_parse(&j, (const char *)r.data, r.len) == FM_OK) {
      const char *tok = json_str(json_get(json_root(&j), "token"), "");
      if (*tok && strlen(tok) < 1800 && !strpbrk(tok, "\r\n")) fm_snprintf(hdr, sizeof hdr, "Authorization: Bearer %s\r\n", tok);
      json_free(&j);
    }
  }
  int st = r.status;
  net_resp_free(&r);
  if (!hdr[0]) return psrc_http_error("RedGifs", st == 200 ? 0 : st, false, out->error, sizeof out->error);
  int page = psrc_page_num(page_token, 1, 1, 500);
  char qe[800], url[1200];
  tags_param(query, NULL, qe, sizeof qe);
  fm_snprintf(url, sizeof url, "https://api.redgifs.com/v2/gifs/search?search_text=%s&type=i&count=%d&page=%d", qe,
              PSRC_PAGE_SIZE, page);
  e = psrc_http_get(url, hdr, &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  if (r.status != 200) {
    e = psrc_http_error("RedGifs", r.status, false, out->error, sizeof out->error);
    net_resp_free(&r);
    return e;
  }
  e = psrc_redgifs_parse((const char *)r.data, r.len, page, out);
  net_resp_free(&r);
  if (e == FM_OK && !out->count && page == 1) fm_strlcpy(out->error, "No photos found", sizeof out->error);
  return e;
}

const FmPsrc g_psrc_redgifs = {
  "redgifs", "RedGifs", IC_IMAGE, PSRC_SEARCH, rg_search, NULL,
  "Adult (18+) photos \xC2\xB7 no key \xC2\xB7 needs Safe search off", NULL, NULL,
};

/* ---- Yande.re ------------------------------------------------------------------ */

FmErr psrc_yandere_parse(const char *json, size_t len, int page, FmPsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK || json_root(&j)->type != JSON_ARR) {
    if (json) json_free(&j);
    fm_strlcpy(out->error, "Yande.re sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  int seen = 0;
  for (const FmJsonNode *p = json_first(json_root(&j)); p && seen < PSRC_MAX_ITEMS; p = json_next(p), seen++) {
    double id = num(json_get(p, "id"));
    const char *full = json_str(json_get(p, "sample_url"), "");
    const char *file = json_str(json_get(p, "file_url"), "");
    const char *th = json_str(json_get(p, "preview_url"), "");
    if (id <= 0 || !*full || !*th) continue;
    const char *ext = json_str(json_get(p, "file_ext"), "jpg");
    if (strcmp(ext, "jpg") && strcmp(ext, "jpeg") && strcmp(ext, "png") && strcmp(ext, "gif")) continue;
    FmPsrcItem *it = psrc_page_add(out);
    fm_strlcpy(it->source, "yandere", sizeof it->source);
    fm_snprintf(it->id, sizeof it->id, "%.0f", id);
    char tags[256];
    psrc_copy(tags, json_str(json_get(p, "tags"), ""), sizeof tags);
    fm_snprintf(it->title, sizeof it->title, "%.200s", tags[0] ? tags : it->id);
    psrc_copy(it->author, json_str(json_get(p, "author"), ""), sizeof it->author);
    fm_strlcpy(it->license, "Fan art (the artist's)", sizeof it->license);
    psrc_url_copy(th, it->thumb, sizeof it->thumb);
    psrc_url_copy(full, it->full, sizeof it->full);
    if (*file && strcmp(file, full)) psrc_url_copy(file, it->original, sizeof it->original);
    fm_snprintf(it->page, sizeof it->page, "https://yande.re/post/show/%s", it->id);
    it->width = (int)num(json_get(p, "sample_width"));
    it->height = (int)num(json_get(p, "sample_height"));
    if (it->width <= 0 || it->height <= 0) {
      it->width = (int)num(json_get(p, "width"));
      it->height = (int)num(json_get(p, "height"));
    }
  }
  if (seen >= PSRC_PAGE_SIZE && page < 500) fm_snprintf(out->next, sizeof out->next, "%d", page + 1);
  json_free(&j);
  return FM_OK;
}

static FmErr yd_search(const FmPsrcConf *c, const char *query, const char *page_token, FmPsrcPage *out,
                       volatile int *cancel) {
  out->next[0] = out->error[0] = 0;
  int page = psrc_page_num(page_token, 1, 1, 500);
  char te[800], url[1200];
  tags_param(query, c->safe_search ? "rating:s" : NULL, te, sizeof te);
  fm_snprintf(url, sizeof url, "https://yande.re/post.json?limit=%d&page=%d&tags=%s", PSRC_PAGE_SIZE, page, te);
  FmNetResp r;
  FmErr e = psrc_http_get(url, NULL, &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  if (r.status != 200) {
    e = psrc_http_error("Yande.re", r.status, false, out->error, sizeof out->error);
    net_resp_free(&r);
    return e;
  }
  e = psrc_yandere_parse((const char *)r.data, r.len, page, out);
  net_resp_free(&r);
  if (e == FM_OK && !out->count && page == 1)
    fm_strlcpy(out->error, "No pictures found (Yande.re searches tags: use_underscores, e.g. long_hair)",
               sizeof out->error);
  return e;
}

const FmPsrc g_psrc_yandere = {
  "yandere", "Yande.re", IC_IMAGE, PSRC_SEARCH | PSRC_BROWSE, yd_search, NULL,
  "Anime art, high resolution \xC2\xB7 no key \xC2\xB7 adult with Safe search off", NULL, NULL,
};

/* ---- e621 ------------------------------------------------------------------------ */

FmErr psrc_e621_parse(const char *json, size_t len, int page, FmPsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "e621 sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *posts = json_get(json_root(&j), "posts");
  if (!posts || posts->type != JSON_ARR) {
    json_free(&j);
    fm_strlcpy(out->error, "e621 sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  int seen = 0;
  for (const FmJsonNode *p = json_first(posts); p && seen < PSRC_MAX_ITEMS; p = json_next(p), seen++) {
    double id = num(json_get(p, "id"));
    const FmJsonNode *f = json_get(p, "file"), *s = json_get(p, "sample"), *pv = json_get(p, "preview");
    const char *ext = json_str(json_get(f, "ext"), "");
    if (strcmp(ext, "jpg") && strcmp(ext, "jpeg") && strcmp(ext, "png") && strcmp(ext, "gif")) continue;
    const char *file = json_str(json_get(f, "url"), "");
    const char *full = json_bool(json_get(s, "has"), false) ? json_str(json_get(s, "url"), "") : "";
    if (!*full) full = file;
    const char *th = json_str(json_get(pv, "url"), "");
    if (id <= 0 || !*full || !*th) continue;            /* hidden posts have no URLs when logged out */
    FmPsrcItem *it = psrc_page_add(out);
    fm_strlcpy(it->source, "e621", sizeof it->source);
    fm_snprintf(it->id, sizeof it->id, "%.0f", id);
    const FmJsonNode *artist = json_first(json_path(p, "tags.artist"));
    const char *by = artist ? json_str(artist, "") : "";
    char gen[200];
    gen[0] = 0;
    size_t gl = 0;
    for (const FmJsonNode *t = json_first(json_path(p, "tags.general")); t && gl + 40 < sizeof gen; t = json_next(t))
      gl += (size_t)fm_snprintf(gen + gl, sizeof gen - gl, "%s%s", gl ? " " : "", json_str(t, ""));
    fm_snprintf(it->title, sizeof it->title, "%s", gen[0] ? gen : it->id);
    psrc_copy(it->author, by, sizeof it->author);
    fm_strlcpy(it->license, "Fan art (the artist's)", sizeof it->license);
    psrc_url_copy(th, it->thumb, sizeof it->thumb);
    psrc_url_copy(full, it->full, sizeof it->full);
    if (*file && strcmp(file, full)) psrc_url_copy(file, it->original, sizeof it->original);
    fm_snprintf(it->page, sizeof it->page, "https://e621.net/posts/%s", it->id);
    bool samp = full != file;
    it->width = (int)num(json_get(samp ? s : f, "width"));
    it->height = (int)num(json_get(samp ? s : f, "height"));
  }
  if (seen >= PSRC_PAGE_SIZE && page < 750) fm_snprintf(out->next, sizeof out->next, "%d", page + 1);
  json_free(&j);
  return FM_OK;
}

static FmErr e6_search(const FmPsrcConf *c, const char *query, const char *page_token, FmPsrcPage *out,
                       volatile int *cancel) {
  out->next[0] = out->error[0] = 0;
  int page = psrc_page_num(page_token, 1, 1, 750);
  char te[800], url[1200];
  tags_param(query, c->safe_search ? "rating:s" : NULL, te, sizeof te);
  fm_snprintf(url, sizeof url, "https://e621.net/posts.json?limit=%d&page=%d&tags=%s", PSRC_PAGE_SIZE, page, te);
  FmNetResp r;
  FmErr e = psrc_http_get(url, NULL, &r, out->error, sizeof out->error, cancel);   /* our UA names the app */
  if (e != FM_OK) return e;
  if (r.status != 200) {
    e = psrc_http_error("e621", r.status, false, out->error, sizeof out->error);
    net_resp_free(&r);
    return e;
  }
  e = psrc_e621_parse((const char *)r.data, r.len, page, out);
  net_resp_free(&r);
  if (e == FM_OK && !out->count && page == 1)
    fm_strlcpy(out->error, "No pictures found (e621 searches tags: use_underscores)", sizeof out->error);
  return e;
}

const FmPsrc g_psrc_e621 = {
  "e621", "e621", IC_IMAGE, PSRC_SEARCH | PSRC_BROWSE, e6_search, NULL,
  "Furry art \xC2\xB7 no key \xC2\xB7 adult with Safe search off", NULL, NULL,
};
