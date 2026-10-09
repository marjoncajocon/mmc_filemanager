/* fcloud_onedrive.c -- OneDrive for the cloud panels (fcloud.h), Microsoft
** Graph /me/drive with the user's own app registration (foauth.h).
**
** Getting a client id (free, once):
**   1. portal.azure.com (or entra.microsoft.com) -> App registrations ->
**      New registration. Supported accounts: "Accounts in any organizational
**      directory and personal Microsoft accounts".
**   2. Redirect URI: platform "Public client/native (mobile & desktop)",
**      http://localhost (any port then works).
**   3. Authentication -> Advanced: "Allow public client flows" = Yes.
**   4. API permissions: Microsoft Graph, delegated Files.ReadWrite (and
**      offline_access); User.Read is there by default.
**   5. Copy the "Application (client) ID" into Settings (no secret).
**
** Design decisions:
**   - Ids are Graph item ids; "" is the drive root. Listing pages follow
**     @odata.nextLink (a full URL) until it is gone.
**   - Sign-in uses login.microsoftonline.com/common (work, school and
**     personal accounts) with redirect host "localhost": that is what the
**     Azure portal registers for desktop apps (Microsoft ignores the port).
**   - Downloads: /content answers 302 to a pre-authorised URL, fetched
**     without the bearer token (oa_download). stream_url hands out that
**     same kind of URL (@microsoft.graph.downloadUrl, valid ~1 hour).
**   - Uploads up to 4 MB: one PUT .../<parent>:/<name>:/content (replaces).
**     Bigger: createUploadSession, then PUTs of 10 MB (a multiple of
**     320 KiB, as Graph requires) to the session URL, which carries its own
**     authorisation (no bearer header). After an error the session's
**     nextExpectedRanges says where to go on.
**   - Delete goes to the OneDrive recycle bin.
*/
#include "foauth_int.h"
#include "fjson.h"
#include "fplat.h"
#include "fsdl.h"

#define OD_SVC "OneDrive"
#define OD_DRIVE "https://graph.microsoft.com/v1.0/me/drive"
#define OD_API OD_DRIVE "/"
#define OD_SIMPLE_MAX (4u << 20)
#define OD_CHUNK (32u * 320u * 1024u)           /* 10 MiB, a multiple of 320 KiB */
#define OD_JSON "application/json"

static const FmOAuthCfg kCfg = {
  "https://login.microsoftonline.com/common/oauth2/v2.0/authorize",
  "https://login.microsoftonline.com/common/oauth2/v2.0/token", NULL, NULL,
  "offline_access Files.ReadWrite", "prompt=select_account", "localhost",
};

static FmOAuthCfg cfg_of(const FmCloudAcct *a) {
  FmOAuthCfg c = kCfg;
  c.client_id = a->client_id;
  c.client_secret = a->client_secret;
  return c;
}

static FmErr od_open(OaSess *s, FmCloudAcct *a, char *err, size_t cap, volatile int *cancel) {
  FmOAuthCfg c = cfg_of(a);
  return oa_open(s, a, &c, OD_SVC, err, cap, cancel);
}

/* "root" or "items/<id>" (id percent-encoded) */
static void od_item(const char *id, char *out, size_t cap) {
  if (!id || !*id) {
    fm_strlcpy(out, "root", cap);
    return;
  }
  char enc[CLOUD_ID_MAX * 3];
  net_urlencode(id, enc, sizeof enc);
  fm_snprintf(out, cap, "items/%s", enc);
}

/* ---- parsing ---------------------------------------------------------------------- */

static bool entry_from(const FmJsonNode *n, FmCloudEntry *e) {
  const char *id = json_str(json_get(n, "id"), "");
  if (!*id || strlen(id) >= sizeof e->id) return false;
  memset(e, 0, sizeof *e);
  fm_strlcpy(e->id, id, sizeof e->id);
  fm_strlcpy(e->name, json_str(json_get(n, "name"), id), sizeof e->name);
  e->dir = json_get(n, "folder") != NULL || json_get(n, "package") != NULL;   /* OneNote notebooks: folders */
  e->mtime = cloud_parse_time(json_str(json_path(n, "fileSystemInfo.lastModifiedDateTime"),
                                       json_str(json_get(n, "lastModifiedDateTime"), "")));
  if (!e->dir) {
    double sz = json_num(json_get(n, "size"), 0);
    e->size = sz > 0 ? (u64)sz : 0;
    const FmJsonNode *f = json_get(n, "file");
    fm_strlcpy(e->mime, json_str(json_get(f, "mimeType"), ""), sizeof e->mime);
    const char *sha1 = json_str(json_path(f, "hashes.sha1Hash"), "");
    const char *qx = json_str(json_path(f, "hashes.quickXorHash"), "");
    if (*sha1 && strlen(sha1) <= 40) fm_snprintf(e->hash, sizeof e->hash, "sha1:%s", sha1);
    else if (*qx && strlen(qx) <= 60) fm_snprintf(e->hash, sizeof e->hash, "qxor:%s", qx);
  }
  return true;
}

