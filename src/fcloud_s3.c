/* fcloud_s3.c -- S3-compatible storage for the cloud panels (fcloud.h):
** Amazon S3, Backblaze B2 (S3 API), Cloudflare R2, Wasabi, MinIO.
**
** The account's server is "endpoint|region|bucket" ("https://s3.eu-west-1.
** amazonaws.com|eu-west-1|photos", "https://<acct>.r2.cloudflarestorage.com|
** auto|photos"); user is the access key id, secret the secret key. Ids are
** object keys; folders are key prefixes ending with '/' ("" = the bucket).
**
** Design decisions:
**   - Path-style addressing (endpoint/bucket/key): it works on every
**     S3-compatible service and with bucket names that contain dots, and
**     needs no DNS per bucket. AWS still serves it for existing endpoints.
**   - AWS Signature V4 in the Authorization header; uploads are signed
**     with UNSIGNED-PAYLOAD so the file streams (body_file) instead of being
**     hashed first; everything else signs the real body hash. The secret
**     never leaves the device, yet plain http is still only allowed on the
**     user's own network (the data would travel in the clear).
**   - Files up to 64 MB are one PUT; bigger ones a multipart upload in
**     16 MB parts (grown so a file fits in 10,000 parts), aborted on any
**     failure or cancel so no invisible parts stay billed.
**   - S3 has no folders: mkdir writes an empty "name/" marker, listing uses
**     delimiter '/' (CommonPrefixes are the folders), and folder delete /
**     rename / move work key by key over a paged recursive listing.
**     Rename and move are server-side copies (no data through the device),
**     multipart UploadPartCopy above 5 GB.
**   - Listings ask for encoding-type=url so keys with control characters
**     survive XML; they are decoded (form style, '+' = space, as AWS and
**     the SDKs do) only when the reply says it encoded.
**   - stream_url is a presigned GET (query-string SigV4, valid 1 hour), so
**     the players need no headers and can seek with Range.
*/
#include "fcloud.h"
#include "fcloud_dav.h"
#include "fcrypt.h"
#include "fxml.h"
#include "fplat.h"

#define S3_NAME "S3 storage"
#define S3_URL_MAX 4096
#define EMPTY_SHA "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
#define S3_COPY_MAX (5ll << 30)       /* CopyObject limit */
#define S3_COPY_PART (512ll << 20)

i64 g_s3_single_max, g_s3_part_size, g_s3_copy_max;   /* test hooks (fcloud_dav.h) */
int g_s3_max_keys;

/* ---- SigV4 ---------------------------------------------------------------------------- */

static void hexs(const u8 *b, int n, char *out) {
  static const char hx[] = "0123456789abcdef";
  for (int i = 0; i < n; i++) { out[2 * i] = hx[b[i] >> 4]; out[2 * i + 1] = hx[b[i] & 15]; }
  out[2 * n] = 0;
}

static void sha256_hex(const void *p, size_t n, char out[65]) {
  FmSha256 s;
  u8 h[32];
  sha256_init(&s);
  sha256_update(&s, p, n);
  sha256_final(&s, h);
  hexs(h, 32, out);
}

/* a growable string for the canonical request */
typedef struct Sb { char *p; size_t n, cap; } Sb;

static void sb_put(Sb *b, const char *s, size_t n) {
  if (b->n + n + 1 > b->cap) {
    b->cap = (b->n + n + 1) * 2;
    b->p = (char *)fm_realloc(b->p, b->cap);
  }
  memcpy(b->p + b->n, s, n);
  b->n += n;
  b->p[b->n] = 0;
}
static void sb_add(Sb *b, const char *s) { sb_put(b, s, strlen(s)); }

static int hdr_cmp(const void *a, const void *b) {
  return strcmp(((const S3Hdr *)a)->name, ((const S3Hdr *)b)->name);
}

void s3_sign(const S3Sig *sg, char sig[65], char *auth, size_t acap) {
  S3Hdr h[16];
  int nh = FM_MIN(sg->nhdr, 16);
  memcpy(h, sg->hdr, (size_t)nh * sizeof *h);
  qsort(h, (size_t)nh, sizeof *h, hdr_cmp);
  Sb cr = { NULL, 0, 0 }, sh = { NULL, 0, 0 };
  sb_add(&cr, sg->method); sb_add(&cr, "\n");
  sb_add(&cr, sg->uri); sb_add(&cr, "\n");
  sb_add(&cr, sg->query ? sg->query : ""); sb_add(&cr, "\n");
  for (int i = 0; i < nh; i++) {
    const char *v = h[i].value;
    size_t vl;
    while (*v == ' ' || *v == '\t') v++;
    vl = strlen(v);
    while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
    sb_add(&cr, h[i].name); sb_add(&cr, ":"); sb_put(&cr, v, vl); sb_add(&cr, "\n");
    if (i) sb_add(&sh, ";");
    sb_add(&sh, h[i].name);
  }
  sb_add(&cr, "\n");
  sb_add(&cr, sh.p ? sh.p : ""); sb_add(&cr, "\n");
  sb_add(&cr, sg->payload);
  char crh[65], date[9], scope[200], sts[400];
  sha256_hex(cr.p, cr.n, crh);
  memcpy(date, sg->amzdate, 8);
  date[8] = 0;
  fm_snprintf(scope, sizeof scope, "%s/%s/%s/aws4_request", date, sg->region, sg->service);
  fm_snprintf(sts, sizeof sts, "AWS4-HMAC-SHA256\n%s\n%s\n%s", sg->amzdate, scope, crh);
  u8 k[32], k2[32];
  char ks[600];
  int kn = fm_snprintf(ks, sizeof ks, "AWS4%s", sg->secret);
  hmac_sha256((const u8 *)ks, (size_t)kn, date, 8, k);
  hmac_sha256(k, 32, sg->region, strlen(sg->region), k2);
  hmac_sha256(k2, 32, sg->service, strlen(sg->service), k);
  hmac_sha256(k, 32, "aws4_request", 12, k2);
  hmac_sha256(k2, 32, sts, strlen(sts), k);
  hexs(k, 32, sig);
  if (auth)
    fm_snprintf(auth, acap, "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s", sg->key_id, scope,
                sh.p ? sh.p : "", sig);
  wipe(ks, sizeof ks);
  wipe(k, sizeof k);
  wipe(k2, sizeof k2);
  fm_free(cr.p);
  fm_free(sh.p);
}

typedef struct QPair { char *k, *v; } QPair;

static int qp_cmp(const void *a, const void *b) {
  const QPair *x = (const QPair *)a, *y = (const QPair *)b;
  int c = strcmp(x->k, y->k);
  return c ? c : strcmp(x->v, y->v);
}

