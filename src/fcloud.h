/* fcloud.h -- cloud storage: one adapter struct per service, the accounts the
** user added, and the shared helpers.
**
** An adapter is a table of functions (like FmVsrc for online videos). The
** panels, transfers and settings are shared; a new service is one FmCloud and
** one line in the registry (fcloud.c).
**
**   const FmCloud g_cloud_mysvc = {
**     "mysvc", "My Service", IC_CLOUD, CLOUD_PASSWORD | CLOUD_UPLOAD,
**     my_login, my_list, my_download, my_upload, my_mkdir, my_remove, my_rename, my_move,
**     my_quota, NULL, "Sign in with your email and password"
**   };
**
** Items are addressed by an id string: services with ids (Google Drive, MEGA,
** OneDrive) put theirs there, path-based ones (WebDAV, S3, Dropbox) put the
** path ("photos/2026/a.jpg"). "" is the root of the account. Folder ids of
** path services end with '/'.
**
** Threading: every function may block (network) and runs on a worker thread,
** never on the UI thread. An account (FmCloudAcct) is used by one call at a
** time (the caller serialises); adapters keep no other global state except
** read-only tables. `cancel` is polled; when it becomes nonzero return
** FM_ERR_CANCEL soon. `err` gets a sentence for the user on failure.
**
** Design decisions:
**   - Results are plain structs with fixed-size strings, one free per listing.
**   - Sign-in state (OAuth tokens, a MEGA session and keys) is the adapter's
**     own text in FmCloudAcct.session; fcloud.c stores accounts in the config
**     folder, encrypted for the user with DPAPI on Windows (obfuscated
**     elsewhere), and saves again whenever an adapter sets session_changed
**     (refreshed tokens).
**   - Transfers stream: downloads go to a file as they arrive, uploads read
**     the local file in pieces (fnet's net_request with body_file), so a 4 GB
**     file never sits in memory.
*/
#ifndef FCLOUD_H
#define FCLOUD_H

#include "fcore.h"
#include "ficon.h"
#include "fnet.h"

enum {
  CLOUD_OAUTH    = 1 << 0,  /* login() opens the browser (OAuth); needs a client id in the account */
  CLOUD_PASSWORD = 1 << 1,  /* login() takes user + secret (password / app password) */
  CLOUD_SERVER   = 1 << 2,  /* the account names a server (WebDAV URL, S3 endpoint) */
  CLOUD_KEYS     = 1 << 3,  /* user = access key id, secret = secret key (S3) */
  CLOUD_UPLOAD   = 1 << 4,  /* upload/mkdir/remove/rename/move work */
  CLOUD_LINKS    = 1 << 5,  /* public share links open without an account (open_link) */
  CLOUD_STREAM   = 1 << 6,  /* stream_url gives plain http(s) URLs the players can open */
};

#define CLOUD_ID_MAX 512

typedef struct FmCloudEntry {
  char id[CLOUD_ID_MAX];    /* how the service finds it (see the header) */
  char name[256];           /* display name, UTF-8 */
  u64 size;                 /* bytes, 0 for folders */
  i64 mtime;                /* unix seconds, 0 = unknown */
  bool dir;
  char mime[64];            /* "" = unknown */
  char hash[72];            /* service checksum when listed ("md5:..", "sha1:..", "etag:.."), "" = none */
} FmCloudEntry;

typedef struct FmCloudList {
  FmCloudEntry *items;
  int count, cap;
  char error[256];          /* human-readable reason when the call failed */
} FmCloudList;

/* One account the user added. Strings only, so it saves as text. */
typedef struct FmCloudAcct {
  char provider[16];        /* FmCloud.key */
  char label[64];           /* shown in the panel ("Work Drive"); defaults to user or the service */
  char user[256];           /* email / user name / access key id */
  char secret[512];         /* password / app password / secret key ("" after login when the
                            ** service gave a session instead, like MEGA and OAuth) */
  char server[512];         /* WebDAV URL; S3 "endpoint|region|bucket"; "" for fixed services */
  char client_id[256];      /* OAuth services: the user's own client id (Settings) */
  char client_secret[256];  /*   and secret, when the service's desktop flow wants one */
  char session[8192];       /* the adapter's sign-in state, opaque to everyone else (Microsoft
                            ** access + refresh tokens together run past 4 KB) */
  bool session_changed;     /* set by the adapter: save the account again */
} FmCloudAcct;

