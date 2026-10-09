/* fcloud_gdrive.c -- Google Drive for the cloud panels (fcloud.h), Drive
** API v3 with the user's own OAuth client (foauth.h).
**
** Getting a client id (free, once):
**   1. console.cloud.google.com -> create a project (any name).
**   2. APIs & Services -> Library -> "Google Drive API" -> Enable.
**   3. APIs & Services -> OAuth consent screen (Google Auth Platform):
**      External, app name + your email; under Audience add your Google
**      account as a test user (or Publish the app).
**   4. Credentials -> Create credentials -> OAuth client ID -> type
**      "Desktop app". Copy the client id AND the client secret into
**      Settings (Google wants the secret for desktop clients even with
**      PKCE; it is not a real secret there). Loopback redirects to
**      http://127.0.0.1 with any port are allowed for Desktop clients.
**
** Design decisions:
**   - Ids are Drive file ids; "" is "root". Listing: files.list with
**     q="'<id>' in parents and trashed=false", 1000 per page, pageToken.
**     Shared drives are included (supportsAllDrives everywhere).
**   - Google Docs / Sheets / Slides / Drawings have no bytes of their own:
**     they are listed with the Google mime ("application/vnd.google-apps.
**     document") and the export extension added to the name (".docx",
**     ".xlsx", ".pptx", ".pdf"), and download as that export (files.export,
**     which Google caps at 10 MB). Forms, sites, maps and shortcuts are not
**     listed: nothing could be done with them.
**   - Uploads up to 5 MB go in one multipart request (metadata + bytes,
**     read into memory); bigger files use a resumable session in 8 MB
**     chunks (a multiple of 256 KB) read straight from the file; after a
**     network error or 5xx the session is asked how far it got and the
**     upload goes on from there. The chunk PUTs set no_redirect: Drive's
**     "308 Resume Incomplete" has no Location and WinHTTP would otherwise
**     try to follow it and fail (12156). A file of the same name in the folder is
**     replaced (new revision of the same id) instead of duplicated.
**   - Delete = move to the Drive trash (PATCH trashed=true).
**   - stream_url is files/<id>?alt=media with the bearer header; Drive
**     honours Range requests there.
*/
#include "foauth_int.h"
#include "fjson.h"
#include "fplat.h"
#include "fsdl.h"

#define GD_SVC "Google Drive"
#define GD_API "https://www.googleapis.com/drive/v3/"
#define GD_UP "https://www.googleapis.com/upload/drive/v3/"
#define GD_FIELDS "id,name,mimeType,size,modifiedTime,md5Checksum"
#define GD_FOLDER "application/vnd.google-apps.folder"
#define GD_APPS "application/vnd.google-apps."
#define GD_SIMPLE_MAX (5u << 20)
#define GD_CHUNK (8u << 20)

static const FmOAuthCfg kCfg = {
  "https://accounts.google.com/o/oauth2/v2/auth", "https://oauth2.googleapis.com/token", NULL, NULL,
  "https://www.googleapis.com/auth/drive", "access_type=offline&prompt=consent", NULL,
};

