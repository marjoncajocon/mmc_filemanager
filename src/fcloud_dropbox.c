/* fcloud_dropbox.c -- Dropbox for the cloud panels (fcloud.h), API v2 with
** the user's own app key and PKCE (foauth.h).
**
** Getting an app key (free, once):
**   1. dropbox.com/developers/apps -> Create app -> "Scoped access",
**      "Full Dropbox" (or "App folder"), any name.
**   2. Permissions tab: tick files.metadata.read/write and
**      files.content.read/write (account_info.read is on by default),
**      Submit.
**   3. Settings tab: Allow public clients (Implicit Grant & PKCE) = Allow;
**      OAuth 2 Redirect URIs: add http://127.0.0.1 (any port then works).
**   4. Copy the "App key" into Settings as the client id (no secret).
**
** Design decisions:
**   - Ids are paths without the leading slash, folders ending in '/'
**     ("photos/2026/" ; "" is the root), per fcloud.h; the API wants
**     "/photos/2026" and "" for the root (dropbox_api_path).
**   - The sign-in asks token_access_type=offline for a refresh token; no
**     scope is sent, so the app's ticked permissions apply.
**   - Content endpoints take their arguments as JSON in the
**     Dropbox-API-Arg header, which must be ASCII: names are escaped with
**     \uXXXX there (oa_json_quote ascii).
**   - Uploads up to 150 MB are one /upload call; bigger files use an upload
**     session in 8 MB appends (multiples of 4 MB as Dropbox advises), with
**     an incorrect_offset reply moving the offset where Dropbox says.
**   - Delete is delete_v2 (Dropbox keeps deleted files restorable for
**     30+ days). Rename and move are move_v2.
**   - stream_url is a temporary link (4 hours, no header needed).
*/
#include "foauth_int.h"
#include "fjson.h"
#include "fplat.h"
#include "fsdl.h"

#define DB_SVC "Dropbox"
#define DB_API "https://api.dropboxapi.com/2/"
#define DB_CONTENT "https://content.dropboxapi.com/2/"
#define DB_SIMPLE_MAX (150ull << 20)
#define DB_CHUNK (8u << 20)
#define DB_JSON "application/json"

static const FmOAuthCfg kCfg = {
  "https://www.dropbox.com/oauth2/authorize", "https://api.dropboxapi.com/oauth2/token", NULL, NULL,
  NULL, "token_access_type=offline", NULL,
};

static FmOAuthCfg cfg_of(const FmCloudAcct *a) {
  FmOAuthCfg c = kCfg;
  c.client_id = a->client_id;
  c.client_secret = a->client_secret;
  return c;
}

static FmErr db_open(OaSess *s, FmCloudAcct *a, char *err, size_t cap, volatile int *cancel) {
  FmOAuthCfg c = cfg_of(a);
  return oa_open(s, a, &c, DB_SVC, err, cap, cancel);
}

void dropbox_api_path(const char *id, char *out, size_t cap) {
  out[0] = 0;
  if (!id || !*id) return;
  if (*id != '/') fm_strlcpy(out, "/", cap);
  fm_strlcat(out, id, cap);
  size_t n = strlen(out);
  while (n > 1 && out[n - 1] == '/') out[--n] = 0;
  if (!strcmp(out, "/")) out[0] = 0;
}

/* api path of `name` inside folder id dir */
static void child_path(const char *dir, const char *name, char *out, size_t cap) {
  dropbox_api_path(dir, out, cap);
  fm_strlcat(out, "/", cap);
  fm_strlcat(out, name, cap);
}

/* ---- parsing ---------------------------------------------------------------------- */