void s3_canon_query(const char *const *kv, char *out, size_t cap) {
  QPair q[16];
  int n = 0;
  if (cap) out[0] = 0;
  for (; kv && kv[0] && n < 16; kv++) {
    const char *eq = strchr(kv[0], '=');
    size_t kl = eq ? (size_t)(eq - kv[0]) : strlen(kv[0]);
    char name[128];
    fm_strlcpy(name, kv[0], FM_MIN(kl + 1, sizeof name));
    const char *val = eq ? eq + 1 : "";
    size_t vc = strlen(val) * 3 + 8, nc = strlen(name) * 3 + 8;
    q[n].k = (char *)fm_alloc(nc);
    q[n].v = (char *)fm_alloc(vc);
    net_urlencode(name, q[n].k, nc);
    net_urlencode(val, q[n].v, vc);
    n++;
  }
  qsort(q, (size_t)n, sizeof *q, qp_cmp);
  for (int i = 0; i < n; i++) {
    if (i) fm_strlcat(out, "&", cap);
    fm_strlcat(out, q[i].k, cap);
    fm_strlcat(out, "=", cap);
    fm_strlcat(out, q[i].v, cap);
    fm_free(q[i].k);
    fm_free(q[i].v);
  }
}

void s3_presign(const char *scheme_host, const char *host_hdr, const char *uri, const char *region,
                const char *key_id, const char *secret, const char *amzdate, int expires, char *out, size_t cap) {
  char cred[400], exp[16], q[1400], sig[65];
  char a1[] = "X-Amz-Algorithm=AWS4-HMAC-SHA256";
  char a2[440], a3[40], a4[40];
  fm_snprintf(cred, sizeof cred, "%s/%.8s/%s/s3/aws4_request", key_id, amzdate, region);
  fm_snprintf(exp, sizeof exp, "%d", expires);
  fm_snprintf(a2, sizeof a2, "X-Amz-Credential=%s", cred);
  fm_snprintf(a3, sizeof a3, "X-Amz-Date=%s", amzdate);
  fm_snprintf(a4, sizeof a4, "X-Amz-Expires=%s", exp);
  const char *kv[] = { a1, a2, a3, a4, "X-Amz-SignedHeaders=host", NULL };
  s3_canon_query(kv, q, sizeof q);
  S3Hdr h[1] = { { "host", host_hdr } };
  S3Sig sg = { "GET", uri, q, h, 1, "UNSIGNED-PAYLOAD", amzdate, region, "s3", key_id, secret };
  s3_sign(&sg, sig, NULL, 0);
  fm_snprintf(out, cap, "%s%s?%s&X-Amz-Signature=%s", scheme_host, uri, q, sig);
}

/* ---- replies ---------------------------------------------------------------------------- */

static bool xname(const FmXml *x, const char *local) {    /* local name, any prefix */
  const char *n = x->name;
  size_t len = x->name_len;
  const char *c = memchr(n, ':', len);
  if (c) { len -= (size_t)(c + 1 - n); n = c + 1; }
  return len == strlen(local) && !memcmp(n, local, len);
}

bool s3_parse_error(const char *xml, size_t len, char *code, size_t ccap, char *msg, size_t mcap) {
  FmXml x;
  bool in = false;
  if (ccap) code[0] = 0;
  if (mcap) msg[0] = 0;
  if (!xml) return false;
  xml_init(&x, xml, len);
  while (xml_next(&x) != XML_EOF) {
    if (x.type != XML_START) continue;
    if (xname(&x, "Error")) in = true;
    else if (in && xname(&x, "Code")) xml_inner_text(&x, code, ccap);
    else if (in && xname(&x, "Message")) xml_inner_text(&x, msg, mcap);
  }
  return in && ccap && code[0];
}

int s3_parse_list(const char *xml, size_t len, const char *prefix, FmCloudList *out, char *next, size_t ncap) {
  FmXml x;
  bool url_enc = false, result = false, truncated = false;
  if (ncap) next[0] = 0;
  /* EncodingType may come after the keys: look for it first */
  xml_init(&x, xml, len);
  while (xml_next(&x) != XML_EOF) {
    if (x.type == XML_START && xname(&x, "ListBucketResult")) result = true;
    if (x.type == XML_START && xname(&x, "EncodingType")) {
      char t[16];
      xml_inner_text(&x, t, sizeof t);
      url_enc = !fm_stricmp(t, "url");
    }
  }
  if (!result) return -1;
  size_t pl = strlen(prefix);
  int added = 0;
  char key[CLOUD_ID_MAX * 3], dkey[CLOUD_ID_MAX * 2], size[32], mod[64], etag[96];
  int in = 0;                                     /* 1 Contents, 2 CommonPrefixes */
  xml_init(&x, xml, len);
  while (xml_next(&x) != XML_EOF) {
    if (x.type == XML_START) {
      if (xname(&x, "Contents") || xname(&x, "CommonPrefixes")) {
        in = xname(&x, "Contents") ? 1 : 2;
        key[0] = size[0] = mod[0] = etag[0] = 0;
      } else if (in == 1 && xname(&x, "Key")) xml_inner_text(&x, key, sizeof key);
      else if (in == 2 && xname(&x, "Prefix")) xml_inner_text(&x, key, sizeof key);
      else if (in == 1 && xname(&x, "Size")) xml_inner_text(&x, size, sizeof size);
      else if (in == 1 && xname(&x, "LastModified")) xml_inner_text(&x, mod, sizeof mod);
      else if (in == 1 && xname(&x, "ETag")) xml_inner_text(&x, etag, sizeof etag);
      else if (!in && xname(&x, "IsTruncated")) {
        char t[8];
        xml_inner_text(&x, t, sizeof t);
        truncated = !fm_stricmp(t, "true");
      } else if (!in && xname(&x, "NextContinuationToken")) xml_inner_text(&x, next, ncap);
    } else if (x.type == XML_END && in && (xname(&x, "Contents") || xname(&x, "CommonPrefixes"))) {
      bool dir = in == 2;
      in = 0;
      if (url_enc) {                              /* form encoding: '+' is a space, a real '+' is %2B */
        for (char *k = key; *k; k++) if (*k == '+') *k = ' ';
        dav_pct_decode(key, strlen(key), dkey, sizeof dkey);
      }
      else fm_strlcpy(dkey, key, sizeof dkey);
      size_t kl = strlen(dkey);
      if (kl >= CLOUD_ID_MAX || kl <= pl || strncmp(dkey, prefix, pl)) continue;   /* the marker, or foreign */
      char name[CLOUD_ID_MAX];
      fm_strlcpy(name, dkey + pl, sizeof name);
      size_t nl = strlen(name);
      while (nl && name[nl - 1] == '/') name[--nl] = 0;
      if (!nl) continue;
      FmCloudEntry *e = cloud_list_add(out);
      fm_strlcpy(e->id, dkey, sizeof e->id);
      fm_strlcpy(e->name, name, sizeof e->name);
      e->dir = dir;
      if (!dir) {
        e->size = dav_atou64(size);
        e->mtime = mod[0] ? cloud_parse_time(mod) : 0;
        const char *t = etag;
        size_t tl = strlen(t);
        if (tl >= 2 && t[0] == '"' && t[tl - 1] == '"') { t++; tl -= 2; }
        if (tl) fm_snprintf(e->hash, sizeof e->hash, "etag:%.*s", (int)FM_MIN(tl, (size_t)60), t);
      }
      added++;
    }
  }
  if (!truncated && ncap) next[0] = 0;
  return added;
}