static const struct { const char *gmime, *mime, *ext; } kExport[] = {
  { GD_APPS "document", "application/vnd.openxmlformats-officedocument.wordprocessingml.document", ".docx" },
  { GD_APPS "spreadsheet", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", ".xlsx" },
  { GD_APPS "presentation", "application/vnd.openxmlformats-officedocument.presentationml.presentation", ".pptx" },
  { GD_APPS "drawing", "application/pdf", ".pdf" },
};

static int export_of(const char *mime) {
  for (int i = 0; i < FM_COUNT(kExport); i++) if (!strcmp(mime, kExport[i].gmime)) return i;
  return -1;
}

static FmOAuthCfg cfg_of(const FmCloudAcct *a) {
  FmOAuthCfg c = kCfg;
  c.client_id = a->client_id;
  c.client_secret = a->client_secret;
  return c;
}

static FmErr gd_open(OaSess *s, FmCloudAcct *a, char *err, size_t cap, volatile int *cancel) {
  FmOAuthCfg c = cfg_of(a);
  return oa_open(s, a, &c, GD_SVC, err, cap, cancel);
}

/* Drive ids are [A-Za-z0-9_-]; "" is the root. NULL for anything else (no query injection). */
static const char *gd_id(const char *id) {
  if (!id || !*id) return "root";
  size_t n = strlen(id);
  if (n > 200) return NULL;
  for (const char *p = id; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
      return NULL;
  return id;
}

static FmErr bad_id(char *err, size_t cap) {
  fm_strlcpy(err, "Not a Google Drive item", cap);
  return FM_ERR_NOT_FOUND;
}

/* ---- parsing ---------------------------------------------------------------------- */

/* false for items that are skipped (forms, shortcuts...) */
static bool entry_from(const FmJsonNode *n, FmCloudEntry *e) {
  const char *id = json_str(json_get(n, "id"), "");
  const char *mime = json_str(json_get(n, "mimeType"), "");
  if (!*id || strlen(id) >= sizeof e->id) return false;
  memset(e, 0, sizeof *e);
  fm_strlcpy(e->id, id, sizeof e->id);
  fm_strlcpy(e->name, json_str(json_get(n, "name"), id), sizeof e->name);
  fm_strlcpy(e->mime, mime, sizeof e->mime);
  e->mtime = cloud_parse_time(json_str(json_get(n, "modifiedTime"), ""));
  if (!strcmp(mime, GD_FOLDER)) {
    e->dir = true;
    return true;
  }
  if (!strncmp(mime, GD_APPS, strlen(GD_APPS))) {
    int x = export_of(mime);
    if (x < 0) return false;
    if (!fm_ends_with_i(e->name, kExport[x].ext)) fm_strlcat(e->name, kExport[x].ext, sizeof e->name);
    return true;                                         /* size unknown until exported */
  }
  double sz = json_num(json_get(n, "size"), 0);          /* a numeric string */
  e->size = sz > 0 ? (u64)sz : 0;
  const char *md5 = json_str(json_get(n, "md5Checksum"), "");
  if (*md5 && strlen(md5) < 60) fm_snprintf(e->hash, sizeof e->hash, "md5:%s", md5);
  return true;
}

FmErr gdrive_parse_list(const char *json, size_t len, FmCloudList *out, char *next, size_t ncap) {
  FmJson j;
  if (ncap) next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "Google Drive sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *files = json_get(json_root(&j), "files");
  if (!files || files->type != JSON_ARR) {
    json_free(&j);
    fm_strlcpy(out->error, "Google Drive sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  for (const FmJsonNode *n = json_first(files); n; n = json_next(n)) {
    FmCloudEntry e;
    if (entry_from(n, &e)) *cloud_list_add(out) = e;
  }
  const char *t = json_str(json_get(json_root(&j), "nextPageToken"), "");
  if (strlen(t) < ncap) fm_strlcpy(next, t, ncap);
  json_free(&j);
  return FM_OK;
}

bool gdrive_parse_entry(const char *json, size_t len, FmCloudEntry *e) {
  FmJson j;
  if (!json || json_parse(&j, json, len) != FM_OK) return false;
  bool ok = entry_from(json_root(&j), e);
  json_free(&j);
  return ok;
}

bool gdrive_parse_quota(const char *json, size_t len, u64 *used, u64 *total) {
  FmJson j;
  if (!json || json_parse(&j, json, len) != FM_OK) return false;
  const FmJsonNode *q = json_get(json_root(&j), "storageQuota");
  double u = json_num(json_get(q, "usage"), -1), l = json_num(json_get(q, "limit"), 0);
  json_free(&j);
  if (u < 0) return false;
  *used = (u64)u;
  *total = l > 0 ? (u64)l : 0;                 /* no limit: unlimited (Workspace pooled) */
  return true;
}

/* 'name' inside a Drive query: \ and ' escaped */
static void q_escape(const char *s, char *out, size_t cap) {
  size_t o = 0;
  for (; *s && o + 3 < cap; s++) {
    if (*s == '\'' || *s == '\\') out[o++] = '\\';
    out[o++] = *s;
  }
  out[o] = 0;
}

/* ---- calls -------------------------------------------------------------------------- */

static FmErr gd_login(FmCloudAcct *a, char *err, size_t errcap, volatile int *cancel) {
  FmOAuthCfg c = cfg_of(a);
  FmErr e = oa_login(a, &c, GD_SVC, err, errcap, cancel);
  if (e != FM_OK) return e;
  OaSess s;
  if (oa_open(&s, a, &c, GD_SVC, err, errcap, cancel) != FM_OK) return FM_OK;
  FmNetResp r;
  if (oa_send(&s, "GET", GD_API "about?fields=user(emailAddress,displayName)", NULL, NULL, NULL, &r) == FM_OK &&
      oa_ok(&r)) {
    FmJson j;
    if (json_parse(&j, (const char *)r.data, r.len) == FM_OK) {
      const char *mail = json_str(json_path(json_root(&j), "user.emailAddress"), "");
      if (!a->user[0]) fm_strlcpy(a->user, mail, sizeof a->user);
      if (!a->label[0] && *mail) fm_strlcpy(a->label, mail, sizeof a->label);
      json_free(&j);
    }
  }
  net_resp_free(&r);
  err[0] = 0;
  return FM_OK;
}

static FmErr gd_list(FmCloudAcct *a, const char *dir_id, FmCloudList *out, volatile int *cancel) {
  OaSess s;
  FmErr e = gd_open(&s, a, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  const char *pid = gd_id(dir_id);
  if (!pid) return bad_id(out->error, sizeof out->error);
  char q[300], qe[900], fe[300], next[1024], ne[3100], url[4600];
  fm_snprintf(q, sizeof q, "'%s' in parents and trashed = false", pid);
  net_urlencode(q, qe, sizeof qe);
  net_urlencode("nextPageToken,files(" GD_FIELDS ")", fe, sizeof fe);
  next[0] = 0;
  for (int page = 0; page < 10000; page++) {
    if (cancel && *cancel) { fm_strlcpy(out->error, "Cancelled", sizeof out->error); return FM_ERR_CANCEL; }
    net_urlencode(next, ne, sizeof ne);
    fm_snprintf(url, sizeof url,
                GD_API "files?q=%s&fields=%s&pageSize=1000&supportsAllDrives=true&includeItemsFromAllDrives=true%s%s",
                qe, fe, next[0] ? "&pageToken=" : "", ne);
    FmNetResp r;
    e = oa_send(&s, "GET", url, NULL, NULL, NULL, &r);
    if (e != FM_OK || !oa_ok(&r)) {
      e = oa_fail(&s, &r, e);
      net_resp_free(&r);
      return e;
    }
    e = gdrive_parse_list((const char *)r.data, r.len, out, next, sizeof next);
    net_resp_free(&r);
    if (e != FM_OK || !next[0]) return e;
  }
  return FM_OK;
}

static FmErr gd_download(FmCloudAcct *a, const FmCloudEntry *en, const char *local_path, FmNetProgress cb,
                         void *user, char *err, size_t errcap, volatile int *cancel) {
  OaSess s;
  FmErr e = gd_open(&s, a, err, errcap, cancel);
  if (e != FM_OK) return e;
  const char *id = gd_id(en->id);
  if (!id || !en->id[0]) return bad_id(err, errcap);
  char url[600], me[300];
  int x = export_of(en->mime);
  if (x >= 0) {
    net_urlencode(kExport[x].mime, me, sizeof me);
    fm_snprintf(url, sizeof url, GD_API "files/%s/export?mimeType=%s", id, me);
  } else {
    fm_snprintf(url, sizeof url, GD_API "files/%s?alt=media&supportsAllDrives=true", id);
  }
  FmNetReq q;
  memset(&q, 0, sizeof q);
  return oa_download(&s, url, &q, local_path, cb, user);
}

/* the id of a file called name in folder pid, "" when none */
static FmErr gd_find(OaSess *s, const char *pid, const char *name, char *id, size_t cap) {
  char nq[800], q[1200], qe[3600], url[4000];
  id[0] = 0;
  q_escape(name, nq, sizeof nq);
  fm_snprintf(q, sizeof q, "name = '%s' and '%s' in parents and trashed = false and mimeType != '" GD_FOLDER "'",
              nq, pid);
  net_urlencode(q, qe, sizeof qe);
  fm_snprintf(url, sizeof url, GD_API "files?q=%s&fields=files(id)&pageSize=1&supportsAllDrives=true"
              "&includeItemsFromAllDrives=true", qe);
  FmNetResp r;
  FmErr e = oa_send(s, "GET", url, NULL, NULL, NULL, &r);
  if (e != FM_OK || !oa_ok(&r)) {
    e = oa_fail(s, &r, e);
    net_resp_free(&r);
    return e;
  }
  FmJson j;
  if (json_parse(&j, (const char *)r.data, r.len) == FM_OK) {
    const char *f = json_str(json_path(json_root(&j), "files.0.id"), "");
    if (gd_id(f) && strlen(f) < cap) fm_strlcpy(id, f, cap);
    json_free(&j);
  }
  net_resp_free(&r);
  return FM_OK;
}

static FmErr finish_entry(OaSess *s, FmNetResp *r, FmCloudEntry *out) {
  if (out && !gdrive_parse_entry((const char *)r->data, r->len, out)) memset(out, 0, sizeof *out);
  net_resp_free(r);
  FM_UNUSED(s);
  return FM_OK;
}

static FmErr gd_upload_small(OaSess *s, const char *exist, const char *meta, const char *local_path, u64 size,
                             FmCloudEntry *out, FmNetProgress cb, void *user) {
  static const char kB[] = "mmcfm-part-8d0c51f2a7";
  char head[1400], url[600];
  fm_snprintf(head, sizeof head,
              "--%s\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n%s\r\n--%s\r\n"
              "Content-Type: application/octet-stream\r\n\r\n", kB, meta, kB);
  char tail[64];
  fm_snprintf(tail, sizeof tail, "\r\n--%s--\r\n", kB);
  size_t hn = strlen(head), tn = strlen(tail);
  u8 *body = (u8 *)fm_alloc(hn + (size_t)size + tn + 1);
  memcpy(body, head, hn);
  FILE *f = fm_fopen(local_path, "rb");
  size_t got = f ? fread(body + hn, 1, (size_t)size, f) : 0;
  if (f) fclose(f);
  if (!f || got != (size_t)size) {
    fm_free(body);
    fm_strlcpy(s->err, "Cannot read the file to upload", s->errcap);
    return FM_ERR_IO;
  }
  memcpy(body + hn + size, tail, tn);
  if (*exist) fm_snprintf(url, sizeof url, GD_UP "files/%s?uploadType=multipart&supportsAllDrives=true&fields=" GD_FIELDS, exist);
  else fm_snprintf(url, sizeof url, GD_UP "files?uploadType=multipart&supportsAllDrives=true&fields=" GD_FIELDS);
  char hdr[128];
  fm_snprintf(hdr, sizeof hdr, "Content-Type: multipart/related; boundary=%s\r\n", kB);
  FmNetReq q;
  memset(&q, 0, sizeof q);
  q.method = *exist ? "PATCH" : "POST";
  q.headers = hdr;
  q.body = body;
  q.body_len = hn + (size_t)size + tn;
  FmNetResp r;
  FmErr e = oa_call(s, url, &q, true, &r);
  fm_free(body);
  if (e != FM_OK || !oa_ok(&r)) {
    e = oa_fail(s, &r, e);
    net_resp_free(&r);
    return e;
  }
  if (cb) cb(user, size, size);
  return finish_entry(s, &r, out);
}

/* "bytes=0-1234" -> 1235; 0 when absent */
static u64 range_end(const FmNetResp *r) {
  char v[96];
  if (!net_resp_header(r, "Range", v, sizeof v)) return 0;
  const char *d = strchr(v, '-');
  if (!d) return 0;
  u64 n = 0;
  for (d++; *d >= '0' && *d <= '9'; d++) n = n * 10 + (u64)(*d - '0');
  return n + 1;
}

static FmErr gd_upload_big(OaSess *s, const char *exist, const char *meta, const char *local_path, u64 size,
                           FmCloudEntry *out, FmNetProgress cb, void *user) {
  char url[600], hdr[256];
  if (*exist) fm_snprintf(url, sizeof url, GD_UP "files/%s?uploadType=resumable&supportsAllDrives=true&fields=" GD_FIELDS, exist);
  else fm_snprintf(url, sizeof url, GD_UP "files?uploadType=resumable&supportsAllDrives=true&fields=" GD_FIELDS);
  fm_snprintf(hdr, sizeof hdr, "X-Upload-Content-Type: application/octet-stream\r\nX-Upload-Content-Length: %llu\r\n",
              (unsigned long long)size);
  FmNetResp r;
  FmErr e = oa_send(s, *exist ? "PATCH" : "POST", url, "application/json; charset=UTF-8", meta, hdr, &r);
  char *loc = (char *)fm_alloc(4096);
  if (e != FM_OK || !oa_ok(&r) || !net_resp_header(&r, "Location", loc, 4096) || fm_strnicmp(loc, "https://", 8)) {
    if (e == FM_OK && oa_ok(&r)) {
      fm_strlcpy(s->err, "Google Drive did not start the upload", s->errcap);
      e = FM_ERR_IO;
    } else {
      e = oa_fail(s, &r, e);
    }
    net_resp_free(&r);
    fm_free(loc);
    return e;
  }
  net_resp_free(&r);
  u64 off = 0;
  int fails = 0;
  OaProg pg = { cb, user, 0, 0, size };
  for (;;) {
    if (s->cancel && *s->cancel) { e = oa_fail(s, NULL, FM_ERR_CANCEL); break; }
    u64 len = FM_MIN((u64)GD_CHUNK, size - off);
    FmNetReq q;
    memset(&q, 0, sizeof q);
    q.method = "PUT";
    q.no_redirect = true;                 /* 308 is "resume incomplete", not a redirect */
    if (len) {
      fm_snprintf(hdr, sizeof hdr, "Content-Range: bytes %llu-%llu/%llu\r\n", (unsigned long long)off,
                  (unsigned long long)(off + len - 1), (unsigned long long)size);
      q.body_file = local_path;
      q.body_off = (i64)off;
      q.body_file_len = (i64)len;
      pg.base = off;
      pg.chunk = len;
      q.progress = oa_prog;
      q.user = &pg;
    } else {
      fm_snprintf(hdr, sizeof hdr, "Content-Range: bytes */%llu\r\n", (unsigned long long)size);
    }
    q.headers = hdr;
    e = oa_call(s, loc, &q, true, &r);
    if (e == FM_OK && (r.status == 200 || r.status == 201)) {
      e = finish_entry(s, &r, out);
      break;
    }
    if (e == FM_OK && r.status == 308) {
      off = range_end(&r);
      net_resp_free(&r);
      if (off > size) off = size;
      fails = 0;
      continue;
    }
    bool retry = e == FM_ERR_IO || (e == FM_OK && (r.status >= 500 || r.status == 429));
    if (!retry || ++fails > 5) {
      e = oa_fail(s, &r, e);
      net_resp_free(&r);
      break;
    }
    net_resp_free(&r);
    for (int i = 0; i < fails * 4 && !(s->cancel && *s->cancel); i++) SDL_Delay(250);
    /* ask where the session is: an empty PUT with "bytes * /size" */
    FmNetReq st;
    memset(&st, 0, sizeof st);
    st.method = "PUT";
    st.no_redirect = true;
    fm_snprintf(hdr, sizeof hdr, "Content-Range: bytes */%llu\r\n", (unsigned long long)size);
    st.headers = hdr;
    e = oa_call(s, loc, &st, true, &r);
    if (e == FM_OK && (r.status == 200 || r.status == 201)) {
      e = finish_entry(s, &r, out);
      break;
    }
    if (e == FM_OK && r.status == 308) off = range_end(&r);
    net_resp_free(&r);
  }
  fm_free(loc);
  return e;
}

static FmErr gd_upload(FmCloudAcct *a, const char *dir_id, const char *local_path, const char *name, FmCloudEntry *out,
                       FmNetProgress cb, void *user, char *err, size_t errcap, volatile int *cancel) {
  OaSess s;
  FmErr e = gd_open(&s, a, err, errcap, cancel);
  if (e != FM_OK) return e;
  const char *pid = gd_id(dir_id);
  if (!pid) return bad_id(err, errcap);
  FmStat st;
  if (!plat_stat(local_path, &st) || (st.flags & FM_ST_DIR)) {
    fm_strlcpy(err, "Cannot read the file to upload", errcap);
    return FM_ERR_IO;
  }
  char exist[256];
  e = gd_find(&s, pid, name, exist, sizeof exist);
  if (e != FM_OK) return e;
  char qn[1100], meta[1300];
  oa_json_quote(name, qn, sizeof qn, false);
  if (*exist) fm_strlcpy(meta, "{}", sizeof meta);              /* new revision, same name and place */
  else fm_snprintf(meta, sizeof meta, "{\"name\":%s,\"parents\":[\"%s\"]}", qn, pid);
  if (st.size <= GD_SIMPLE_MAX) return gd_upload_small(&s, exist, meta, local_path, st.size, out, cb, user);
  return gd_upload_big(&s, exist, meta, local_path, st.size, out, cb, user);
}

static FmErr gd_mkdir(FmCloudAcct *a, const char *parent_id, const char *name, FmCloudEntry *out, char *err,
                      size_t errcap, volatile int *cancel) {
  OaSess s;
  FmErr e = gd_open(&s, a, err, errcap, cancel);
  if (e != FM_OK) return e;
  const char *pid = gd_id(parent_id);
  if (!pid) return bad_id(err, errcap);
  char qn[1100], body[1400];
  oa_json_quote(name, qn, sizeof qn, false);
  fm_snprintf(body, sizeof body, "{\"name\":%s,\"mimeType\":\"" GD_FOLDER "\",\"parents\":[\"%s\"]}", qn, pid);
  FmNetResp r;
  e = oa_send(&s, "POST", GD_API "files?supportsAllDrives=true&fields=" GD_FIELDS, "application/json; charset=UTF-8",
              body, NULL, &r);
  if (e != FM_OK || !oa_ok(&r)) {
    e = oa_fail(&s, &r, e);
    net_resp_free(&r);
    return e;
  }
  return finish_entry(&s, &r, out);
}

static FmErr gd_patch(FmCloudAcct *a, const char *id_in, const char *query, const char *body, char *err,
                      size_t errcap, volatile int *cancel) {
  OaSess s;
  FmErr e = gd_open(&s, a, err, errcap, cancel);
  if (e != FM_OK) return e;
  const char *id = gd_id(id_in);
  if (!id || !id_in[0]) return bad_id(err, errcap);
  char url[2400];
  fm_snprintf(url, sizeof url, GD_API "files/%s?supportsAllDrives=true&fields=id%s", id, query ? query : "");
  FmNetResp r;
  e = oa_send(&s, "PATCH", url, "application/json; charset=UTF-8", body, NULL, &r);
  if (e != FM_OK || !oa_ok(&r)) e = oa_fail(&s, &r, e);
  net_resp_free(&r);
  return e;
}

static FmErr gd_remove(FmCloudAcct *a, const FmCloudEntry *en, char *err, size_t errcap, volatile int *cancel) {
  return gd_patch(a, en->id, NULL, "{\"trashed\":true}", err, errcap, cancel);
}

static FmErr gd_rename(FmCloudAcct *a, const FmCloudEntry *en, const char *new_name, char *err, size_t errcap,
                       volatile int *cancel) {
  char name[512], qn[1100], body[1200];
  fm_strlcpy(name, new_name, sizeof name);
  int x = export_of(en->mime);                   /* "Report.docx" stays "Report" in Drive */
  if (x >= 0 && fm_ends_with_i(name, kExport[x].ext)) name[strlen(name) - strlen(kExport[x].ext)] = 0;
  oa_json_quote(name, qn, sizeof qn, false);
  fm_snprintf(body, sizeof body, "{\"name\":%s}", qn);
  return gd_patch(a, en->id, NULL, body, err, errcap, cancel);
}

static FmErr gd_move(FmCloudAcct *a, const FmCloudEntry *en, const char *new_parent_id, char *err, size_t errcap,
                     volatile int *cancel) {
  OaSess s;
  FmErr e = gd_open(&s, a, err, errcap, cancel);
  if (e != FM_OK) return e;
  const char *id = gd_id(en->id), *np = gd_id(new_parent_id);
  if (!id || !np || !en->id[0]) return bad_id(err, errcap);
  char url[600], old[1600];
  fm_snprintf(url, sizeof url, GD_API "files/%s?fields=parents&supportsAllDrives=true", id);
  FmNetResp r;
  e = oa_send(&s, "GET", url, NULL, NULL, NULL, &r);
  if (e != FM_OK || !oa_ok(&r)) {
    e = oa_fail(&s, &r, e);
    net_resp_free(&r);
    return e;
  }
  old[0] = 0;
  FmJson j;
  if (json_parse(&j, (const char *)r.data, r.len) == FM_OK) {
    for (const FmJsonNode *p = json_first(json_get(json_root(&j), "parents")); p; p = json_next(p)) {
      const char *pid = json_str(p, "");
      if (!gd_id(pid) || !*pid) continue;
      if (old[0]) fm_strlcat(old, ",", sizeof old);
      fm_strlcat(old, pid, sizeof old);
    }
    json_free(&j);
  }
  net_resp_free(&r);
  char q[2000];
  fm_snprintf(q, sizeof q, "&addParents=%s%s%s", np, old[0] ? "&removeParents=" : "", old);
  return gd_patch(a, en->id, q, "{}", err, errcap, cancel);
}

static FmErr gd_quota(FmCloudAcct *a, u64 *used, u64 *total, char *err, size_t errcap, volatile int *cancel) {
  OaSess s;
  FmErr e = gd_open(&s, a, err, errcap, cancel);
  if (e != FM_OK) return e;
  FmNetResp r;
  e = oa_send(&s, "GET", GD_API "about?fields=storageQuota", NULL, NULL, NULL, &r);
  if (e != FM_OK || !oa_ok(&r)) e = oa_fail(&s, &r, e);
  else if (!gdrive_parse_quota((const char *)r.data, r.len, used, total)) {
    fm_strlcpy(err, "Google Drive did not say how much space is used", errcap);
    e = FM_ERR_FORMAT;
  }
  net_resp_free(&r);
  return e;
}

static FmErr gd_stream_url(FmCloudAcct *a, const FmCloudEntry *en, char *url, size_t urlcap, char *headers,
                           size_t hcap, char *err, size_t errcap, volatile int *cancel) {
  if (export_of(en->mime) >= 0) {
    fm_strlcpy(err, "Google Docs files cannot be streamed", errcap);
    return FM_ERR_UNSUPPORTED;
  }
  OaSess s;
  FmErr e = gd_open(&s, a, err, errcap, cancel);
  if (e != FM_OK) return e;
  const char *id = gd_id(en->id);
  if (!id || !en->id[0]) return bad_id(err, errcap);
  fm_snprintf(url, urlcap, GD_API "files/%s?alt=media&supportsAllDrives=true", id);
  if (fm_snprintf(headers, hcap, "Authorization: Bearer %s\r\n", s.tok.access) >= (int)hcap) {
    fm_strlcpy(err, "The sign-in token is too long to stream with", errcap);
    return FM_ERR_UNSUPPORTED;
  }
  return FM_OK;
}

const FmCloud g_cloud_gdrive = {
  "gdrive", "Google Drive", IC_CLOUD, CLOUD_OAUTH | CLOUD_UPLOAD | CLOUD_STREAM,
  gd_login, gd_list, gd_download, gd_upload, gd_mkdir, gd_remove, gd_rename, gd_move, gd_quota, gd_stream_url, NULL,
  "Sign in with Google. Needs your own free client id: console.cloud.google.com > new project > enable "
  "\"Google Drive API\" > OAuth consent screen (External, add yourself as a test user) > Credentials > "
  "OAuth client ID, type \"Desktop app\". Paste its client id and client secret here.",
};
