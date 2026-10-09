/* fcloud_oauth.c -- what the OAuth cloud adapters (Google Drive, Dropbox,
** OneDrive) share: the signed-in session, authorised requests with one
** automatic refresh, downloads into .part files, error sentences and small
** JSON / time helpers (foauth_int.h).
**
** Design decisions:
**   - A stale access token is refreshed before the call (oa_open) and again
**     when the service still answers 401 (revoked early, clock skew); the
**     new tokens go back into FmCloudAcct.session with session_changed set so
**     fcloud.c saves them. A refused refresh means "Signed out: sign in again"
**     (FM_ERR_PASSWORD), which the UI turns into a sign-in prompt.
**   - Error sentences: the service's own JSON message when there is one
**     (Google/Microsoft error.message, Dropbox error_summary), mapped to the
**     FmErr the operation engine understands (not found, exists, full).
**     Plain status sentences come from cloud_http_error (fcloud.c).
**   - Downloads follow a 3xx by hand when asked (OneDrive's /content answers
**     302 to a pre-authorised URL that must not get the bearer token).
*/
#include "foauth_int.h"
#include "fcrypt.h"
#include "fjson.h"
#include "fplat.h"

/* ---- small helpers ------------------------------------------------------------------ */

char g_oa_test_base[64];

/* self test: "https://host/p" -> "<base>/host/p" so a loopback mock plays the service */
static const char *test_url(const char *url, char **tmp) {
  *tmp = NULL;
  if (!g_oa_test_base[0] || fm_strnicmp(url, "https://", 8) != 0) return url;
  size_t n = strlen(g_oa_test_base) + strlen(url) + 2;
  *tmp = (char *)fm_alloc(n);
  fm_snprintf(*tmp, n, "%s/%s", g_oa_test_base, url + 8);
  return *tmp;
}

bool oa_ok(const FmNetResp *r) { return r->status >= 200 && r->status < 300; }

void oa_json_quote(const char *s, char *out, size_t cap, bool ascii) {
  static const char hx[] = "0123456789abcdef";
  size_t o = 0;
  if (cap < 3) { if (cap) out[0] = 0; return; }
  out[o++] = '"';
  while (s && *s && o + 14 < cap) {
    u8 c = (u8)*s;
    if (c == '"' || c == '\\') {
      out[o++] = '\\';
      out[o++] = (char)c;
      s++;
    } else if (c < 0x20 || (ascii && c >= 0x7F)) {
      u32 cp = c;
      if (c >= 0x80) s += utf8_decode(s, &cp);
      else s++;
      u32 units[2];
      int nu = 1;
      units[0] = cp;
      if (cp >= 0x10000) {
        cp -= 0x10000;
        units[0] = 0xD800 + (cp >> 10);
        units[1] = 0xDC00 + (cp & 0x3FF);
        nu = 2;
      }
      for (int i = 0; i < nu; i++) {
        out[o++] = '\\';
        out[o++] = 'u';
        out[o++] = hx[units[i] >> 12 & 15];
        out[o++] = hx[units[i] >> 8 & 15];
        out[o++] = hx[units[i] >> 4 & 15];
        out[o++] = hx[units[i] & 15];
      }
    } else {
      out[o++] = *s++;
    }
  }
  out[o++] = '"';
  out[o] = 0;
}

bool oa_prog(void *user, u64 done, u64 total) {
  OaProg *p = (OaProg *)user;
  if (!p->cb || total != p->chunk || done > p->chunk) return true;   /* the reply part: not ours */
  return p->cb(p->user, p->base + done, p->total);
}

/* ---- errors ------------------------------------------------------------------------- */