FmErr onedrive_parse_list(const char *json, size_t len, FmCloudList *out, char *next, size_t ncap) {
  FmJson j;
  if (ncap) next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "OneDrive sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *r = json_root(&j), *v = json_get(r, "value");
  if (!v || v->type != JSON_ARR) {
    json_free(&j);
    fm_strlcpy(out->error, "OneDrive sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  for (const FmJsonNode *n = json_first(v); n; n = json_next(n)) {
    FmCloudEntry e;
    if (entry_from(n, &e)) *cloud_list_add(out) = e;
  }
  const char *nl = json_str(json_get(r, "@odata.nextLink"), "");
  if (!strncmp(nl, "https://graph.microsoft.com/", 28) && strlen(nl) < ncap) fm_strlcpy(next, nl, ncap);
  json_free(&j);
  return FM_OK;
}

bool onedrive_parse_quota(const char *json, size_t len, u64 *used, u64 *total) {
  FmJson j;
  if (!json || json_parse(&j, json, len) != FM_OK) return false;
  const FmJsonNode *q = json_get(json_root(&j), "quota");
  double u = json_num(json_get(q, "used"), -1), t = json_num(json_get(q, "total"), 0);
  json_free(&j);
  if (u < 0) return false;
  *used = (u64)u;
  *total = t > 0 ? (u64)t : 0;
  return true;
}

i64 onedrive_next_range(const char *json, size_t len) {
  FmJson j;
  i64 v = -1;
  if (json && json_parse(&j, json, len) == FM_OK) {
    const char *r = json_str(json_path(json_root(&j), "nextExpectedRanges.0"), "");
    if (*r >= '0' && *r <= '9') {
      v = 0;
      for (; *r >= '0' && *r <= '9'; r++) v = v * 10 + (*r - '0');
    }
    json_free(&j);
  }
  return v;
}

static bool parse_item(const FmNetResp *r, FmCloudEntry *e) {
  FmJson j;
  if (!r->data || json_parse(&j, (const char *)r->data, r->len) != FM_OK) return false;
  bool ok = entry_from(json_root(&j), e);
  json_free(&j);
  return ok;
}

/* ---- calls -------------------------------------------------------------------------- */

static FmErr call(OaSess *s, const char *method, const char *url, const char *body, FmNetResp *r) {
  FmErr e = oa_send(s, method, url, body ? OD_JSON : NULL, body, NULL, r);
  if (e != FM_OK || !oa_ok(r)) {
    e = oa_fail(s, r, e);
    net_resp_free(r);
    return e;
  }
  return FM_OK;
}

static FmErr od_login(FmCloudAcct *a, char *err, size_t errcap, volatile int *cancel) {
  FmOAuthCfg c = cfg_of(a);
  FmErr e = oa_login(a, &c, OD_SVC, err, errcap, cancel);
  if (e != FM_OK) return e;
  OaSess s;
  FmNetResp r;
  if (oa_open(&s, a, &c, OD_SVC, err, errcap, cancel) == FM_OK && call(&s, "GET", OD_DRIVE "?$select=owner", NULL, &r) == FM_OK) {
    FmJson j;
    if (json_parse(&j, (const char *)r.data, r.len) == FM_OK) {
      const char *who = json_str(json_path(json_root(&j), "owner.user.displayName"), "");
      if (!a->user[0]) fm_strlcpy(a->user, who, sizeof a->user);
      if (!a->label[0] && *who) fm_snprintf(a->label, sizeof a->label, "OneDrive %s", who);
      json_free(&j);
    }
    net_resp_free(&r);
  }
  err[0] = 0;
  return FM_OK;
}

static FmErr od_list(FmCloudAcct *a, const char *dir_id, FmCloudList *out, volatile int *cancel) {
  OaSess s;
  FmErr e = od_open(&s, a, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  char item[CLOUD_ID_MAX * 3 + 16];
  char *url = (char *)fm_alloc(4096), *next = (char *)fm_alloc(4096);
  od_item(dir_id, item, sizeof item);
  fm_snprintf(url, 4096, OD_API "%s/children?$top=1000&$select=id,name,size,folder,package,file,"
              "lastModifiedDateTime,fileSystemInfo", item);
  for (int page = 0; page < 10000; page++) {
    if (cancel && *cancel) { e = oa_fail(&s, NULL, FM_ERR_CANCEL); break; }
    FmNetResp r;
    e = call(&s, "GET", url, NULL, &r);
    if (e != FM_OK) break;
    e = onedrive_parse_list((const char *)r.data, r.len, out, next, 4096);
    net_resp_free(&r);
    if (e != FM_OK || !next[0]) break;
    fm_strlcpy(url, next, 4096);
  }
  fm_free(url);
  fm_free(next);
  return e;
}

static FmErr od_download(FmCloudAcct *a, const FmCloudEntry *en, const char *local_path, FmNetProgress cb,
                         void *user, char *err, size_t errcap, volatile int *cancel) {
  OaSess s;
  FmErr e = od_open(&s, a, err, errcap, cancel);
  if (e != FM_OK) return e;
  char item[CLOUD_ID_MAX * 3 + 16], url[CLOUD_ID_MAX * 3 + 128];
  od_item(en->id, item, sizeof item);
  fm_snprintf(url, sizeof url, OD_API "%s/content", item);
  FmNetReq q;
  memset(&q, 0, sizeof q);
  q.no_redirect = true;                     /* the 302 target must not get the token */
  return oa_download(&s, url, &q, local_path, cb, user);
}

static FmErr od_upload(FmCloudAcct *a, const char *dir_id, const char *local_path, const char *name, FmCloudEntry *out,
                       FmNetProgress cb, void *user, char *err, size_t errcap, volatile int *cancel) {
  OaSess s;
  FmErr e = od_open(&s, a, err, errcap, cancel);
  if (e != FM_OK) return e;
  FmStat st;
  if (!plat_stat(local_path, &st) || (st.flags & FM_ST_DIR)) {
    fm_strlcpy(err, "Cannot read the file to upload", errcap);
    return FM_ERR_IO;
  }
  char item[CLOUD_ID_MAX * 3 + 16], en[800], url[CLOUD_ID_MAX * 3 + 1000];
  od_item(dir_id, item, sizeof item);
  net_urlencode(name, en, sizeof en);
  OaProg pg = { cb, user, 0, 0, st.size };
  FmNetResp r;
  if (st.size <= OD_SIMPLE_MAX) {
    fm_snprintf(url, sizeof url, OD_API "%s:/%s:/content?@microsoft.graph.conflictBehavior=replace", item, en);
    FmNetReq q;
    memset(&q, 0, sizeof q);
    q.method = "PUT";
    q.headers = "Content-Type: application/octet-stream\r\n";
    q.body_file = local_path;
    q.body_file_len = (i64)st.size;
    pg.chunk = st.size;
    q.progress = oa_prog;
    q.user = &pg;
    e = oa_call(&s, url, &q, true, &r);
    if (e != FM_OK || !oa_ok(&r)) {
      e = oa_fail(&s, &r, e);
      net_resp_free(&r);
      return e;
    }
    if (out && !parse_item(&r, out)) memset(out, 0, sizeof *out);
    net_resp_free(&r);
    return FM_OK;
  }
  fm_snprintf(url, sizeof url, OD_API "%s:/%s:/createUploadSession", item, en);
  e = call(&s, "POST", url, "{\"item\":{\"@microsoft.graph.conflictBehavior\":\"replace\"}}", &r);
  if (e != FM_OK) return e;
  char *up = (char *)fm_alloc(4096);
  up[0] = 0;
  FmJson j;
  if (json_parse(&j, (const char *)r.data, r.len) == FM_OK) {
    const char *u = json_str(json_get(json_root(&j), "uploadUrl"), "");
    if (!fm_strnicmp(u, "https://", 8) && strlen(u) < 4096) fm_strlcpy(up, u, 4096);
    json_free(&j);
  }
  net_resp_free(&r);
  if (!up[0]) {
    fm_free(up);
    fm_strlcpy(err, "OneDrive did not start the upload", errcap);
    return FM_ERR_IO;
  }
  u64 off = 0;
  int fails = 0;
  char hdr[160];
  for (;;) {
    if (cancel && *cancel) { e = oa_fail(&s, NULL, FM_ERR_CANCEL); break; }
    u64 len = FM_MIN((u64)OD_CHUNK, st.size - off);
    fm_snprintf(hdr, sizeof hdr, "Content-Range: bytes %llu-%llu/%llu\r\n", (unsigned long long)off,
                (unsigned long long)(off + len - 1), (unsigned long long)st.size);
    FmNetReq q;
    memset(&q, 0, sizeof q);
    q.method = "PUT";
    q.headers = hdr;
    q.body_file = local_path;
    q.body_off = (i64)off;
    q.body_file_len = (i64)len;
    pg.base = off;
    pg.chunk = len;
    q.progress = oa_prog;
    q.user = &pg;
    e = oa_call(&s, up, &q, false, &r);          /* the session URL is pre-authorised */
    if (e == FM_OK && (r.status == 200 || r.status == 201)) {
      if (out && !parse_item(&r, out)) memset(out, 0, sizeof *out);
      net_resp_free(&r);
      break;
    }
    if (e == FM_OK && r.status == 202) {
      i64 nx = onedrive_next_range((const char *)r.data, r.len);
      net_resp_free(&r);
      off = nx >= 0 && (u64)nx < st.size ? (u64)nx : off + len;
      if (off >= st.size) off = st.size - 1;     /* the server wants the end again: resend the last byte */
      fails = 0;
      continue;
    }
    bool retry = e == FM_ERR_IO || (e == FM_OK && (r.status >= 500 || r.status == 429 || r.status == 416));
    if (!retry || ++fails > 5) {
      e = oa_fail(&s, &r, e);
      net_resp_free(&r);
      break;
    }
    net_resp_free(&r);
    for (int i = 0; i < fails * 4 && !(cancel && *cancel); i++) SDL_Delay(250);
    FmNetReq g;                                  /* where does the session stand? */
    memset(&g, 0, sizeof g);
    if (oa_call(&s, up, &g, false, &r) == FM_OK && oa_ok(&r)) {
      i64 nx = onedrive_next_range((const char *)r.data, r.len);
      if (nx >= 0 && (u64)nx < st.size) off = (u64)nx;
    }
    net_resp_free(&r);
  }
  if (e != FM_OK) {                              /* drop the half-done session */
    FmNetReq d;
    memset(&d, 0, sizeof d);
    d.method = "DELETE";
    if (oa_call(&s, up, &d, false, &r) == FM_OK) net_resp_free(&r);
  }
  fm_free(up);
  return e;
}

static FmErr od_mkdir(FmCloudAcct *a, const char *parent_id, const char *name, FmCloudEntry *out, char *err,
                      size_t errcap, volatile int *cancel) {
  OaSess s;
  FmErr e = od_open(&s, a, err, errcap, cancel);
  if (e != FM_OK) return e;
  char item[CLOUD_ID_MAX * 3 + 16], url[CLOUD_ID_MAX * 3 + 128], qn[1100], body[1300];
  od_item(parent_id, item, sizeof item);
  fm_snprintf(url, sizeof url, OD_API "%s/children", item);
  oa_json_quote(name, qn, sizeof qn, false);
  fm_snprintf(body, sizeof body, "{\"name\":%s,\"folder\":{},\"@microsoft.graph.conflictBehavior\":\"fail\"}", qn);
  FmNetResp r;
  e = call(&s, "POST", url, body, &r);
  if (e != FM_OK) return e;
  if (out && !parse_item(&r, out)) memset(out, 0, sizeof *out);
  net_resp_free(&r);
  return FM_OK;
}

static FmErr item_call(FmCloudAcct *a, const char *id, const char *method, const char *body, char *err,
                       size_t errcap, volatile int *cancel) {
  OaSess s;
  FmErr e = od_open(&s, a, err, errcap, cancel);
  if (e != FM_OK) return e;
  if (!id || !*id) {
    fm_strlcpy(err, "The OneDrive root cannot be changed", errcap);
    return FM_ERR_ACCESS;
  }
  char item[CLOUD_ID_MAX * 3 + 16], url[CLOUD_ID_MAX * 3 + 128];
  od_item(id, item, sizeof item);
  fm_snprintf(url, sizeof url, OD_API "%s", item);
  FmNetResp r;
  e = call(&s, method, url, body, &r);
  if (e == FM_OK) net_resp_free(&r);
  return e;
}

static FmErr od_remove(FmCloudAcct *a, const FmCloudEntry *en, char *err, size_t errcap, volatile int *cancel) {
  return item_call(a, en->id, "DELETE", NULL, err, errcap, cancel);
}

static FmErr od_rename(FmCloudAcct *a, const FmCloudEntry *en, const char *new_name, char *err, size_t errcap,
                       volatile int *cancel) {
  char qn[1100], body[1200];
  oa_json_quote(new_name, qn, sizeof qn, false);
  fm_snprintf(body, sizeof body, "{\"name\":%s}", qn);
  return item_call(a, en->id, "PATCH", body, err, errcap, cancel);
}

static FmErr od_move(FmCloudAcct *a, const FmCloudEntry *en, const char *new_parent_id, char *err, size_t errcap,
                     volatile int *cancel) {
  char pid[CLOUD_ID_MAX], qp[CLOUD_ID_MAX * 2 + 8], body[CLOUD_ID_MAX * 2 + 64];
  fm_strlcpy(pid, new_parent_id ? new_parent_id : "", sizeof pid);
  if (!pid[0]) {                                  /* the root's real id */
    OaSess s;
    FmErr e = od_open(&s, a, err, errcap, cancel);
    if (e != FM_OK) return e;
    FmNetResp r;
    e = call(&s, "GET", OD_API "root?$select=id", NULL, &r);
    if (e != FM_OK) return e;
    FmJson j;
    if (json_parse(&j, (const char *)r.data, r.len) == FM_OK) {
      fm_strlcpy(pid, json_str(json_get(json_root(&j), "id"), ""), sizeof pid);
      json_free(&j);
    }
    net_resp_free(&r);
    if (!pid[0]) {
      fm_strlcpy(err, "OneDrive did not say where its root is", errcap);
      return FM_ERR_FORMAT;
    }
  }
  oa_json_quote(pid, qp, sizeof qp, false);
  fm_snprintf(body, sizeof body, "{\"parentReference\":{\"id\":%s}}", qp);
  return item_call(a, en->id, "PATCH", body, err, errcap, cancel);
}

static FmErr od_quota(FmCloudAcct *a, u64 *used, u64 *total, char *err, size_t errcap, volatile int *cancel) {
  OaSess s;
  FmErr e = od_open(&s, a, err, errcap, cancel);
  if (e != FM_OK) return e;
  FmNetResp r;
  e = call(&s, "GET", OD_DRIVE "?$select=quota", NULL, &r);
  if (e != FM_OK) return e;
  if (!onedrive_parse_quota((const char *)r.data, r.len, used, total)) {
    fm_strlcpy(err, "OneDrive did not say how much space is used", errcap);
    e = FM_ERR_FORMAT;
  }
  net_resp_free(&r);
  return e;
}

static FmErr od_stream_url(FmCloudAcct *a, const FmCloudEntry *en, char *url, size_t urlcap, char *headers,
                           size_t hcap, char *err, size_t errcap, volatile int *cancel) {
  OaSess s;
  FmErr e = od_open(&s, a, err, errcap, cancel);
  if (e != FM_OK) return e;
  char item[CLOUD_ID_MAX * 3 + 16], u[CLOUD_ID_MAX * 3 + 128];
  od_item(en->id, item, sizeof item);
  fm_snprintf(u, sizeof u, OD_API "%s", item);
  FmNetResp r;
  e = call(&s, "GET", u, NULL, &r);
  if (e != FM_OK) return e;
  FmJson j;
  e = FM_ERR_FORMAT;
  if (json_parse(&j, (const char *)r.data, r.len) == FM_OK) {
    const char *dl = json_str(json_get(json_root(&j), "@microsoft.graph.downloadUrl"), "");
    if (!fm_strnicmp(dl, "https://", 8) && strlen(dl) < urlcap) {
      fm_strlcpy(url, dl, urlcap);
      if (hcap) headers[0] = 0;
      e = FM_OK;
    }
    json_free(&j);
  }
  if (e != FM_OK) fm_strlcpy(err, "OneDrive gave no link to play from", errcap);
  net_resp_free(&r);
  return e;
}

const FmCloud g_cloud_onedrive = {
  "onedrive", "OneDrive", IC_CLOUD, CLOUD_OAUTH | CLOUD_UPLOAD | CLOUD_STREAM,
  od_login, od_list, od_download, od_upload, od_mkdir, od_remove, od_rename, od_move, od_quota, od_stream_url, NULL,
  "Sign in with Microsoft. Needs your own free client id: portal.azure.com > App registrations > New "
  "(any org directory + personal accounts), redirect URI \"Public client/native\" http://localhost, "
  "Authentication: allow public client flows; API permissions: Files.ReadWrite. Paste the Application "
  "(client) ID here.",
};