/* kind: 1 file, 2 folder, 0 from ".tag" */
static bool entry_from(const FmJsonNode *n, int kind, FmCloudEntry *e) {
  if (!kind) {
    const char *tag = json_str(json_get(n, ".tag"), "");
    kind = !strcmp(tag, "file") ? 1 : !strcmp(tag, "folder") ? 2 : 0;
    if (!kind) return false;                                     /* "deleted" */
  }
  const char *path = json_str(json_get(n, "path_display"), json_str(json_get(n, "path_lower"), ""));
  if (*path != '/' || strlen(path) + 2 >= sizeof e->id) return false;
  memset(e, 0, sizeof *e);
  fm_strlcpy(e->id, path + 1, sizeof e->id);
  e->dir = kind == 2;
  if (e->dir) fm_strlcat(e->id, "/", sizeof e->id);
  fm_strlcpy(e->name, json_str(json_get(n, "name"), fm_path_base(path)), sizeof e->name);
  if (!e->dir) {
    double sz = json_num(json_get(n, "size"), 0);
    e->size = sz > 0 ? (u64)sz : 0;
    e->mtime = cloud_parse_time(json_str(json_get(n, "client_modified"), json_str(json_get(n, "server_modified"), "")));
    const char *h = json_str(json_get(n, "content_hash"), "");
    if (*h && strlen(h) <= 64) fm_snprintf(e->hash, sizeof e->hash, "dbx:%s", h);
  }
  return true;
}