FmErr oa_error_text(const char *service, int status, const char *body, size_t len, char *out, size_t cap) {
  char msg[200], reason[96];
  msg[0] = reason[0] = 0;
  FmJson j;
  if (body && len && body[0] == '{' && json_parse(&j, body, len) == FM_OK) {
    const FmJsonNode *r = json_root(&j);
    const FmJsonNode *e = json_get(r, "error");
    if (e && e->type == JSON_OBJ) {
      fm_strlcpy(msg, json_str(json_get(e, "message"), ""), sizeof msg);
      fm_strlcpy(reason, json_str(json_path(e, "errors.0.reason"), json_str(json_get(e, "code"), "")),
                 sizeof reason);
      if (!reason[0]) fm_strlcpy(reason, json_str(json_get(e, "status"), ""), sizeof reason);
    } else if (e && e->type == JSON_STR) {
      fm_strlcpy(reason, e->s, sizeof reason);
      fm_strlcpy(msg, json_str(json_get(r, "error_description"), ""), sizeof msg);
    }
    const char *sum = json_str(json_get(r, "error_summary"), "");      /* Dropbox: "path/not_found/.." */
    if (*sum) {
      fm_strlcpy(reason, sum, sizeof reason);
      if (!msg[0]) fm_strlcpy(msg, sum, sizeof msg);
    }
    json_free(&j);
  } else if (body && len && status >= 400 && status < 500) {        /* Dropbox 400s are plain text */
    size_t n = FM_MIN(len, sizeof msg - 1);
    memcpy(msg, body, n);
    msg[n] = 0;
    for (char *c = msg; *c; c++) if ((u8)*c < ' ') *c = ' ';
  }
  if (status == 401) {
    fm_strlcpy(out, "Signed out: sign in again", cap);
    return FM_ERR_PASSWORD;
  }
  if (status == 507 || !strcmp(reason, "storageQuotaExceeded") || !strcmp(reason, "quotaLimitReached") ||
      strstr(reason, "insufficient_space")) {
    fm_snprintf(out, cap, "Not enough space left in %s", service);
    return FM_ERR_FULL;
  }
  if (status == 404 || !strcmp(reason, "itemNotFound") || strstr(reason, "not_found")) {
    fm_snprintf(out, cap, "Not found in %s (it may have been moved or deleted)", service);
    return FM_ERR_NOT_FOUND;
  }
  if (!strcmp(reason, "nameAlreadyExists") || strstr(reason, "/conflict")) {
    fm_strlcpy(out, "An item with that name is already there", cap);
    return FM_ERR_EXISTS;
  }
  if (!strcmp(reason, "accessNotConfigured") || strstr(msg, "has not been used in project") ||
      !strcmp(reason, "SERVICE_DISABLED")) {
    fm_snprintf(out, cap, "Turn on the %s API for your client id's project in the Google Cloud Console",
                service);
    return FM_ERR_ACCESS;
  }
  if (status == 503 || !strcmp(reason, "rateLimitExceeded") || !strcmp(reason, "userRateLimitExceeded") ||
      strstr(reason, "too_many")) {
    fm_snprintf(out, cap, "%s is busy: try again in a minute", service);
    return FM_ERR_IO;
  }
  FmErr e;
  if (status == 409 && reason[0]) {               /* Dropbox: 409 is any endpoint error, not "exists" */
    fm_snprintf(out, cap, "%s refused the request (HTTP 409)", service);
    e = FM_ERR_IO;
  } else {
    FmNetResp r;                                  /* the generic sentence from fcloud.c */
    memset(&r, 0, sizeof r);
    r.status = status;
    e = cloud_http_error(service, &r, FM_OK, out, cap);
  }
  if (msg[0] && status >= 400 && status < 500 && strlen(out) + 2 < cap) {
    fm_strlcat(out, ": ", cap);
    fm_strlcat(out, msg, cap);
  }
  return e;
}

FmErr oa_fail(OaSess *s, const FmNetResp *r, FmErr e) {
  if (s->said) return s->said;
  if (e != FM_OK || !r || !r->status) return cloud_http_error(s->service, r, e, s->err, s->errcap);
  return oa_error_text(s->service, r->status, (const char *)r->data, r->data ? r->len : 0, s->err, s->errcap);
}

/* ---- session ----------------------------------------------------------------------------- */

static void keep_tokens(OaSess *s) {
  oauth_save(&s->tok, s->a->session, sizeof s->a->session);
  s->a->session_changed = true;
}

static FmErr oa_refresh(OaSess *s) {
  FmErr e = oauth_refresh(&s->cfg, &s->tok, s->err, s->errcap, s->cancel);
  if (e == FM_OK) keep_tokens(s);
  else s->said = e;
  return e;
}

FmErr oa_login(FmCloudAcct *a, const FmOAuthCfg *c, const char *service, char *err, size_t errcap,
               volatile int *cancel) {
  FM_UNUSED(service);
  FmOAuthTok t;
  memset(&t, 0, sizeof t);
  FmErr e = oauth_login(c, &t, err, errcap, cancel);
  if (e == FM_OK) {
    oauth_save(&t, a->session, sizeof a->session);
    a->session_changed = true;
  }
  wipe(&t, sizeof t);
  return e;
}