/* ---- the account ------------------------------------------------------------------------- */

typedef struct S3 {
  char scheme_host[400];   /* "https://s3.eu-west-1.amazonaws.com" (+ ":port" when not the default) */
  char host[300];          /* the Host header: "s3.eu-west-1.amazonaws.com", "127.0.0.1:9000" */
  char root[300];          /* "<endpoint path>/<bucket>", encoded: the canonical URI of the bucket */
  size_t bucket_at;        /* where "/<bucket>" starts in root */
  char region[64];
  char key_id[256], secret[512];
} S3;

static FmErr s3_open(const FmCloudAcct *a, S3 *s, char *err, size_t cap) {
  char ep[512], region[64] = "", bucket[256] = "";
  memset(s, 0, sizeof *s);
  const char *p = a->server, *b1 = strchr(p, '|');
  fm_strlcpy(ep, p, b1 ? FM_MIN((size_t)(b1 - p) + 1, sizeof ep) : sizeof ep);
  if (b1) {
    const char *b2 = strchr(b1 + 1, '|');
    fm_strlcpy(region, b1 + 1, b2 ? FM_MIN((size_t)(b2 - b1), sizeof region) : sizeof region);
    if (b2) fm_strlcpy(bucket, b2 + 1, sizeof bucket);
  }
  /* trim blanks */
  char *fields[3] = { ep, region, bucket };
  for (int i = 0; i < 3; i++) {
    char *f = fields[i];
    size_t b = 0, n = strlen(f);
    while (b < n && (f[b] == ' ' || f[b] == '\t')) b++;
    while (n > b && (f[n - 1] == ' ' || f[n - 1] == '\t' || f[n - 1] == '/' || f[n - 1] == '\r' || f[n - 1] == '\n'))
      n--;
    memmove(f, f + b, n - b);
    f[n - b] = 0;
  }
  if (!region[0]) fm_strlcpy(region, "us-east-1", sizeof region);
  if (!ep[0]) fm_snprintf(ep, sizeof ep, "s3.%s.amazonaws.com", region);
  if (!bucket[0]) { fm_strlcpy(err, "Enter the bucket name.", cap); return FM_ERR_FORMAT; }
  if (!a->user[0] || !a->secret[0]) { fm_strlcpy(err, "Enter the access key id and the secret key.", cap); return FM_ERR_PASSWORD; }
  char url[600];
  if (!strstr(ep, "://")) fm_snprintf(url, sizeof url, "https://%s", ep);
  else fm_strlcpy(url, ep, sizeof url);
  bool https;
  char host[256], port[16];
  const char *path;
  if (!dav_split_url(url, &https, host, sizeof host, port, sizeof port, &path)) {
    fm_strlcpy(err, "The endpoint should look like https://s3.eu-west-1.amazonaws.com", cap);
    return FM_ERR_FORMAT;
  }
  if (!https && !dav_host_is_local(host)) {
    fm_strlcpy(err, "Plain http:// would send your files unencrypted over the internet. Use https:// "
               "(http is only allowed for servers on your own network).", cap);
    return FM_ERR_ACCESS;
  }
  if ((https && !strcmp(port, "443")) || (!https && !strcmp(port, "80"))) port[0] = 0;
  bool v6 = strchr(host, ':') != NULL;
  fm_snprintf(s->host, sizeof s->host, "%s%s%s%s%s", v6 ? "[" : "", host, v6 ? "]" : "", port[0] ? ":" : "", port);
  fm_snprintf(s->scheme_host, sizeof s->scheme_host, "%s://%s", https ? "https" : "http", s->host);
  char ppath[256], epath[300], ebucket[800];
  fm_strlcpy(ppath, path, sizeof ppath);
  ppath[strcspn(ppath, "?#")] = 0;
  size_t pn = strlen(ppath);
  while (pn && ppath[pn - 1] == '/') ppath[--pn] = 0;
  char dpath[256];
  dav_pct_decode(ppath, pn, dpath, sizeof dpath);
  if (!dav_pct_path(dpath, epath, sizeof epath) || !dav_pct_path(bucket, ebucket, sizeof ebucket)) {
    fm_strlcpy(err, "The endpoint or bucket name is too long.", cap);
    return FM_ERR_FORMAT;
  }
  fm_snprintf(s->root, sizeof s->root, "%s/%s", epath, ebucket);
  s->bucket_at = strlen(epath);
  fm_strlcpy(s->region, region, sizeof s->region);
  fm_strlcpy(s->key_id, a->user, sizeof s->key_id);
  fm_strlcpy(s->secret, a->secret, sizeof s->secret);
  return FM_OK;
}

static void s3_close(S3 *s) { wipe(s->secret, sizeof s->secret); }

static void amz_now(char out[20]) {
  int y, mo, d, h, mi, se;
  dav_civil(plat_time_unix(), &y, &mo, &d, &h, &mi, &se);
  fm_snprintf(out, 20, "%04d%02d%02dT%02d%02d%02dZ", y, mo, d, h, mi, se);
}

/* One request. */
typedef struct Call {
  const char *method;
  const char *key;               /* object key (raw), NULL = the bucket */
  const char *const *query;      /* "k=v" pairs, NULL-terminated, or NULL */
  const char *payload;           /* hex sha256 / UNSIGNED-PAYLOAD; NULL = hash of body (or empty) */
  S3Hdr amz[3];                  /* extra signed x-amz-* headers (name NULL = none) */
  const char *plain;             /* unsigned header lines ("Range: ...\r\n") */
  const char *body;              /* in memory */
  const char *body_file;         /* or a file slice */
  i64 off, len;
  FmNetProgress progress;
  void *user;
  size_t max_reply;
} Call;