typedef struct FmCloud {
  const char *key;          /* "gdrive", "mega", "webdav", "s3", "dropbox", "onedrive" (saved) */
  const char *name;         /* "Google Drive" */
  FmIcon icon;
  int flags;                /* CLOUD_* */
  /* Signs in: OAuth opens the browser and waits; password services check the
  ** credentials. Fills acct->session (and may clear acct->secret). */
  FmErr (*login)(FmCloudAcct *a, char *err, size_t errcap, volatile int *cancel);
  /* One folder's children (not recursive). */
  FmErr (*list)(FmCloudAcct *a, const char *dir_id, FmCloudList *out, volatile int *cancel);
  /* Saves a file to local_path (written as local_path.part, renamed when complete). */
  FmErr (*download)(FmCloudAcct *a, const FmCloudEntry *e, const char *local_path, FmNetProgress cb, void *user,
                    char *err, size_t errcap, volatile int *cancel);
  /* Uploads local_path into dir_id as name (replacing a file of that name);
  ** out (may be NULL) gets the new entry. */
  FmErr (*upload)(FmCloudAcct *a, const char *dir_id, const char *local_path, const char *name, FmCloudEntry *out,
                  FmNetProgress cb, void *user, char *err, size_t errcap, volatile int *cancel);
  FmErr (*mkdir)(FmCloudAcct *a, const char *parent_id, const char *name, FmCloudEntry *out, char *err, size_t errcap,
                 volatile int *cancel);
  /* To the service's trash when it has one. */
  FmErr (*remove)(FmCloudAcct *a, const FmCloudEntry *e, char *err, size_t errcap, volatile int *cancel);
  FmErr (*rename)(FmCloudAcct *a, const FmCloudEntry *e, const char *new_name, char *err, size_t errcap,
                  volatile int *cancel);
  FmErr (*move)(FmCloudAcct *a, const FmCloudEntry *e, const char *new_parent_id, char *err, size_t errcap,
                volatile int *cancel);
  /* Storage used / total in bytes (0 total = unknown or unlimited). NULL = not offered. */
  FmErr (*quota)(FmCloudAcct *a, u64 *used, u64 *total, char *err, size_t errcap, volatile int *cancel);
  /* CLOUD_STREAM: a URL (+ request lines "Key: v\r\n") the media players can
  ** stream from, valid for a while. NULL = download first (MEGA: encrypted). */
  FmErr (*stream_url)(FmCloudAcct *a, const FmCloudEntry *e, char *url, size_t urlcap, char *headers, size_t hcap,
                      char *err, size_t errcap, volatile int *cancel);
  /* CLOUD_LINKS: opens a public share link: fills a pseudo account (session
  ** carries the link's keys) and the root entry to list from. NULL = none. */
  FmErr (*open_link)(const char *link, FmCloudAcct *a, FmCloudEntry *root, char *err, size_t errcap,
                     volatile int *cancel);
  const char *about;        /* a line for the "add account" sheet */
} FmCloud;

/* ---- registry and accounts (fcloud.c) --------------------------------------- */

int  cloud_count(void);
const FmCloud *cloud_at(int i);
const FmCloud *cloud_find(const char *key);

/* The saved accounts: loaded once, kept in memory, saved on change. Indexes
** are stable until an account is removed. Main thread only. */
int  cloud_acct_count(void);
FmCloudAcct *cloud_acct_at(int i);
int  cloud_acct_add(const FmCloudAcct *a);       /* returns its index, -1 when full */
void cloud_acct_remove(int i);
void cloud_acct_save(void);                      /* after edits / session_changed */

void cloud_list_free(FmCloudList *l);
/* Adds one entry (grows the array); returns it zeroed. */
FmCloudEntry *cloud_list_add(FmCloudList *l);

/* ---- shared helpers for adapters (fcloud.c) ------------------------------------ */

/* "Network error: ..." / "Signed out: sign in again" / "<Service> refused the
** request (HTTP 403)" from a reply; returns the FmErr to hand back. */
FmErr cloud_http_error(const char *service, const FmNetResp *r, FmErr e, char *err, size_t cap);
/* ISO 8601 / RFC 1123 time -> unix seconds, 0 when malformed. */
i64   cloud_parse_time(const char *s);

#endif
