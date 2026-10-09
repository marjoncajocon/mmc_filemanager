/* fcloud_webdav.c -- WebDAV for the cloud panels (fcloud.h): Nextcloud,
** ownCloud, pCloud, Koofr, Yandex Disk, Box, a NAS.
**
** The account's server is the WebDAV base URL ("https://cloud.example.com/
** remote.php/dav/files/USER/"); ids are paths relative to it, folders end
** with '/'. Every request carries Basic auth, so the password stays in the
** account (there is no session to trade it for).
**
** Design decisions:
**   - Plain http:// is refused unless the host is on the user's own network
**     (loopback, private ranges, single-label / .local names): Basic auth
**     would otherwise send the password in the clear over the internet.
**   - Listing is one PROPFIND Depth 1 parsed with the pull reader (fxml.c),
**     matching element names by their local part, so "d:", "D:", "lp1:" and
**     a default namespace all work. Hrefs are percent-decoded and taken
**     relative to the base path; a server that answers with other hrefs
**     still lists (by the last path segment).
**   - Names come from the href, not displayname: displayname is optional,
**     localised on some servers, and the href is what later requests use.
**   - Uploads stream from the file (net_request body_file). On Nextcloud
**     (".../remote.php/dav/files/USER/") files above 100 MB go through
**     chunked upload v2 in 32 MB pieces, so a dropped connection or a proxy
**     body limit costs one piece; any other server gets one plain PUT.
**   - Rename and move are one MOVE with Overwrite: F (the server says 412
**     when the target exists instead of silently replacing it).
**   - Shared helpers for fcloud_s3.c and the tests live here (fcloud_dav.h).
*/
#include "fcloud.h"
#include "fcloud_dav.h"
#include "fcrypt.h"
#include "fxml.h"
#include "fplat.h"

#define DAV_NAME "WebDAV"
#define DAV_URL_MAX 3200
#define DAV_LIST_MAX (64u << 20)      /* a 100k-item folder is about 40 MB of XML */
#define DAV_CHUNK (32ll << 20)

i64 g_dav_chunk_min, g_dav_chunk_size;   /* test hooks (fcloud_dav.h) */

/* ---- shared helpers -------------------------------------------------------------- */

u64 dav_atou64(const char *s) {
  u64 v = 0;
  if (!s) return 0;
  while (*s == ' ' || *s == '\t') s++;
  for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (u64)(*s - '0');
  return v;
}

void dav_civil(i64 t, int *y, int *mo, int *d, int *h, int *mi, int *s) {
  i64 days = t / 86400, rem = t % 86400;
  if (rem < 0) { rem += 86400; days--; }
  *h = (int)(rem / 3600);
  *mi = (int)(rem / 60 % 60);
  *s = (int)(rem % 60);
  /* Howard Hinnant's civil_from_days */
  days += 719468;
  i64 era = (days >= 0 ? days : days - 146096) / 146097;
  i64 doe = days - era * 146097;
  i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  i64 mp = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *mo = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = (int)(yoe + era * 400 + (*mo <= 2));
}

bool dav_split_url(const char *url, bool *https, char *host, size_t hcap, char *port, size_t pcap,
                   const char **path) {
  const char *p;
  if (!fm_strnicmp(url, "https://", 8)) { *https = true; p = url + 8; }
  else if (!fm_strnicmp(url, "http://", 7)) { *https = false; p = url + 7; }
  else return false;
  const char *end = p + strcspn(p, "/?#");
  const char *at = p;                             /* skip user:pass@ */
  for (const char *q = p; q < end; q++) if (*q == '@') at = q + 1;
  p = at;
  const char *hb = p, *he, *pt = NULL;
  if (*p == '[') {
    hb = p + 1;
    he = memchr(hb, ']', (size_t)(end - hb));
    if (!he) return false;
    if (he + 1 < end && he[1] == ':') pt = he + 2;
  } else {
    he = memchr(p, ':', (size_t)(end - p));
    if (he) pt = he + 1; else he = end;
  }
  if (he == hb || (size_t)(he - hb) + 1 > hcap) return false;
  memcpy(host, hb, (size_t)(he - hb));
  host[he - hb] = 0;
  if (pcap) {
    size_t n = pt ? (size_t)(end - pt) : 0;
    if (n + 1 > pcap) return false;
    if (n) memcpy(port, pt, n);
    port[n] = 0;
  }
  if (path) *path = *end == '/' ? end : "/";
  return true;
}

static bool ends_with_label(const char *h, const char *suf) {
  size_t n = strlen(h), m = strlen(suf);
  return n > m && h[n - m - 1] == '.' && !fm_stricmp(h + n - m, suf);
}