static bool s3_object_uri(const S3 *s, const char *key, char *out, size_t cap) {
  size_t n = fm_strlcpy(out, s->root, cap);
  if (n + 2 >= cap) return false;
  if (!key) return true;
  out[n++] = '/';
  return dav_pct_path(key, out + n, cap - n);
}

static FmErr s3_call(const S3 *s, const Call *c, FmNetResp *r, volatile int *cancel) {
  char uri[S3_URL_MAX], q[S3_URL_MAX], url[S3_URL_MAX * 2 + 512], date[20], payload[65], sig[65], auth[700];
  if (!s3_object_uri(s, c->key, uri, sizeof uri)) {
    memset(r, 0, sizeof *r);
    fm_strlcpy(r->error, "the path is too long", sizeof r->error);
    return FM_ERR_FORMAT;
  }
  s3_canon_query(c->query, q, sizeof q);
  amz_now(date);
  if (c->payload) fm_strlcpy(payload, c->payload, sizeof payload);
  else if (c->body) sha256_hex(c->body, strlen(c->body), payload);
  else fm_strlcpy(payload, EMPTY_SHA, sizeof payload);
  S3Hdr h[6];
  int nh = 0;
  h[nh].name = "host"; h[nh++].value = s->host;
  h[nh].name = "x-amz-content-sha256"; h[nh++].value = payload;
  h[nh].name = "x-amz-date"; h[nh++].value = date;
  for (int i = 0; i < 3 && c->amz[i].name; i++) h[nh++] = c->amz[i];
  S3Sig sg = { c->method, uri, q, h, nh, payload, date, s->region, "s3", s->key_id, s->secret };
  s3_sign(&sg, sig, auth, sizeof auth);
  Sb hd = { NULL, 0, 0 };
  sb_add(&hd, "Authorization: "); sb_add(&hd, auth); sb_add(&hd, "\r\n");
  for (int i = 1; i < nh; i++) { sb_add(&hd, h[i].name); sb_add(&hd, ": "); sb_add(&hd, h[i].value); sb_add(&hd, "\r\n"); }
  if (c->plain) sb_add(&hd, c->plain);
  fm_snprintf(url, sizeof url, "%s%s%s%s", s->scheme_host, uri, q[0] ? "?" : "", q);
  FmNetReq rq;
  memset(&rq, 0, sizeof rq);
  rq.method = c->method;
  rq.headers = hd.p;
  if (c->body) { rq.body = c->body; rq.body_len = strlen(c->body); }
  else if (c->body_file) { rq.body_file = c->body_file; rq.body_off = c->off; rq.body_file_len = c->len; }
  rq.progress = c->progress;
  rq.user = c->user;
  rq.max_reply = c->max_reply ? c->max_reply : (8u << 20);
  rq.no_redirect = true;                          /* a redirect would need a new signature */
  FmErr e = net_request(url, &rq, r, cancel);
  fm_free(hd.p);
  return e;
}

static bool ok2xx(const FmNetResp *r) { return r->status >= 200 && r->status < 300; }

static FmErr s3_fail(const FmNetResp *r, FmErr e, char *err, size_t cap) {
  char code[64], msg[200];
  if (e == FM_ERR_CANCEL) { fm_strlcpy(err, "Cancelled.", cap); return e; }
  if (e == FM_OK && s3_parse_error((const char *)r->data, r->len, code, sizeof code, msg, sizeof msg)) {
    if (!strcmp(code, "InvalidAccessKeyId") || !strcmp(code, "SignatureDoesNotMatch") ||
        !strcmp(code, "InvalidToken")) {
      fm_snprintf(err, cap, "The access key or secret key is wrong (%s).", code);
      return FM_ERR_PASSWORD;
    }
    if (!strcmp(code, "NoSuchBucket")) { fm_strlcpy(err, "The bucket does not exist.", cap); return FM_ERR_NOT_FOUND; }
    if (!strcmp(code, "NoSuchKey")) { fm_strlcpy(err, "The file is gone.", cap); return FM_ERR_NOT_FOUND; }
    if (!strcmp(code, "RequestTimeTooSkewed")) {
      fm_strlcpy(err, "The device clock is wrong: set the date and time, then try again.", cap);
      return FM_ERR_ACCESS;
    }
    if (!strcmp(code, "PermanentRedirect") || !strcmp(code, "AuthorizationHeaderMalformed")) {
      fm_snprintf(err, cap, "Wrong region or endpoint for this bucket: %s", msg[0] ? msg : code);
      return FM_ERR_FORMAT;
    }
    fm_snprintf(err, cap, "%s refused the request: %s (%s, HTTP %d)", S3_NAME, msg[0] ? msg : code, code, r->status);
    return r->status == 403 ? FM_ERR_ACCESS : r->status == 404 ? FM_ERR_NOT_FOUND : FM_ERR_IO;
  }
  if (e == FM_OK && r->status >= 300 && r->status < 400) {
    fm_snprintf(err, cap, "The endpoint redirected (HTTP %d): check the region and endpoint address.", r->status);
    return FM_ERR_FORMAT;
  }
  FmErr ret = cloud_http_error(S3_NAME, r, e, err, cap);
  if (ret == FM_OK) {                             /* called on a reply that is not the expected one */
    fm_snprintf(err, cap, "%s gave an unexpected answer (HTTP %d)", S3_NAME, r->status);
    ret = FM_ERR_IO;
  }
  return ret;
}

/* one ListObjectsV2 page */
static FmErr list_page(const S3 *s, const char *prefix, bool delim, const char *token, FmCloudList *out, char *next,
                       size_t ncap, int max_keys, char *err, size_t errcap, volatile int *cancel) {
  char a1[CLOUD_ID_MAX + 16], a2[1100], a3[24];
  fm_snprintf(a1, sizeof a1, "prefix=%s", prefix);
  fm_snprintf(a2, sizeof a2, "continuation-token=%s", token ? token : "");
  fm_snprintf(a3, sizeof a3, "max-keys=%d", max_keys);
  const char *kv[8];
  int n = 0;
  kv[n++] = "list-type=2";
  kv[n++] = "encoding-type=url";
  kv[n++] = a1;
  kv[n++] = a3;
  if (delim) kv[n++] = "delimiter=/";
  if (token && *token) kv[n++] = a2;
  kv[n] = NULL;
  Call c;
  memset(&c, 0, sizeof c);
  c.method = "GET";
  c.query = kv;
  c.max_reply = 32u << 20;
  FmNetResp r;
  FmErr e = s3_call(s, &c, &r, cancel);
  if (e == FM_OK && r.status == 200) {
    if (s3_parse_list((const char *)r.data, r.len, prefix, out, next, ncap) < 0) {
      fm_strlcpy(err, "The bucket listing could not be read.", errcap);
      e = FM_ERR_FORMAT;
    }
  } else {
    e = s3_fail(&r, e, err, errcap);
  }
  net_resp_free(&r);
  return e;
}