FmErr oa_open(OaSess *s, FmCloudAcct *a, const FmOAuthCfg *c, const char *service, char *err, size_t errcap,
              volatile int *cancel) {
  memset(s, 0, sizeof *s);
  s->a = a;
  s->cfg = *c;
  s->service = service;
  s->err = err;
  s->errcap = errcap;
  s->cancel = cancel;
  if (!oauth_load(&s->tok, a->session)) {
    fm_strlcpy(err, "Signed out: sign in again", errcap);
    return FM_ERR_PASSWORD;
  }
  if (!oauth_fresh(&s->tok)) return oa_refresh(s);
  return FM_OK;
}

FmErr oa_call(OaSess *s, const char *url, const FmNetReq *rq, bool auth, FmNetResp *out) {
  for (int attempt = 0;; attempt++) {
    FmNetReq q = *rq;
    char *hdr = NULL;
    if (auth) {
      size_t n = strlen(s->tok.access) + (rq->headers ? strlen(rq->headers) : 0) + 32;
      hdr = (char *)fm_alloc(n);
      fm_snprintf(hdr, n, "Authorization: Bearer %s\r\n%s", s->tok.access, rq->headers ? rq->headers : "");
      q.headers = hdr;
    }
    char *tu;
    FmErr e = net_request(test_url(url, &tu), &q, out, s->cancel);
    fm_free(tu);
    if (hdr) {
      wipe(hdr, strlen(hdr));
      fm_free(hdr);
    }
    if (e != FM_OK || !auth || out->status != 401 || attempt) return e;
    net_resp_free(out);
    e = oa_refresh(s);                 /* expired early or revoked: one new token, then once more */
    if (e != FM_OK) {
      memset(out, 0, sizeof *out);
      return e;
    }
  }
}

FmErr oa_send(OaSess *s, const char *method, const char *url, const char *type, const char *body,
              const char *headers, FmNetResp *out) {
  char hdr[4096];
  hdr[0] = 0;
  if (type) fm_snprintf(hdr, sizeof hdr, "Content-Type: %s\r\n", type);
  if (headers) fm_strlcat(hdr, headers, sizeof hdr);
  FmNetReq q;
  memset(&q, 0, sizeof q);
  q.method = method;
  q.headers = hdr[0] ? hdr : NULL;
  q.body = body;
  q.body_len = body ? strlen(body) : 0;
  return oa_call(s, url, &q, true, out);
}

/* a failed reply written to a file: its first 64 KB back into memory for the message */
static void reply_from_file(FmNetResp *r, const char *path) {
  fm_free(r->data);
  r->data = NULL;
  r->len = 0;
  FILE *f = fm_fopen(path, "rb");
  if (!f) return;
  r->data = (u8 *)fm_alloc(65537);
  r->len = fread(r->data, 1, 65536, f);
  r->data[r->len] = 0;
  fclose(f);
}

FmErr oa_download(OaSess *s, const char *url, const FmNetReq *rq, const char *local_path, FmNetProgress cb,
                  void *user) {
  char part[FM_PATH_MAX];
  if (fm_snprintf(part, sizeof part, "%s.part", local_path) >= (int)sizeof part) {
    fm_strlcpy(s->err, "The file name is too long", s->errcap);
    return FM_ERR_IO;
  }
  FmNetReq q = *rq;
  q.out_file = part;
  q.progress = cb;
  q.user = user;
  FmNetResp r;
  FmErr e = oa_call(s, url, &q, true, &r);
  if (e == FM_OK && r.status >= 300 && r.status < 400) {
    char *loc = (char *)fm_alloc(8192);
    if (net_resp_header(&r, "Location", loc, 8192) && !fm_strnicmp(loc, "http", 4)) {
      net_resp_free(&r);
      FmNetReq g;
      memset(&g, 0, sizeof g);
      g.out_file = part;
      g.progress = cb;
      g.user = user;
      char *tu;
      e = net_request(test_url(loc, &tu), &g, &r, s->cancel);   /* pre-authorised: no token */
      fm_free(tu);
    }
    fm_free(loc);
  }
  if (e == FM_OK && oa_ok(&r)) {
    net_resp_free(&r);
    plat_remove_file(local_path);
    if (plat_rename(part, local_path) != FM_OK) {
      plat_remove_file(part);
      fm_strlcpy(s->err, "Cannot save the downloaded file", s->errcap);
      return FM_ERR_IO;
    }
    return FM_OK;
  }
  if (e == FM_OK) reply_from_file(&r, part);
  plat_remove_file(part);
  e = oa_fail(s, &r, e);
  net_resp_free(&r);
  return e;
}