bool dav_host_is_local(const char *host) {
  if (!fm_stricmp(host, "localhost") || ends_with_label(host, "localhost")) return true;
  if (strchr(host, ':')) {                        /* IPv6 */
    if (!strcmp(host, "::1")) return true;
    if (!fm_strnicmp(host, "fe8", 3) || !fm_strnicmp(host, "fe9", 3) || !fm_strnicmp(host, "fea", 3) ||
        !fm_strnicmp(host, "feb", 3)) return true;    /* fe80::/10 link-local */
    if ((host[0] == 'f' || host[0] == 'F') && (host[1] == 'c' || host[1] == 'C' || host[1] == 'd' || host[1] == 'D'))
      return true;                                /* fc00::/7 unique local */
    return false;
  }
  int a[4], n = 0;
  const char *p = host;
  while (n < 4) {
    if (*p < '0' || *p > '9') break;
    int v = 0, digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 4) { v = v * 10 + (*p - '0'); p++; digits++; }
    a[n++] = v;
    if (*p == '.') p++; else break;
  }
  if (n == 4 && !*p) {                            /* dotted IPv4 */
    if (a[0] == 127 || a[0] == 10) return true;
    if (a[0] == 192 && a[1] == 168) return true;
    if (a[0] == 172 && a[1] >= 16 && a[1] <= 31) return true;
    if (a[0] == 169 && a[1] == 254) return true;
    if (a[0] == 100 && a[1] >= 64 && a[1] <= 127) return true;   /* CGNAT: Tailscale */
    return false;
  }
  if (!strchr(host, '.')) return true;            /* "nas", "diskstation" */
  return ends_with_label(host, "local") || ends_with_label(host, "lan") || ends_with_label(host, "home") ||
         ends_with_label(host, "internal") || ends_with_label(host, "home.arpa");
}

bool dav_pct_path(const char *s, char *out, size_t cap) {
  static const char hx[] = "0123456789ABCDEF";
  size_t o = 0;
  for (; *s; s++) {
    u8 c = (u8)*s;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~' || c == '/') {
      if (o + 1 >= cap) return false;
      out[o++] = (char)c;
    } else {
      if (o + 3 >= cap) return false;
      out[o++] = '%';
      out[o++] = hx[c >> 4];
      out[o++] = hx[c & 15];
    }
  }
  if (!cap) return false;
  out[o] = 0;
  return true;
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

size_t dav_pct_decode(const char *s, size_t n, char *out, size_t cap) {
  size_t o = 0;
  if (!cap) return 0;
  for (size_t i = 0; i < n && o + 1 < cap; i++) {
    int h, l;
    if (s[i] == '%' && i + 2 < n && (h = hexval(s[i + 1])) >= 0 && (l = hexval(s[i + 2])) >= 0) {
      out[o++] = (char)(h * 16 + l);
      i += 2;
    } else {
      out[o++] = s[i];
    }
  }
  out[o] = 0;
  return o;
}

static void base64(const u8 *in, size_t n, char *out, size_t cap) {
  static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < n && o + 5 < cap; i += 3) {
    u32 v = (u32)in[i] << 16 | (i + 1 < n ? (u32)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
    out[o++] = t[v >> 18 & 63];
    out[o++] = t[v >> 12 & 63];
    out[o++] = i + 1 < n ? t[v >> 6 & 63] : '=';
    out[o++] = i + 2 < n ? t[v & 63] : '=';
  }
  if (cap) out[o] = 0;
}

/* ---- the account ------------------------------------------------------------------- */

typedef struct Dav {
  char base[1100];        /* the URL, '/'-terminated, unsafe bytes encoded */
  char base_path[1100];   /* its path, decoded ("/remote.php/dav/files/me/") */
  char auth[1100];        /* "Authorization: Basic ...\r\n" */
  char user[256];
} Dav;

static FmErr dav_open(const FmCloudAcct *a, Dav *d, char *err, size_t cap) {
  char host[256], port[16];
  const char *path;
  bool https;
  const char *s = a->server;
  while (*s == ' ') s++;
  memset(d, 0, sizeof *d);
  if (!*s) { fm_strlcpy(err, "Enter the WebDAV address of your server.", cap); return FM_ERR_FORMAT; }
  char url[1100];
  if (!strstr(s, "://")) fm_snprintf(url, sizeof url, "https://%s", s);   /* "cloud.example.com/dav" */
  else fm_strlcpy(url, s, sizeof url);
  size_t n = strlen(url);
  while (n && (url[n - 1] == ' ' || url[n - 1] == '\r' || url[n - 1] == '\n')) url[--n] = 0;
  if (!dav_split_url(url, &https, host, sizeof host, port, sizeof port, &path)) {
    fm_strlcpy(err, "The server address should look like https://cloud.example.com/remote.php/dav/files/USER/", cap);
    return FM_ERR_FORMAT;
  }
  if (!https && !dav_host_is_local(host)) {
    fm_strlcpy(err, "Plain http:// would send your password unencrypted over the internet. Use https:// "
               "(http is only allowed for servers on your own network).", cap);
    return FM_ERR_ACCESS;
  }
  /* copy, encoding what a pasted address may carry raw (spaces, UTF-8), keeping %XX as typed */
  size_t o = 0;
  static const char hx[] = "0123456789ABCDEF";
  for (const char *p = url; *p && *p != '?' && *p != '#' && o + 5 < sizeof d->base; p++) {
    u8 c = (u8)*p;
    if (c <= ' ' || c >= 0x7f || c == '"' || c == '<' || c == '>' || c == '\\' || c == '^' || c == '`' ||
        c == '{' || c == '|' || c == '}') {
      d->base[o++] = '%'; d->base[o++] = hx[c >> 4]; d->base[o++] = hx[c & 15];
    } else {
      d->base[o++] = (char)c;
    }
  }
  if (o && d->base[o - 1] != '/') d->base[o++] = '/';
  d->base[o] = 0;
  const char *bp;
  if (!dav_split_url(d->base, &https, host, sizeof host, port, sizeof port, &bp)) return FM_ERR_FORMAT;
  dav_pct_decode(bp, strlen(bp), d->base_path, sizeof d->base_path);
  char cred[800];
  char b64[1100];
  fm_snprintf(cred, sizeof cred, "%s:%s", a->user, a->secret);
  base64((const u8 *)cred, strlen(cred), b64, sizeof b64);
  wipe(cred, sizeof cred);
  fm_snprintf(d->auth, sizeof d->auth, "Authorization: Basic %s\r\n", b64);
  wipe(b64, sizeof b64);
  fm_strlcpy(d->user, a->user, sizeof d->user);
  return FM_OK;
}