static FmErr del_key(const S3 *s, const char *key, char *err, size_t errcap, volatile int *cancel) {
  Call c;
  memset(&c, 0, sizeof c);
  c.method = "DELETE";
  c.key = key;
  FmNetResp r;
  FmErr e = s3_call(s, &c, &r, cancel);
  if (e != FM_OK || (!ok2xx(&r) && r.status != 404)) e = s3_fail(&r, e, err, errcap);
  net_resp_free(&r);
  return e;
}

/* ---- the adapter --------------------------------------------------------------------------- */

static FmErr s_login(FmCloudAcct *a, char *err, size_t errcap, volatile int *cancel) {
  S3 s;
  FmErr e = s3_open(a, &s, err, errcap);
  if (e) return e;
  FmCloudList l;
  char next[1024];
  memset(&l, 0, sizeof l);
  e = list_page(&s, "", true, NULL, &l, next, sizeof next, 1, err, errcap, cancel);
  cloud_list_free(&l);
  if (e == FM_OK) {
    fm_strlcpy(a->session, "keys", sizeof a->session);   /* signing needs the secret every time */
    a->session_changed = true;
  }
  s3_close(&s);
  return e;
}

static void dir_prefix(const char *dir_id, char *out, size_t cap) {
  while (dir_id && *dir_id == '/') dir_id++;
  fm_strlcpy(out, dir_id ? dir_id : "", cap);
  size_t n = strlen(out);
  if (n && out[n - 1] != '/') fm_strlcat(out, "/", cap);
}

static FmErr s_list(FmCloudAcct *a, const char *dir_id, FmCloudList *out, volatile int *cancel) {
  S3 s;
  FmErr e = s3_open(a, &s, out->error, sizeof out->error);
  if (e) return e;
  char prefix[CLOUD_ID_MAX + 2], token[1024] = "", next[1024];
  dir_prefix(dir_id, prefix, sizeof prefix);
  for (int page = 0; page < 10000; page++) {
    e = list_page(&s, prefix, true, token, out, next, sizeof next, g_s3_max_keys ? g_s3_max_keys : 1000, out->error, sizeof out->error, cancel);
    if (e || !next[0] || !strcmp(next, token)) break;
    fm_strlcpy(token, next, sizeof token);
    if (cancel && *cancel) { e = FM_ERR_CANCEL; break; }
  }
  s3_close(&s);
  return e;
}

static FmErr s_download(FmCloudAcct *a, const FmCloudEntry *ent, const char *local_path, FmNetProgress cb, void *user,
                        char *err, size_t errcap, volatile int *cancel) {
  S3 s;
  FmErr e = s3_open(a, &s, err, errcap);
  if (e) return e;
  /* a presigned URL: the same signed GET, and nothing secret in the headers */
  char uri[S3_URL_MAX], url[S3_URL_MAX * 2], date[20];
  if (!s3_object_uri(&s, ent->id, uri, sizeof uri)) { s3_close(&s); fm_strlcpy(err, "The path is too long.", errcap); return FM_ERR_FORMAT; }
  amz_now(date);
  s3_presign(s.scheme_host, s.host, uri, s.region, s.key_id, s.secret, date, 3600, url, sizeof url);
  FmNetResp r;
  e = net_download(url, NULL, local_path, cb, user, &r, cancel);
  if (e != FM_OK && e != FM_ERR_CANCEL && ok2xx(&r))   /* the reply was fine: the local file failed */
    fm_snprintf(err, errcap, "Cannot write the file: %s", fm_err_str(e));
  else if (e != FM_OK) e = s3_fail(&r, e == FM_ERR_IO && r.status ? FM_OK : e, err, errcap);
  net_resp_free(&r);
  s3_close(&s);
  return e;
}

typedef struct UpProg { FmNetProgress cb; void *user; u64 base, total; i64 piece; } UpProg;

static bool up_prog(void *user, u64 done, u64 total) {
  UpProg *u = (UpProg *)user;
  if (!u->cb || (i64)total != u->piece) return true;
  return u->cb(u->user, u->base + done, u->total);
}

/* the text of <tag> in a reply */
static void reply_text(const FmNetResp *r, const char *tag, char *out, size_t cap) {
  FmXml x;
  if (cap) out[0] = 0;
  if (!r->data) return;
  xml_init(&x, (const char *)r->data, r->len);
  while (xml_next(&x) != XML_EOF)
    if (x.type == XML_START && xname(&x, tag)) { xml_inner_text(&x, out, cap); return; }
}

typedef struct Mpu {
  const S3 *s;
  const char *key;
  char id[1024];
  char (*etag)[80];
  int parts, cap;
} Mpu;

static FmErr mpu_start(Mpu *m, char *err, size_t errcap, volatile int *cancel) {
  const char *kv[] = { "uploads=", NULL };
  Call c;
  memset(&c, 0, sizeof c);
  c.method = "POST";
  c.key = m->key;
  c.query = kv;
  c.plain = "Content-Type: application/octet-stream\r\n";
  FmNetResp r;
  FmErr e = s3_call(m->s, &c, &r, cancel);
  if (e == FM_OK && r.status == 200) reply_text(&r, "UploadId", m->id, sizeof m->id);
  if (e != FM_OK || r.status != 200) e = s3_fail(&r, e, err, errcap);
  else if (!m->id[0]) { fm_strlcpy(err, "The server did not start the upload.", errcap); e = FM_ERR_FORMAT; }
  net_resp_free(&r);
  return e;
}

static void mpu_add_etag(Mpu *m, const char *etag) {
  if (m->parts == m->cap) {
    m->cap = m->cap ? m->cap * 2 : 64;
    m->etag = fm_realloc(m->etag, (size_t)m->cap * sizeof *m->etag);
  }
  fm_strlcpy(m->etag[m->parts++], etag, sizeof m->etag[0]);
}