FmErr dropbox_parse_list(const char *json, size_t len, FmCloudList *out, char *cursor, size_t ccap, bool *more) {
  FmJson j;
  *more = false;
  if (ccap) cursor[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "Dropbox sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *r = json_root(&j), *ents = json_get(r, "entries");
  if (!ents || ents->type != JSON_ARR) {
    json_free(&j);
    fm_strlcpy(out->error, "Dropbox sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  for (const FmJsonNode *n = json_first(ents); n; n = json_next(n)) {
    FmCloudEntry e;
    if (entry_from(n, 0, &e)) *cloud_list_add(out) = e;
  }
  const char *c = json_str(json_get(r, "cursor"), "");
  if (strlen(c) < ccap) fm_strlcpy(cursor, c, ccap);
  *more = json_bool(json_get(r, "has_more"), false) && cursor[0];
  json_free(&j);
  return FM_OK;
}

bool dropbox_parse_quota(const char *json, size_t len, u64 *used, u64 *total) {
  FmJson j;
  if (!json || json_parse(&j, json, len) != FM_OK) return false;
  const FmJsonNode *r = json_root(&j);
  double u = json_num(json_get(r, "used"), -1), t = json_num(json_path(r, "allocation.allocated"), 0);
  json_free(&j);
  if (u < 0) return false;
  *used = (u64)u;
  *total = t > 0 ? (u64)t : 0;
  return true;
}

static bool parse_meta(const FmNetResp *r, const char *member, int kind, FmCloudEntry *e) {
  FmJson j;
  if (!r->data || json_parse(&j, (const char *)r->data, r->len) != FM_OK) return false;
  const FmJsonNode *n = member ? json_get(json_root(&j), member) : json_root(&j);
  bool ok = n && entry_from(n, kind, e);
  json_free(&j);
  return ok;
}

/* ---- calls -------------------------------------------------------------------------- */

/* RPC call: JSON in, JSON out. */
static FmErr rpc(OaSess *s, const char *fn, const char *body, FmNetResp *r) {
  char url[160];
  fm_snprintf(url, sizeof url, DB_API "%s", fn);
  FmErr e = oa_send(s, "POST", url, DB_JSON, body ? body : "null", NULL, r);
  if (e != FM_OK || !oa_ok(r)) {
    e = oa_fail(s, r, e);
    net_resp_free(r);
    return e;
  }
  return FM_OK;
}

static FmErr db_login(FmCloudAcct *a, char *err, size_t errcap, volatile int *cancel) {
  FmOAuthCfg c = cfg_of(a);
  FmErr e = oa_login(a, &c, DB_SVC, err, errcap, cancel);
  if (e != FM_OK) return e;
  OaSess s;
  FmNetResp r;
  if (oa_open(&s, a, &c, DB_SVC, err, errcap, cancel) == FM_OK && rpc(&s, "users/get_current_account", NULL, &r) == FM_OK) {
    FmJson j;
    if (json_parse(&j, (const char *)r.data, r.len) == FM_OK) {
      const char *mail = json_str(json_get(json_root(&j), "email"), "");
      if (!a->user[0]) fm_strlcpy(a->user, mail, sizeof a->user);
      if (!a->label[0] && *mail) fm_strlcpy(a->label, mail, sizeof a->label);
      json_free(&j);
    }
    net_resp_free(&r);
  }
  err[0] = 0;
  return FM_OK;
}

static FmErr db_list(FmCloudAcct *a, const char *dir_id, FmCloudList *out, volatile int *cancel) {
  OaSess s;
  FmErr e = db_open(&s, a, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  char path[1200], qp[2600], cursor[2048], qc[2200];
  char *body = (char *)fm_alloc(4096);
  dropbox_api_path(dir_id, path, sizeof path);
  oa_json_quote(path, qp, sizeof qp, false);
  fm_snprintf(body, 4096, "{\"path\":%s,\"limit\":2000}", qp);
  const char *fn = "files/list_folder";
  for (int page = 0; page < 10000; page++) {
    if (cancel && *cancel) { e = oa_fail(&s, NULL, FM_ERR_CANCEL); break; }
    FmNetResp r;
    e = rpc(&s, fn, body, &r);
    if (e != FM_OK) break;
    bool more;
    e = dropbox_parse_list((const char *)r.data, r.len, out, cursor, sizeof cursor, &more);
    net_resp_free(&r);
    if (e != FM_OK || !more) break;
    oa_json_quote(cursor, qc, sizeof qc, true);
    fm_snprintf(body, 4096, "{\"cursor\":%s}", qc);
    fn = "files/list_folder/continue";
  }
  fm_free(body);
  return e;
}

/* "Dropbox-API-Arg: <json>\r\n" with the JSON ASCII-only */
static void arg_header(char *out, size_t cap, const char *json) {
  fm_snprintf(out, cap, "Dropbox-API-Arg: %s\r\n", json);
}

static FmErr db_download(FmCloudAcct *a, const FmCloudEntry *en, const char *local_path, FmNetProgress cb,
                         void *user, char *err, size_t errcap, volatile int *cancel) {
  OaSess s;
  FmErr e = db_open(&s, a, err, errcap, cancel);
  if (e != FM_OK) return e;
  char path[1200], qp[3000], arg[3100], hdr[3300];
  dropbox_api_path(en->id, path, sizeof path);
  oa_json_quote(path, qp, sizeof qp, true);
  fm_snprintf(arg, sizeof arg, "{\"path\":%s}", qp);
  arg_header(hdr, sizeof hdr, arg);
  FmNetReq q;
  memset(&q, 0, sizeof q);
  q.method = "POST";
  q.headers = hdr;
  return oa_download(&s, DB_CONTENT "files/download", &q, local_path, cb, user);
}

static FmErr content_call(OaSess *s, const char *fn, const char *arg, const char *local_path, u64 off, u64 len,
                          OaProg *pg, FmNetResp *r) {
  char url[160], *hdr = (char *)fm_alloc(5000);
  fm_snprintf(url, sizeof url, DB_CONTENT "%s", fn);
  fm_snprintf(hdr, 5000, "Content-Type: application/octet-stream\r\nDropbox-API-Arg: %s\r\n", arg);
  FmNetReq q;
  memset(&q, 0, sizeof q);
  q.method = "POST";
  q.headers = hdr;
  if (len) {
    q.body_file = local_path;
    q.body_off = (i64)off;
    q.body_file_len = (i64)len;
    pg->base = off;
    pg->chunk = len;
    q.progress = oa_prog;
    q.user = pg;
  }
  FmErr e = oa_call(s, url, &q, true, r);
  fm_free(hdr);
  return e;
}

/* incorrect_offset -> the offset Dropbox has, else -1 */
static i64 correct_offset(const FmNetResp *r) {
  FmJson j;
  i64 v = -1;
  if (r->data && json_parse(&j, (const char *)r->data, r->len) == FM_OK) {
    double d = json_num(json_path(json_root(&j), "error.correct_offset"), -1);
    if (d < 0) d = json_num(json_path(json_root(&j), "error.lookup_failed.correct_offset"), -1);
    if (d >= 0) v = (i64)d;
    json_free(&j);
  }
  return v;
}

static FmErr db_upload(FmCloudAcct *a, const char *dir_id, const char *local_path, const char *name, FmCloudEntry *out,
                       FmNetProgress cb, void *user, char *err, size_t errcap, volatile int *cancel) {
  OaSess s;
  FmErr e = db_open(&s, a, err, errcap, cancel);
  if (e != FM_OK) return e;
  FmStat st;
  if (!plat_stat(local_path, &st) || (st.flags & FM_ST_DIR)) {
    fm_strlcpy(err, "Cannot read the file to upload", errcap);
    return FM_ERR_IO;
  }
  char path[1400], qp[3000], commit[3200], arg[4400];
  child_path(dir_id, name, path, sizeof path);
  oa_json_quote(path, qp, sizeof qp, true);
  fm_snprintf(commit, sizeof commit, "{\"path\":%s,\"mode\":\"overwrite\",\"autorename\":false,\"mute\":true}", qp);
  OaProg pg = { cb, user, 0, 0, st.size };
  FmNetResp r;
  if (st.size <= DB_SIMPLE_MAX) {
    e = content_call(&s, "files/upload", commit, local_path, 0, st.size, &pg, &r);
    if (e != FM_OK || !oa_ok(&r)) {
      e = oa_fail(&s, &r, e);
      net_resp_free(&r);
      return e;
    }
    if (out && !parse_meta(&r, NULL, 1, out)) memset(out, 0, sizeof *out);
    net_resp_free(&r);
    return FM_OK;
  }
  /* upload session */
  u64 off = 0, len = FM_MIN((u64)DB_CHUNK, st.size);
  e = content_call(&s, "files/upload_session/start", "{\"close\":false}", local_path, 0, len, &pg, &r);
  char sid[256];
  sid[0] = 0;
  if (e == FM_OK && oa_ok(&r)) {
    FmJson j;
    if (json_parse(&j, (const char *)r.data, r.len) == FM_OK) {
      fm_strlcpy(sid, json_str(json_get(json_root(&j), "session_id"), ""), sizeof sid);
      json_free(&j);
    }
  }
  if (!sid[0]) {
    if (e == FM_OK && oa_ok(&r)) {
      fm_strlcpy(err, "Dropbox did not start the upload", errcap);
      e = FM_ERR_IO;
    } else {
      e = oa_fail(&s, &r, e);
    }
    net_resp_free(&r);
    return e;
  }
  net_resp_free(&r);
  off = len;
  int fails = 0;
  char qs[600];
  oa_json_quote(sid, qs, sizeof qs, true);
  while (off < st.size) {
    if (cancel && *cancel) return oa_fail(&s, NULL, FM_ERR_CANCEL);
    len = FM_MIN((u64)DB_CHUNK, st.size - off);
    fm_snprintf(arg, sizeof arg, "{\"cursor\":{\"session_id\":%s,\"offset\":%llu},\"close\":false}", qs,
                (unsigned long long)off);
    e = content_call(&s, "files/upload_session/append_v2", arg, local_path, off, len, &pg, &r);
    if (e == FM_OK && oa_ok(&r)) {
      off += len;
      fails = 0;
      net_resp_free(&r);
      continue;
    }
    i64 co = e == FM_OK && r.status == 409 ? correct_offset(&r) : -1;
    bool retry = co >= 0 || e == FM_ERR_IO || (e == FM_OK && (r.status >= 500 || r.status == 429));
    if (!retry || ++fails > 5) {
      e = oa_fail(&s, &r, e);
      net_resp_free(&r);
      return e;
    }
    net_resp_free(&r);
    if (co >= 0 && (u64)co <= st.size) off = (u64)co;
    else for (int i = 0; i < fails * 4 && !(cancel && *cancel); i++) SDL_Delay(250);
  }
  fm_snprintf(arg, sizeof arg, "{\"cursor\":{\"session_id\":%s,\"offset\":%llu},\"commit\":%s}", qs,
              (unsigned long long)st.size, commit);
  for (fails = 0;; fails++) {
    e = content_call(&s, "files/upload_session/finish", arg, local_path, 0, 0, &pg, &r);
    if (e == FM_OK && oa_ok(&r)) break;
    if (fails >= 3 || !(e == FM_ERR_IO || (e == FM_OK && r.status >= 500))) {
      e = oa_fail(&s, &r, e);
      net_resp_free(&r);
      return e;
    }
    net_resp_free(&r);
    SDL_Delay(1000);
  }
  if (out && !parse_meta(&r, NULL, 1, out)) memset(out, 0, sizeof *out);
  net_resp_free(&r);
  return FM_OK;
}

static FmErr db_mkdir(FmCloudAcct *a, const char *parent_id, const char *name, FmCloudEntry *out, char *err,
                      size_t errcap, volatile int *cancel) {
  OaSess s;
  FmErr e = db_open(&s, a, err, errcap, cancel);
  if (e != FM_OK) return e;
  char path[1400], qp[3000], body[3100];
  child_path(parent_id, name, path, sizeof path);
  oa_json_quote(path, qp, sizeof qp, false);
  fm_snprintf(body, sizeof body, "{\"path\":%s,\"autorename\":false}", qp);
  FmNetResp r;
  e = rpc(&s, "files/create_folder_v2", body, &r);
  if (e != FM_OK) return e;
  if (out && !parse_meta(&r, "metadata", 2, out)) memset(out, 0, sizeof *out);
  net_resp_free(&r);
  return FM_OK;
}

static FmErr db_remove(FmCloudAcct *a, const FmCloudEntry *en, char *err, size_t errcap, volatile int *cancel) {
  OaSess s;
  FmErr e = db_open(&s, a, err, errcap, cancel);
  if (e != FM_OK) return e;
  char path[1200], qp[3000], body[3100];
  dropbox_api_path(en->id, path, sizeof path);
  if (!path[0]) {
    fm_strlcpy(err, "The Dropbox root cannot be deleted", errcap);
    return FM_ERR_ACCESS;
  }
  oa_json_quote(path, qp, sizeof qp, false);
  fm_snprintf(body, sizeof body, "{\"path\":%s}", qp);
  FmNetResp r;
  e = rpc(&s, "files/delete_v2", body, &r);
  if (e == FM_OK) net_resp_free(&r);
  return e;
}

static FmErr move_to(FmCloudAcct *a, const char *from_id, const char *to_path, char *err, size_t errcap,
                     volatile int *cancel) {
  OaSess s;
  FmErr e = db_open(&s, a, err, errcap, cancel);
  if (e != FM_OK) return e;
  char from[1200], qf[3000], qt[3000], *body = (char *)fm_alloc(6200);
  dropbox_api_path(from_id, from, sizeof from);
  oa_json_quote(from, qf, sizeof qf, false);
  oa_json_quote(to_path, qt, sizeof qt, false);
  fm_snprintf(body, 6200, "{\"from_path\":%s,\"to_path\":%s,\"autorename\":false}", qf, qt);
  FmNetResp r;
  e = rpc(&s, "files/move_v2", body, &r);
  fm_free(body);
  if (e == FM_OK) net_resp_free(&r);
  return e;
}

static FmErr db_rename(FmCloudAcct *a, const FmCloudEntry *en, const char *new_name, char *err, size_t errcap,
                       volatile int *cancel) {
  char path[1200], to[1500];
  dropbox_api_path(en->id, path, sizeof path);
  char *sl = strrchr(path, '/');
  if (!path[0] || !sl) {
    fm_strlcpy(err, "The Dropbox root cannot be renamed", errcap);
    return FM_ERR_ACCESS;
  }
  *sl = 0;
  fm_snprintf(to, sizeof to, "%s/%s", path, new_name);
  return move_to(a, en->id, to, err, errcap, cancel);
}

static FmErr db_move(FmCloudAcct *a, const FmCloudEntry *en, const char *new_parent_id, char *err, size_t errcap,
                     volatile int *cancel) {
  char path[1200], to[1500];
  dropbox_api_path(en->id, path, sizeof path);
  const char *sl = strrchr(path, '/');
  if (!path[0] || !sl) {
    fm_strlcpy(err, "The Dropbox root cannot be moved", errcap);
    return FM_ERR_ACCESS;
  }
  child_path(new_parent_id, sl + 1, to, sizeof to);
  return move_to(a, en->id, to, err, errcap, cancel);
}

static FmErr db_quota(FmCloudAcct *a, u64 *used, u64 *total, char *err, size_t errcap, volatile int *cancel) {
  OaSess s;
  FmErr e = db_open(&s, a, err, errcap, cancel);
  if (e != FM_OK) return e;
  FmNetResp r;
  e = rpc(&s, "users/get_space_usage", NULL, &r);
  if (e != FM_OK) return e;
  if (!dropbox_parse_quota((const char *)r.data, r.len, used, total)) {
    fm_strlcpy(err, "Dropbox did not say how much space is used", errcap);
    e = FM_ERR_FORMAT;
  }
  net_resp_free(&r);
  return e;
}

static FmErr db_stream_url(FmCloudAcct *a, const FmCloudEntry *en, char *url, size_t urlcap, char *headers,
                           size_t hcap, char *err, size_t errcap, volatile int *cancel) {
  OaSess s;
  FmErr e = db_open(&s, a, err, errcap, cancel);
  if (e != FM_OK) return e;
  char path[1200], qp[3000], body[3100];
  dropbox_api_path(en->id, path, sizeof path);
  oa_json_quote(path, qp, sizeof qp, false);
  fm_snprintf(body, sizeof body, "{\"path\":%s}", qp);
  FmNetResp r;
  e = rpc(&s, "files/get_temporary_link", body, &r);
  if (e != FM_OK) return e;
  FmJson j;
  e = FM_ERR_FORMAT;
  if (json_parse(&j, (const char *)r.data, r.len) == FM_OK) {
    const char *link = json_str(json_get(json_root(&j), "link"), "");
    if (!fm_strnicmp(link, "https://", 8) && strlen(link) < urlcap) {
      fm_strlcpy(url, link, urlcap);
      if (hcap) headers[0] = 0;
      e = FM_OK;
    }
    json_free(&j);
  }
  if (e != FM_OK) fm_strlcpy(err, "Dropbox gave no link to play from", errcap);
  net_resp_free(&r);
  return e;
}

const FmCloud g_cloud_dropbox = {
  "dropbox", "Dropbox", IC_CLOUD, CLOUD_OAUTH | CLOUD_UPLOAD | CLOUD_STREAM,
  db_login, db_list, db_download, db_upload, db_mkdir, db_remove, db_rename, db_move, db_quota, db_stream_url, NULL,
  "Sign in with Dropbox. Needs your own free app key: dropbox.com/developers/apps > Create app > Scoped access; "
  "Permissions: tick files.metadata and files.content read/write; Settings: allow public clients (PKCE) and add "
  "the redirect URI http://127.0.0.1. Paste the App key here as the client id.",
};