static void dav_close(Dav *d) { wipe(d->auth, sizeof d->auth); }

/* base + encoded id */
static bool dav_url(const Dav *d, const char *id, char *out, size_t cap) {
  size_t n = fm_strlcpy(out, d->base, cap);
  if (n >= cap) return false;
  while (*id == '/') id++;
  return dav_pct_path(id, out + n, cap - n);
}

static FmErr dav_fail(const FmNetResp *r, FmErr e, char *err, size_t cap) {
  if (e == FM_OK && r->status == 401) {
    fm_strlcpy(err, "The server refused the user name or password (use an app password when the account has "
               "two-factor sign-in).", cap);
    return FM_ERR_PASSWORD;
  }
  if (e == FM_OK && r->status == 507) {
    fm_strlcpy(err, "The server is out of space (HTTP 507).", cap);
    return FM_ERR_FULL;
  }
  FmErr ret = cloud_http_error(DAV_NAME, r, e, err, cap);
  if (ret == FM_OK) {                             /* called on a reply that is not the expected one */
    fm_snprintf(err, cap, "%s gave an unexpected answer (HTTP %d)", DAV_NAME, r->status);
    ret = FM_ERR_IO;
  }
  return ret;
}

/* One request with auth + extra header lines. */
static FmErr dav_req(const Dav *d, const char *method, const char *url, const char *extra, const char *body,
                     FmNetResp *r, volatile int *cancel) {
  char hdr[DAV_URL_MAX + 1400];
  fm_snprintf(hdr, sizeof hdr, "%s%s", d->auth, extra ? extra : "");
  FmNetReq rq;
  memset(&rq, 0, sizeof rq);
  rq.method = method;
  rq.headers = hdr;
  if (body) { rq.body = body; rq.body_len = strlen(body); }
  rq.max_reply = DAV_LIST_MAX;
  FmErr e = net_request(url, &rq, r, cancel);
  wipe(hdr, sizeof hdr);
  return e;
}

static bool ok2xx(const FmNetResp *r) { return r->status >= 200 && r->status < 300; }

/* ---- PROPFIND replies ---------------------------------------------------------------- */

/* The local part of the current START/END name ("d:href" -> "href"). */
static bool lname(const FmXml *x, const char *local) {
  const char *n = x->name;
  size_t len = x->name_len;
  const char *c = memchr(n, ':', len);
  if (c) { len -= (size_t)(c + 1 - n); n = c + 1; }
  return len == strlen(local) && !memcmp(n, local, len);
}

typedef struct Prop {
  char href[2048];
  char size[32], mod[64], type[64], etag[80], disp[256];
  char qused[32], qfree[32];
  bool dir, in_rt;
} Prop;

static void trim_slashes(char *s) {
  size_t n = strlen(s);
  while (n > 1 && s[n - 1] == '/') s[--n] = 0;
}