static FmErr mpu_finish(Mpu *m, char *err, size_t errcap, volatile int *cancel) {
  char a1[1100];
  fm_snprintf(a1, sizeof a1, "uploadId=%s", m->id);
  const char *kv[] = { a1, NULL };
  Sb b = { NULL, 0, 0 };
  sb_add(&b, "<CompleteMultipartUpload xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">");
  for (int i = 0; i < m->parts; i++) {
    char part[200];
    fm_snprintf(part, sizeof part, "<Part><PartNumber>%d</PartNumber><ETag>", i + 1);
    sb_add(&b, part);
    for (const char *t = m->etag[i]; *t; t++) {   /* the etag carries quotes */
      if (*t == '"') sb_add(&b, "&quot;");
      else if (*t == '&') sb_add(&b, "&amp;");
      else if (*t == '<') sb_add(&b, "&lt;");
      else sb_put(&b, t, 1);
    }
    sb_add(&b, "</ETag></Part>");
  }
  sb_add(&b, "</CompleteMultipartUpload>");
  Call c;
  memset(&c, 0, sizeof c);
  c.method = "POST";
  c.key = m->key;
  c.query = kv;
  c.body = b.p;
  c.plain = "Content-Type: application/xml\r\n";
  FmNetResp r;
  FmErr e = s3_call(m->s, &c, &r, cancel);
  char code[64], msg[8];
  /* S3 can answer 200 and still fail in the body */
  if (e != FM_OK || r.status != 200 || s3_parse_error((const char *)r.data, r.len, code, sizeof code, msg, sizeof msg))
    e = s3_fail(&r, e, err, errcap);
  net_resp_free(&r);
  fm_free(b.p);
  return e;
}

static void mpu_abort(Mpu *m) {
  char a1[1100];
  fm_snprintf(a1, sizeof a1, "uploadId=%s", m->id);
  const char *kv[] = { a1, NULL };
  Call c;
  memset(&c, 0, sizeof c);
  c.method = "DELETE";
  c.key = m->key;
  c.query = kv;
  FmNetResp r;
  volatile int nocancel = 0;
  if (s3_call(m->s, &c, &r, &nocancel) == FM_OK) net_resp_free(&r);
}

static i64 part_size(i64 size, i64 min_part) {
  i64 p = min_part;
  while ((size + p - 1) / p > 10000) p += 1ll << 20;
  return p;
}

static FmErr s_upload_key(const S3 *s, const char *key, const char *local_path, i64 size, FmNetProgress cb,
                          void *user, char *err, size_t errcap, volatile int *cancel) {
  UpProg up = { cb, user, 0, (u64)size, 0 };
  i64 single = g_s3_single_max ? g_s3_single_max : (64ll << 20);
  FmNetResp r;
  FmErr e;
  if (size <= single) {
    Call c;
    memset(&c, 0, sizeof c);
    c.method = "PUT";
    c.key = key;
    c.payload = "UNSIGNED-PAYLOAD";
    c.plain = "Content-Type: application/octet-stream\r\n";
    c.body_file = local_path;
    c.len = size;
    c.progress = up_prog;
    c.user = &up;
    up.piece = size;
    e = s3_call(s, &c, &r, cancel);
    if (e != FM_OK || !ok2xx(&r)) e = s3_fail(&r, e, err, errcap);
    net_resp_free(&r);
    return e;
  }
  Mpu m;
  memset(&m, 0, sizeof m);
  m.s = s;
  m.key = key;
  e = mpu_start(&m, err, errcap, cancel);
  if (e) return e;
  i64 ps = part_size(size, g_s3_part_size ? g_s3_part_size : (16ll << 20));
  int num = 0;
  for (i64 off = 0; off < size && e == FM_OK; off += ps) {
    char a1[1100], a2[24];
    fm_snprintf(a1, sizeof a1, "uploadId=%s", m.id);
    fm_snprintf(a2, sizeof a2, "partNumber=%d", ++num);
    const char *kv[] = { a1, a2, NULL };
    Call c;
    memset(&c, 0, sizeof c);
    c.method = "PUT";
    c.key = key;
    c.query = kv;
    c.payload = "UNSIGNED-PAYLOAD";
    c.body_file = local_path;
    c.off = off;
    c.len = FM_MIN(ps, size - off);
    c.progress = up_prog;
    c.user = &up;
    up.base = (u64)off;
    up.piece = c.len;
    e = s3_call(s, &c, &r, cancel);
    char etag[80];
    if (e == FM_OK && ok2xx(&r) && net_resp_header(&r, "ETag", etag, sizeof etag) && etag[0]) mpu_add_etag(&m, etag);
    else e = s3_fail(&r, e, err, errcap);
    if (e == FM_OK && !ok2xx(&r)) e = FM_ERR_IO;
    net_resp_free(&r);
    if (e == FM_OK && cancel && *cancel) { e = FM_ERR_CANCEL; fm_strlcpy(err, "Cancelled.", errcap); }
  }
  if (e == FM_OK) e = mpu_finish(&m, err, errcap, cancel);
  if (e != FM_OK) mpu_abort(&m);
  fm_free(m.etag);
  return e;
}

static bool join_id(char *out, size_t cap, const char *dir, const char *name, bool folder) {
  char d[CLOUD_ID_MAX + 2];
  dir_prefix(dir, d, sizeof d);
  int n = fm_snprintf(out, cap, "%s%s%s", d, name, folder ? "/" : "");
  return n > 0 && (size_t)n < cap && (size_t)n < CLOUD_ID_MAX;
}

static FmErr s_upload(FmCloudAcct *a, const char *dir_id, const char *local_path, const char *name, FmCloudEntry *out,
                      FmNetProgress cb, void *user, char *err, size_t errcap, volatile int *cancel) {
  FmStat st;
  if (!plat_stat(local_path, &st)) { fm_strlcpy(err, "Cannot read the file to upload.", errcap); return FM_ERR_IO; }
  char key[CLOUD_ID_MAX];
  if (!join_id(key, sizeof key, dir_id, name, false)) { fm_strlcpy(err, "The path is too long.", errcap); return FM_ERR_FORMAT; }
  S3 s;
  FmErr e = s3_open(a, &s, err, errcap);
  if (e) return e;
  e = s_upload_key(&s, key, local_path, (i64)st.size, cb, user, err, errcap, cancel);
  if (e == FM_OK && out) {
    memset(out, 0, sizeof *out);
    fm_strlcpy(out->id, key, sizeof out->id);
    fm_strlcpy(out->name, name, sizeof out->name);
    out->size = st.size;
    out->mtime = plat_time_unix();
  }
  s3_close(&s);
  return e;
}

static FmErr put_marker(const S3 *s, const char *key, char *err, size_t errcap, volatile int *cancel) {
  Call c;
  memset(&c, 0, sizeof c);
  c.method = "PUT";
  c.key = key;
  c.body = "";
  FmNetResp r;
  FmErr e = s3_call(s, &c, &r, cancel);
  if (e != FM_OK || !ok2xx(&r)) e = s3_fail(&r, e, err, errcap);
  net_resp_free(&r);
  return e;
}

static FmErr s_mkdir(FmCloudAcct *a, const char *parent_id, const char *name, FmCloudEntry *out, char *err,
                     size_t errcap, volatile int *cancel) {
  char key[CLOUD_ID_MAX];
  if (!*name || strchr(name, '/')) { fm_strlcpy(err, "Names cannot contain '/'.", errcap); return FM_ERR_FORMAT; }
  if (!join_id(key, sizeof key, parent_id, name, true)) { fm_strlcpy(err, "The path is too long.", errcap); return FM_ERR_FORMAT; }
  S3 s;
  FmErr e = s3_open(a, &s, err, errcap);
  if (e) return e;
  e = put_marker(&s, key, err, errcap, cancel);
  if (e == FM_OK && out) {
    memset(out, 0, sizeof *out);
    fm_strlcpy(out->id, key, sizeof out->id);
    fm_strlcpy(out->name, name, sizeof out->name);
    out->dir = true;
  }
  s3_close(&s);
  return e;
}

/* Calls fn for every key under prefix (recursive, paged), then for the
** marker "prefix" itself. */
typedef FmErr (*KeyFn)(const S3 *s, const FmCloudEntry *e, void *user, char *err, size_t errcap, volatile int *cancel);

static FmErr each_key(const S3 *s, const char *prefix, KeyFn fn, void *user, char *err, size_t errcap,
                      volatile int *cancel) {
  char token[1024] = "", next[1024];
  FmErr e = FM_OK;
  for (int page = 0; page < 100000 && e == FM_OK; page++) {
    FmCloudList l;
    memset(&l, 0, sizeof l);
    e = list_page(s, prefix, false, token, &l, next, sizeof next, g_s3_max_keys ? g_s3_max_keys : 1000, err, errcap, cancel);
    for (int i = 0; i < l.count && e == FM_OK; i++) {
      if (cancel && *cancel) { e = FM_ERR_CANCEL; fm_strlcpy(err, "Cancelled.", errcap); break; }
      e = fn(s, &l.items[i], user, err, errcap, cancel);
    }
    cloud_list_free(&l);
    if (e || !next[0] || !strcmp(next, token)) break;
    fm_strlcpy(token, next, sizeof token);
  }
  if (e == FM_OK) {
    FmCloudEntry m;
    memset(&m, 0, sizeof m);
    fm_strlcpy(m.id, prefix, sizeof m.id);
    m.dir = true;                                 /* the marker (absent is fine) */
    e = fn(s, &m, user, err, errcap, cancel);
  }
  return e;
}

static FmErr del_fn(const S3 *s, const FmCloudEntry *e, void *user, char *err, size_t errcap, volatile int *cancel) {
  FM_UNUSED(user);
  return del_key(s, e->id, err, errcap, cancel);
}

static FmErr s_remove(FmCloudAcct *a, const FmCloudEntry *ent, char *err, size_t errcap, volatile int *cancel) {
  if (!ent->id[0]) { fm_strlcpy(err, "Cannot delete the whole bucket.", errcap); return FM_ERR_ACCESS; }
  S3 s;
  FmErr e = s3_open(a, &s, err, errcap);
  if (e) return e;
  if (ent->dir) {
    char prefix[CLOUD_ID_MAX + 2];
    dir_prefix(ent->id, prefix, sizeof prefix);
    e = each_key(&s, prefix, del_fn, NULL, err, errcap, cancel);
  } else {
    e = del_key(&s, ent->id, err, errcap, cancel);
  }
  s3_close(&s);
  return e;
}

/* Server-side copy of one object (multipart UploadPartCopy above 5 GB). */
static FmErr copy_key(const S3 *s, const char *from, u64 size, const char *to, char *err, size_t errcap,
                      volatile int *cancel) {
  char src[S3_URL_MAX];
  size_t n = fm_strlcpy(src, s->root, sizeof src);   /* "/bucket/key", encoded */
  if (n + 2 >= sizeof src) return FM_ERR_FORMAT;
  src[n++] = '/';
  if (!dav_pct_path(from, src + n, sizeof src - n)) { fm_strlcpy(err, "The path is too long.", errcap); return FM_ERR_FORMAT; }
  const char *srcv = src + s->bucket_at;          /* "/bucket/key", without the endpoint's own path */
  FmNetResp r;
  FmErr e;
  char code[64], msg[8];
  if ((i64)size <= (g_s3_copy_max ? g_s3_copy_max : S3_COPY_MAX)) {
    Call c;
    memset(&c, 0, sizeof c);
    c.method = "PUT";
    c.key = to;
    c.amz[0].name = "x-amz-copy-source";
    c.amz[0].value = srcv;
    e = s3_call(s, &c, &r, cancel);
    if (e != FM_OK || !ok2xx(&r) || s3_parse_error((const char *)r.data, r.len, code, sizeof code, msg, sizeof msg))
      e = s3_fail(&r, e, err, errcap);
    net_resp_free(&r);
    return e;
  }
  Mpu m;
  memset(&m, 0, sizeof m);
  m.s = s;
  m.key = to;
  e = mpu_start(&m, err, errcap, cancel);
  if (e) return e;
  i64 ps = part_size((i64)size, g_s3_copy_max && g_s3_part_size ? g_s3_part_size : S3_COPY_PART);
  int num = 0;
  for (i64 off = 0; off < (i64)size && e == FM_OK; off += ps) {
    char a1[1100], a2[24], range[64], etag[80];
    fm_snprintf(a1, sizeof a1, "uploadId=%s", m.id);
    fm_snprintf(a2, sizeof a2, "partNumber=%d", ++num);
    fm_snprintf(range, sizeof range, "bytes=%lld-%lld", (long long)off, (long long)(FM_MIN(off + ps, (i64)size) - 1));
    const char *kv[] = { a1, a2, NULL };
    Call c;
    memset(&c, 0, sizeof c);
    c.method = "PUT";
    c.key = to;
    c.query = kv;
    c.amz[0].name = "x-amz-copy-source";
    c.amz[0].value = srcv;
    c.amz[1].name = "x-amz-copy-source-range";
    c.amz[1].value = range;
    e = s3_call(s, &c, &r, cancel);
    if (e == FM_OK && ok2xx(&r)) reply_text(&r, "ETag", etag, sizeof etag);
    else etag[0] = 0;
    if (e != FM_OK || !ok2xx(&r) || !etag[0]) e = s3_fail(&r, e, err, errcap);
    if (e == FM_OK && !etag[0]) e = FM_ERR_IO;
    if (e == FM_OK) mpu_add_etag(&m, etag);
    net_resp_free(&r);
  }
  if (e == FM_OK) e = mpu_finish(&m, err, errcap, cancel);
  if (e != FM_OK) mpu_abort(&m);
  fm_free(m.etag);
  return e;
}

typedef struct MoveCtx { const char *from, *to; } MoveCtx;  /* prefixes */

static FmErr move_fn(const S3 *s, const FmCloudEntry *e, void *user, char *err, size_t errcap, volatile int *cancel) {
  const MoveCtx *mc = (const MoveCtx *)user;
  char to[CLOUD_ID_MAX * 2];
  size_t fl = strlen(mc->from);
  if (strncmp(e->id, mc->from, fl)) return FM_OK;
  fm_snprintf(to, sizeof to, "%s%s", mc->to, e->id + fl);
  if (strlen(to) >= CLOUD_ID_MAX) { fm_strlcpy(err, "The new path is too long.", errcap); return FM_ERR_FORMAT; }
  FmErr r;
  if (e->dir) {                                    /* the marker: write the new one, drop the old */
    r = put_marker(s, to, err, errcap, cancel);
  } else {
    r = copy_key(s, e->id, e->size, to, err, errcap, cancel);
  }
  if (r == FM_OK) r = del_key(s, e->id, err, errcap, cancel);
  return r;
}

static FmErr move_to(FmCloudAcct *a, const FmCloudEntry *ent, const char *to, char *err, size_t errcap,
                     volatile int *cancel) {
  if (!ent->id[0]) { fm_strlcpy(err, "Cannot move the bucket.", errcap); return FM_ERR_ACCESS; }
  if (!strcmp(ent->id, to)) return FM_OK;
  S3 s;
  FmErr e = s3_open(a, &s, err, errcap);
  if (e) return e;
  /* no overwrite: refuse when the target exists (a HEAD for files, a listing for folders) */
  FmNetResp r;
  if (ent->dir) {
    FmCloudList l;
    char next[1024];
    memset(&l, 0, sizeof l);
    e = list_page(&s, to, false, NULL, &l, next, sizeof next, 1, err, errcap, cancel);
    if (e == FM_OK && l.count) { fm_strlcpy(err, "A folder with that name is already there.", errcap); e = FM_ERR_EXISTS; }
    cloud_list_free(&l);
    if (e == FM_OK) {
      MoveCtx mc = { ent->id, to };
      e = each_key(&s, ent->id, move_fn, &mc, err, errcap, cancel);
    }
  } else {
    Call c;
    memset(&c, 0, sizeof c);
    c.method = "HEAD";
    c.key = to;
    e = s3_call(&s, &c, &r, cancel);
    if (e == FM_OK && ok2xx(&r)) { fm_strlcpy(err, "A file with that name is already there.", errcap); e = FM_ERR_EXISTS; }
    else if (e != FM_OK) e = s3_fail(&r, e, err, errcap);
    net_resp_free(&r);
    if (e == FM_OK) e = copy_key(&s, ent->id, ent->size, to, err, errcap, cancel);
    if (e == FM_OK) e = del_key(&s, ent->id, err, errcap, cancel);
  }
  s3_close(&s);
  return e;
}

static void split_id(const char *id, char *parent, size_t pcap, char *base, size_t bcap) {
  char t[CLOUD_ID_MAX];
  fm_strlcpy(t, id, sizeof t);
  size_t n = strlen(t);
  if (n && t[n - 1] == '/') t[--n] = 0;
  char *sl = strrchr(t, '/');
  fm_strlcpy(base, sl ? sl + 1 : t, bcap);
  if (sl) { sl[1] = 0; fm_strlcpy(parent, t, pcap); }
  else if (pcap) parent[0] = 0;
}

static FmErr s_rename(FmCloudAcct *a, const FmCloudEntry *ent, const char *new_name, char *err, size_t errcap,
                      volatile int *cancel) {
  char parent[CLOUD_ID_MAX], base[CLOUD_ID_MAX], to[CLOUD_ID_MAX];
  if (!*new_name || strchr(new_name, '/')) { fm_strlcpy(err, "Names cannot contain '/'.", errcap); return FM_ERR_FORMAT; }
  split_id(ent->id, parent, sizeof parent, base, sizeof base);
  if (!join_id(to, sizeof to, parent, new_name, ent->dir)) { fm_strlcpy(err, "The name is too long.", errcap); return FM_ERR_FORMAT; }
  return move_to(a, ent, to, err, errcap, cancel);
}

static FmErr s_move(FmCloudAcct *a, const FmCloudEntry *ent, const char *new_parent_id, char *err, size_t errcap,
                    volatile int *cancel) {
  char parent[CLOUD_ID_MAX], base[CLOUD_ID_MAX], to[CLOUD_ID_MAX];
  split_id(ent->id, parent, sizeof parent, base, sizeof base);
  if (!join_id(to, sizeof to, new_parent_id, base, ent->dir)) { fm_strlcpy(err, "The path is too long.", errcap); return FM_ERR_FORMAT; }
  if (ent->dir && !strncmp(to, ent->id, strlen(ent->id))) {
    fm_strlcpy(err, "A folder cannot move into itself.", errcap);
    return FM_ERR_ACCESS;
  }
  return move_to(a, ent, to, err, errcap, cancel);
}

static FmErr s_stream_url(FmCloudAcct *a, const FmCloudEntry *ent, char *url, size_t urlcap, char *headers,
                          size_t hcap, char *err, size_t errcap, volatile int *cancel) {
  FM_UNUSED(cancel);
  S3 s;
  FmErr e = s3_open(a, &s, err, errcap);
  if (e) return e;
  char uri[S3_URL_MAX], date[20];
  if (!s3_object_uri(&s, ent->id, uri, sizeof uri)) { s3_close(&s); fm_strlcpy(err, "The path is too long.", errcap); return FM_ERR_FORMAT; }
  amz_now(date);
  s3_presign(s.scheme_host, s.host, uri, s.region, s.key_id, s.secret, date, 3600, url, urlcap);
  if (hcap) headers[0] = 0;
  s3_close(&s);
  return FM_OK;
}

const FmCloud g_cloud_s3 = {
  "s3", "S3 storage", IC_CLOUD, CLOUD_SERVER | CLOUD_KEYS | CLOUD_UPLOAD | CLOUD_STREAM,
  s_login, s_list, s_download, s_upload, s_mkdir, s_remove, s_rename, s_move, NULL, s_stream_url, NULL,
  "Amazon S3, Backblaze B2, Cloudflare R2, Wasabi, MinIO: endpoint, bucket and access keys",
};