static void emit(const Prop *p, int idx, const char *base_path, const char *dir_id, FmCloudList *out, int *added) {
  char path[2048];
  const char *h = p->href;
  if (!fm_strnicmp(h, "http://", 7) || !fm_strnicmp(h, "https://", 8)) {   /* absolute URL: its path */
    h = strstr(h, "://") + 3;
    h += strcspn(h, "/");
  }
  dav_pct_decode(h, strcspn(h, "?#"), path, sizeof path);
  if (!path[0]) return;
  /* the path relative to the account's base */
  const char *rel = NULL;
  size_t bl = strlen(base_path);
  if (bl && !fm_strnicmp(path, base_path, bl)) rel = path + bl;
  else if (bl && strlen(path) + 1 == bl && !fm_strnicmp(path, base_path, bl - 1)) rel = "";  /* "/dav" for "/dav/" */
  char want[CLOUD_ID_MAX + 2], got[2048];
  fm_strlcpy(want, dir_id, sizeof want);
  trim_slashes(want);
  if (!strcmp(want, "/")) want[0] = 0;
  if (rel) {
    while (*rel == '/') rel++;
    fm_strlcpy(got, rel, sizeof got);
    trim_slashes(got);
    if (!strcmp(got, "/")) got[0] = 0;
    if (!strcmp(got, want)) return;               /* the folder itself */
  } else {
    /* hrefs outside the base (a proxy rewriting paths): the folder itself is
    ** the first response (every server does that); the rest give a name */
    char me[2048];
    fm_snprintf(me, sizeof me, "%s%s", base_path, want);
    trim_slashes(me);
    fm_strlcpy(got, path, sizeof got);
    trim_slashes(got);
    if (!strcmp(got, me) || idx == 0) return;
  }
  /* the last segment is the name */
  trim_slashes(got);
  const char *name = strrchr(got, '/');
  name = name ? name + 1 : got;
  if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) return;
  if (rel) {
    size_t wl = strlen(want);                     /* only direct children */
    if (wl && (strncmp(got, want, wl) || got[wl] != '/')) return;
    if (strchr(got + (wl ? wl + 1 : 0), '/')) return;
  }
  char id[CLOUD_ID_MAX];
  int n = fm_snprintf(id, sizeof id, "%s%s%s%s", want, want[0] ? "/" : "", name, p->dir ? "/" : "");
  if (n < 0 || (size_t)n >= sizeof id) return;    /* too deep to address */
  FmCloudEntry *e = cloud_list_add(out);
  fm_strlcpy(e->id, id, sizeof e->id);
  fm_strlcpy(e->name, name, sizeof e->name);
  e->dir = p->dir;
  e->size = p->dir ? 0 : dav_atou64(p->size);
  e->mtime = p->mod[0] ? cloud_parse_time(p->mod) : 0;
  if (!p->dir) fm_strlcpy(e->mime, p->type, sizeof e->mime);
  if (p->etag[0]) {
    const char *t = p->etag;
    if (!strncmp(t, "W/", 2)) t += 2;
    size_t tl = strlen(t);
    if (tl >= 2 && t[0] == '"' && t[tl - 1] == '"') { t++; tl -= 2; }
    char tag[72];
    fm_snprintf(tag, sizeof tag, "etag:%.*s", (int)FM_MIN(tl, (size_t)60), t);
    fm_strlcpy(e->hash, tag, sizeof e->hash);
  }
  (*added)++;
}

/* Walks the multistatus; calls emit for each response when out != NULL,
** else keeps the last quota values in q. */
static int walk(const char *xml, size_t len, const char *base_path, const char *dir_id, FmCloudList *out, Prop *q) {
  FmXml x;
  Prop *p = (Prop *)fm_calloc(1, sizeof *p);
  bool multi = false, in_resp = false;
  int added = 0, idx = 0;
  xml_init(&x, xml, len);
  while (xml_next(&x) != XML_EOF) {
    if (x.type == XML_START) {
      if (lname(&x, "multistatus")) multi = true;
      else if (lname(&x, "response")) { memset(p, 0, sizeof *p); in_resp = true; if (x.empty) in_resp = false; }
      else if (!in_resp) continue;
      else if (lname(&x, "href") && !p->href[0]) xml_inner_text(&x, p->href, sizeof p->href);
      else if (lname(&x, "getcontentlength")) xml_inner_text(&x, p->size, sizeof p->size);
      else if (lname(&x, "getlastmodified")) xml_inner_text(&x, p->mod, sizeof p->mod);
      else if (lname(&x, "getcontenttype")) xml_inner_text(&x, p->type, sizeof p->type);
      else if (lname(&x, "getetag")) xml_inner_text(&x, p->etag, sizeof p->etag);
      else if (lname(&x, "displayname")) xml_inner_text(&x, p->disp, sizeof p->disp);
      else if (lname(&x, "quota-used-bytes")) xml_inner_text(&x, p->qused, sizeof p->qused);
      else if (lname(&x, "quota-available-bytes")) xml_inner_text(&x, p->qfree, sizeof p->qfree);
      else if (lname(&x, "resourcetype")) p->in_rt = !x.empty;
      else if (p->in_rt && lname(&x, "collection")) p->dir = true;
    } else if (x.type == XML_END) {
      if (lname(&x, "resourcetype")) p->in_rt = false;
      else if (lname(&x, "response") && in_resp) {
        in_resp = false;
        if (out && p->href[0]) emit(p, idx, base_path, dir_id, out, &added);
        idx++;
        if (q && (p->qused[0] || p->qfree[0]) && !q->qused[0] && !q->qfree[0]) *q = *p;
      }
    }
  }
  fm_free(p);
  return multi ? added : -1;
}

int dav_parse_multistatus(const char *xml, size_t len, const char *base_path, const char *dir_id,
                          FmCloudList *out) {
  return walk(xml, len, base_path, dir_id, out, NULL);
}

bool dav_parse_quota(const char *xml, size_t len, u64 *used, u64 *total) {
  Prop *q = (Prop *)fm_calloc(1, sizeof *q);
  walk(xml, len, "", "", NULL, q);
  bool ok = q->qused[0] || q->qfree[0];
  *used = dav_atou64(q->qused);
  /* negative "available" (-1 not computed, -2 unknown, -3 unlimited) = no total */
  const char *f = q->qfree;
  while (*f == ' ') f++;
  *total = (f[0] && f[0] != '-') ? *used + dav_atou64(f) : 0;
  fm_free(q);
  return ok;
}

static const char kPropfind[] =
  "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
  "<d:propfind xmlns:d=\"DAV:\"><d:prop>"
  "<d:resourcetype/><d:getcontentlength/><d:getlastmodified/><d:getcontenttype/><d:getetag/><d:displayname/>"
  "</d:prop></d:propfind>";

static const char kQuota[] =
  "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
  "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:quota-available-bytes/><d:quota-used-bytes/></d:prop></d:propfind>";

#define PROPFIND_HDR(depth) "Depth: " depth "\r\nContent-Type: application/xml; charset=utf-8\r\n"

/* ---- the adapter --------------------------------------------------------------------- */

static FmErr w_login(FmCloudAcct *a, char *err, size_t errcap, volatile int *cancel) {
  Dav d;
  FmErr e = dav_open(a, &d, err, errcap);
  if (e) return e;
  FmNetResp r;
  e = dav_req(&d, "PROPFIND", d.base, PROPFIND_HDR("0"), kPropfind, &r, cancel);
  if (e == FM_OK && r.status == 207) {
    if (dav_parse_multistatus((const char *)r.data, r.len, d.base_path, "", NULL) < 0) {
      fm_strlcpy(err, "The address answered, but not as a WebDAV server: check the path.", errcap);
      e = FM_ERR_FORMAT;
    } else {
      fm_strlcpy(a->session, "basic", sizeof a->session);   /* nothing to keep: every request signs in */
      a->session_changed = true;
    }
  } else if (e == FM_OK && (r.status == 200 || r.status == 404 || r.status == 405)) {
    fm_snprintf(err, errcap, "%s", r.status == 404 ? "The server has no WebDAV folder at that address: check the path."
                : "The address answered, but not as a WebDAV server: check the path.");
    e = FM_ERR_NOT_FOUND;
  } else {
    e = dav_fail(&r, e, err, errcap);
  }
  net_resp_free(&r);
  dav_close(&d);
  return e;
}

static FmErr w_list(FmCloudAcct *a, const char *dir_id, FmCloudList *out, volatile int *cancel) {
  Dav d;
  FmErr e = dav_open(a, &d, out->error, sizeof out->error);
  if (e) return e;
  char url[DAV_URL_MAX];
  char dir[CLOUD_ID_MAX + 2];
  fm_strlcpy(dir, dir_id ? dir_id : "", sizeof dir);
  size_t n = strlen(dir);
  if (n && dir[n - 1] != '/') fm_strlcat(dir, "/", sizeof dir);
  if (!dav_url(&d, dir, url, sizeof url)) {
    fm_strlcpy(out->error, "The path is too long.", sizeof out->error);
    dav_close(&d);
    return FM_ERR_FORMAT;
  }
  FmNetResp r;
  e = dav_req(&d, "PROPFIND", url, PROPFIND_HDR("1"), kPropfind, &r, cancel);
  if (e == FM_OK && r.status == 207) {
    if (dav_parse_multistatus((const char *)r.data, r.len, d.base_path, dir, out) < 0) {
      fm_strlcpy(out->error, "The server's folder listing could not be read.", sizeof out->error);
      e = FM_ERR_FORMAT;
    }
  } else if (e == FM_OK && r.status == 404) {
    fm_strlcpy(out->error, "The folder is gone (HTTP 404).", sizeof out->error);
    e = FM_ERR_NOT_FOUND;
  } else {
    e = dav_fail(&r, e, out->error, sizeof out->error);
  }
  net_resp_free(&r);
  dav_close(&d);
  return e;
}

static FmErr w_download(FmCloudAcct *a, const FmCloudEntry *ent, const char *local_path, FmNetProgress cb,
                        void *user, char *err, size_t errcap, volatile int *cancel) {
  Dav d;
  FmErr e = dav_open(a, &d, err, errcap);
  if (e) return e;
  char url[DAV_URL_MAX];
  if (!dav_url(&d, ent->id, url, sizeof url)) { dav_close(&d); fm_strlcpy(err, "The path is too long.", errcap); return FM_ERR_FORMAT; }
  FmNetResp r;
  e = net_download(url, d.auth, local_path, cb, user, &r, cancel);
  if (e != FM_OK && e != FM_ERR_CANCEL && ok2xx(&r))   /* the reply was fine: the local file failed */
    fm_snprintf(err, errcap, "Cannot write the file: %s", fm_err_str(e));
  else if (e != FM_OK && e != FM_ERR_CANCEL) e = dav_fail(&r, r.status ? FM_OK : e, err, errcap);
  else if (e == FM_ERR_CANCEL) fm_strlcpy(err, "Cancelled.", errcap);
  net_resp_free(&r);
  dav_close(&d);
  return e;
}

/* Upload progress: only the body phase, offset by the pieces already sent. */
typedef struct UpProg { FmNetProgress cb; void *user; u64 base, total; i64 piece; } UpProg;

static bool up_prog(void *user, u64 done, u64 total) {
  UpProg *u = (UpProg *)user;
  if (!u->cb || (i64)total != u->piece) return true;
  return u->cb(u->user, u->base + done, u->total);
}

static FmErr put_file(const Dav *d, const char *url, const char *extra, const char *path, i64 off, i64 len,
                      UpProg *up, FmNetResp *r, volatile int *cancel) {
  char hdr[DAV_URL_MAX + 1400];
  fm_snprintf(hdr, sizeof hdr, "%sContent-Type: application/octet-stream\r\n%s", d->auth, extra ? extra : "");
  FmNetReq rq;
  memset(&rq, 0, sizeof rq);
  rq.method = "PUT";
  rq.headers = hdr;
  rq.body_file = path;
  rq.body_off = off;
  rq.body_file_len = len;
  rq.progress = up_prog;
  up->piece = len;
  rq.user = up;
  FmErr e = net_request(url, &rq, r, cancel);
  wipe(hdr, sizeof hdr);
  return e;
}

/* Nextcloud chunked upload v2. Returns FM_ERR_UNSUPPORTED (nothing sent)
** when the server does not offer it, so the caller falls back to one PUT. */
static FmErr nc_chunked(const Dav *d, const char *dest_url, const char *path, i64 size, UpProg *up, char *err,
                        size_t errcap, volatile int *cancel) {
  const char *files = strstr(d->base, "/remote.php/dav/files/");
  if (!files) return FM_ERR_UNSUPPORTED;
  const char *userseg = files + strlen("/remote.php/dav/files/");
  size_t ul = strcspn(userseg, "/");
  if (!ul) return FM_ERR_UNSUPPORTED;
  static volatile int s_seq;
  char root[DAV_URL_MAX], url[DAV_URL_MAX], extra[DAV_URL_MAX + 200];
  fm_snprintf(root, sizeof root, "%.*s/remote.php/dav/uploads/%.*s/mmcfm-%llx-%x/", (int)(files - d->base), d->base,
              (int)ul, userseg, (unsigned long long)plat_time_unix(), (unsigned)(++s_seq) ^ (unsigned)plat_now_ms());
  fm_snprintf(extra, sizeof extra, "Destination: %s\r\nOC-Total-Length: %lld\r\n", dest_url, (long long)size);
  FmNetResp r;
  FmErr e = dav_req(d, "MKCOL", root, extra, NULL, &r, cancel);
  if (e != FM_OK || r.status != 201) {
    FmErr ret = e == FM_ERR_CANCEL ? e : FM_ERR_UNSUPPORTED;
    net_resp_free(&r);
    return ret;
  }
  net_resp_free(&r);
  int part = 0;
  i64 cs = g_dav_chunk_size ? g_dav_chunk_size : DAV_CHUNK;
  for (i64 off = 0; off < size; off += cs) {
    i64 n = FM_MIN(cs, size - off);
    fm_snprintf(url, sizeof url, "%s%05d", root, ++part);
    up->base = (u64)off;
    e = put_file(d, url, extra, path, off, n, up, &r, cancel);
    if (e != FM_OK || !ok2xx(&r)) {
      e = e == FM_ERR_CANCEL ? e : dav_fail(&r, e, err, errcap);
      net_resp_free(&r);
      goto fail;
    }
    net_resp_free(&r);
  }
  fm_snprintf(url, sizeof url, "%s.file", root);
  fm_snprintf(extra, sizeof extra, "Destination: %s\r\nOC-Total-Length: %lld\r\nOverwrite: T\r\n", dest_url,
              (long long)size);
  e = dav_req(d, "MOVE", url, extra, NULL, &r, cancel);
  if (e == FM_OK && ok2xx(&r)) { net_resp_free(&r); return FM_OK; }
  e = e == FM_ERR_CANCEL ? e : dav_fail(&r, e, err, errcap);
  net_resp_free(&r);
fail:;
  volatile int nocancel = 0;
  if (dav_req(d, "DELETE", root, NULL, NULL, &r, &nocancel) == FM_OK) net_resp_free(&r);
  return e;
}

static FmErr w_upload(FmCloudAcct *a, const char *dir_id, const char *local_path, const char *name, FmCloudEntry *out,
                      FmNetProgress cb, void *user, char *err, size_t errcap, volatile int *cancel) {
  FmStat st;
  if (!plat_stat(local_path, &st)) { fm_strlcpy(err, "Cannot read the file to upload.", errcap); return FM_ERR_IO; }
  Dav d;
  FmErr e = dav_open(a, &d, err, errcap);
  if (e) return e;
  char id[CLOUD_ID_MAX], url[DAV_URL_MAX];
  int n = fm_snprintf(id, sizeof id, "%s%s%s", dir_id ? dir_id : "",
                      dir_id && *dir_id && dir_id[strlen(dir_id) - 1] != '/' ? "/" : "", name);
  if (n < 0 || (size_t)n >= sizeof id || !dav_url(&d, id, url, sizeof url)) {
    dav_close(&d);
    fm_strlcpy(err, "The path is too long.", errcap);
    return FM_ERR_FORMAT;
  }
  UpProg up = { cb, user, 0, st.size, 0 };
  i64 minc = g_dav_chunk_min ? g_dav_chunk_min : (100ll << 20);
  e = FM_ERR_UNSUPPORTED;
  if ((i64)st.size > minc) e = nc_chunked(&d, url, local_path, (i64)st.size, &up, err, errcap, cancel);
  if (e == FM_ERR_UNSUPPORTED) {
    FmNetResp r;
    up.base = 0;
    e = put_file(&d, url, NULL, local_path, 0, (i64)st.size, &up, &r, cancel);
    if (e != FM_OK || !ok2xx(&r)) e = e == FM_ERR_CANCEL ? e : dav_fail(&r, e, err, errcap);
    net_resp_free(&r);
  }
  if (e == FM_ERR_CANCEL) fm_strlcpy(err, "Cancelled.", errcap);
  if (e == FM_OK && out) {
    memset(out, 0, sizeof *out);
    fm_strlcpy(out->id, id, sizeof out->id);
    fm_strlcpy(out->name, name, sizeof out->name);
    out->size = st.size;
    out->mtime = plat_time_unix();
  }
  dav_close(&d);
  return e;
}

static FmErr w_mkdir(FmCloudAcct *a, const char *parent_id, const char *name, FmCloudEntry *out, char *err,
                     size_t errcap, volatile int *cancel) {
  Dav d;
  FmErr e = dav_open(a, &d, err, errcap);
  if (e) return e;
  char id[CLOUD_ID_MAX], url[DAV_URL_MAX];
  int n = fm_snprintf(id, sizeof id, "%s%s%s/", parent_id ? parent_id : "",
                      parent_id && *parent_id && parent_id[strlen(parent_id) - 1] != '/' ? "/" : "", name);
  if (n < 0 || (size_t)n >= sizeof id || !dav_url(&d, id, url, sizeof url)) {
    dav_close(&d);
    fm_strlcpy(err, "The path is too long.", errcap);
    return FM_ERR_FORMAT;
  }
  FmNetResp r;
  e = dav_req(&d, "MKCOL", url, NULL, NULL, &r, cancel);
  if (e == FM_OK && r.status == 405) {
    fm_strlcpy(err, "A folder or file with that name already exists.", errcap);
    e = FM_ERR_EXISTS;
  } else if (e == FM_OK && r.status == 409) {
    fm_strlcpy(err, "The parent folder is gone (HTTP 409).", errcap);
    e = FM_ERR_NOT_FOUND;
  } else if (e != FM_OK || !ok2xx(&r)) {
    e = dav_fail(&r, e, err, errcap);
  }
  net_resp_free(&r);
  if (e == FM_OK && out) {
    memset(out, 0, sizeof *out);
    fm_strlcpy(out->id, id, sizeof out->id);
    fm_strlcpy(out->name, name, sizeof out->name);
    out->dir = true;
    out->mtime = plat_time_unix();
  }
  dav_close(&d);
  return e;
}

static FmErr w_remove(FmCloudAcct *a, const FmCloudEntry *ent, char *err, size_t errcap, volatile int *cancel) {
  Dav d;
  FmErr e = dav_open(a, &d, err, errcap);
  if (e) return e;
  char url[DAV_URL_MAX];
  if (!*ent->id || !dav_url(&d, ent->id, url, sizeof url)) {
    dav_close(&d);
    fm_strlcpy(err, "Cannot delete that.", errcap);
    return FM_ERR_ACCESS;
  }
  FmNetResp r;
  e = dav_req(&d, "DELETE", url, NULL, NULL, &r, cancel);
  if (e == FM_OK && r.status == 404) e = FM_OK;   /* already gone */
  else if (e != FM_OK || !ok2xx(&r)) e = dav_fail(&r, e, err, errcap);
  net_resp_free(&r);
  dav_close(&d);
  return e;
}

/* MOVE ent to the id `to` (no overwrite). */
static FmErr dav_move_to(FmCloudAcct *a, const FmCloudEntry *ent, const char *to, char *err, size_t errcap,
                         volatile int *cancel) {
  Dav d;
  FmErr e = dav_open(a, &d, err, errcap);
  if (e) return e;
  char url[DAV_URL_MAX], dest[DAV_URL_MAX], extra[DAV_URL_MAX + 64];
  if (!*ent->id || !dav_url(&d, ent->id, url, sizeof url) || !dav_url(&d, to, dest, sizeof dest)) {
    dav_close(&d);
    fm_strlcpy(err, "The path is too long.", errcap);
    return FM_ERR_FORMAT;
  }
  fm_snprintf(extra, sizeof extra, "Destination: %s\r\nOverwrite: F\r\n", dest);
  FmNetResp r;
  e = dav_req(&d, "MOVE", url, extra, NULL, &r, cancel);
  if (e == FM_OK && r.status == 412) {
    fm_strlcpy(err, "An item with that name is already there.", errcap);
    e = FM_ERR_EXISTS;
  } else if (e != FM_OK || !ok2xx(&r)) {
    e = dav_fail(&r, e, err, errcap);
  }
  net_resp_free(&r);
  dav_close(&d);
  return e;
}

/* "a/b/c/" -> parent "a/b/" + base "c" */
static void split_id(const char *id, char *parent, size_t pcap, char *base, size_t bcap) {
  char t[CLOUD_ID_MAX];
  fm_strlcpy(t, id, sizeof t);
  size_t n = strlen(t);
  if (n && t[n - 1] == '/') t[--n] = 0;
  char *s = strrchr(t, '/');
  fm_strlcpy(base, s ? s + 1 : t, bcap);
  if (s) { s[1] = 0; fm_strlcpy(parent, t, pcap); }
  else if (pcap) parent[0] = 0;
}

static FmErr w_rename(FmCloudAcct *a, const FmCloudEntry *ent, const char *new_name, char *err, size_t errcap,
                      volatile int *cancel) {
  char parent[CLOUD_ID_MAX], base[CLOUD_ID_MAX], to[CLOUD_ID_MAX];
  if (!*new_name || strchr(new_name, '/')) { fm_strlcpy(err, "Names cannot contain '/'.", errcap); return FM_ERR_FORMAT; }
  split_id(ent->id, parent, sizeof parent, base, sizeof base);
  int n = fm_snprintf(to, sizeof to, "%s%s%s", parent, new_name, ent->dir ? "/" : "");
  if (n < 0 || (size_t)n >= sizeof to) { fm_strlcpy(err, "The name is too long.", errcap); return FM_ERR_FORMAT; }
  return dav_move_to(a, ent, to, err, errcap, cancel);
}

static FmErr w_move(FmCloudAcct *a, const FmCloudEntry *ent, const char *new_parent_id, char *err, size_t errcap,
                    volatile int *cancel) {
  char parent[CLOUD_ID_MAX], base[CLOUD_ID_MAX], to[CLOUD_ID_MAX];
  split_id(ent->id, parent, sizeof parent, base, sizeof base);
  const char *np = new_parent_id ? new_parent_id : "";
  int n = fm_snprintf(to, sizeof to, "%s%s%s%s", np, *np && np[strlen(np) - 1] != '/' ? "/" : "", base,
                      ent->dir ? "/" : "");
  if (n < 0 || (size_t)n >= sizeof to) { fm_strlcpy(err, "The path is too long.", errcap); return FM_ERR_FORMAT; }
  if (ent->dir && !strncmp(to, ent->id, strlen(ent->id))) {
    fm_strlcpy(err, "A folder cannot move into itself.", errcap);
    return FM_ERR_ACCESS;
  }
  return dav_move_to(a, ent, to, err, errcap, cancel);
}

static FmErr w_quota(FmCloudAcct *a, u64 *used, u64 *total, char *err, size_t errcap, volatile int *cancel) {
  Dav d;
  FmErr e = dav_open(a, &d, err, errcap);
  if (e) return e;
  FmNetResp r;
  *used = *total = 0;
  e = dav_req(&d, "PROPFIND", d.base, PROPFIND_HDR("0"), kQuota, &r, cancel);
  if (e == FM_OK && r.status == 207) {
    if (!dav_parse_quota((const char *)r.data, r.len, used, total)) {
      fm_strlcpy(err, "The server does not report its storage use.", errcap);
      e = FM_ERR_UNSUPPORTED;
    }
  } else {
    e = dav_fail(&r, e, err, errcap);
  }
  net_resp_free(&r);
  dav_close(&d);
  return e;
}

static FmErr w_stream_url(FmCloudAcct *a, const FmCloudEntry *ent, char *url, size_t urlcap, char *headers,
                          size_t hcap, char *err, size_t errcap, volatile int *cancel) {
  FM_UNUSED(cancel);
  Dav d;
  FmErr e = dav_open(a, &d, err, errcap);
  if (e) return e;
  if (!dav_url(&d, ent->id, url, urlcap) || strlen(d.auth) + 1 > hcap) {
    dav_close(&d);
    fm_strlcpy(err, "The path is too long.", errcap);
    return FM_ERR_FORMAT;
  }
  fm_strlcpy(headers, d.auth, hcap);
  dav_close(&d);
  return FM_OK;
}

const FmCloud g_cloud_webdav = {
  "webdav", "WebDAV", IC_CLOUD, CLOUD_PASSWORD | CLOUD_SERVER | CLOUD_UPLOAD | CLOUD_STREAM,
  w_login, w_list, w_download, w_upload, w_mkdir, w_remove, w_rename, w_move, w_quota, w_stream_url, NULL,
  "Nextcloud, ownCloud, pCloud, Koofr, Yandex Disk, a NAS: server address, user, app password",
};
