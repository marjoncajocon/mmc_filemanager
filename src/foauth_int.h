/* foauth_int.h -- internals shared by foauth.c, the OAuth cloud adapters
** (fcloud_gdrive.c, fcloud_dropbox.c, fcloud_onedrive.c), their helpers in
** fcloud_oauth.c and the self test (ftest_oauth.c). Not a public contract.
**
** Design decisions:
**   - The loopback sockets are a tiny blocking API over Winsock (ws2_32.dll
**     loaded at run time, so nothing new is linked and tcc builds) or POSIX
**     sockets. The self test uses the same calls to run a mock token server
**     and to "be the browser".
**   - The browser opener is a hook so the test can read the sign-in URL
**     instead of opening a window.
**   - OaSess bundles what every adapter call needs: the account, its OAuth
**     config and tokens; oa_call adds the bearer header and, on HTTP 401,
**     refreshes once and repeats the request (file bodies are re-read).
*/
#ifndef FOAUTH_INT_H
#define FOAUTH_INT_H

#include "foauth.h"
#include "fcloud.h"

/* ---- loopback sockets (foauth.c) ------------------------------------------------- */

typedef intptr_t FmOaSock;
#define OA_SOCK_NONE ((FmOaSock)-1)

/* Listens on 127.0.0.1 (or ::1 with v6); *port in: wanted (0 = any free), out: bound. */
FmOaSock oa_sock_listen(bool v6, int *port);
FmOaSock oa_sock_accept(FmOaSock s);
FmOaSock oa_sock_connect(int port);                       /* to 127.0.0.1:port */
/* Waits up to ms for one of n sockets to be readable: its index, -1 timeout, -2 error. */
int  oa_sock_wait(const FmOaSock *s, int n, int ms);
/* Bytes read (>0), 0 when closed, -1 on error or when nothing came within ms. */
int  oa_sock_recv(FmOaSock s, void *buf, int cap, int ms);
bool oa_sock_send(FmOaSock s, const void *p, int n);
void oa_sock_close(FmOaSock s);

/* ---- OAuth pieces (foauth.c) ------------------------------------------------------ */

/* Opens the sign-in page; NULL = the system browser (SDL_OpenURL). Test hook. */
extern bool (*g_oauth_browser)(const char *url, void *user);
extern void *g_oauth_browser_user;

void  oauth_b64url(const u8 *p, size_t n, char *out, size_t cap);
/* S256 code challenge of a verifier (RFC 7636 4.2). */
void  oauth_pkce_challenge(const char *verifier, char *out, size_t cap);
/* A token endpoint reply -> t (refresh kept when none comes). On failure err
** says why; FM_ERR_PASSWORD means the grant is gone (sign in again). */
FmErr oauth_parse_token(const char *json, size_t len, FmOAuthTok *t, char *err, size_t errcap);

/* ---- adapter helpers (fcloud_oauth.c) --------------------------------------------- */

typedef struct OaSess {
  FmCloudAcct *a;
  FmOAuthCfg cfg;
  FmOAuthTok tok;
  const char *service;      /* "Google Drive", for messages */
  char *err;
  size_t errcap;
  volatile int *cancel;
  FmErr said;               /* err already holds the final sentence (a failed refresh) */
} OaSess;

/* Self test only: when set ("http://127.0.0.1:<port>"), https://host/path
** requests go to <base>/host/path instead. Empty in the app. */
extern char g_oa_test_base[64];

/* Browser sign-in for an adapter's login(): checks the client id, fills a->session. */
FmErr oa_login(FmCloudAcct *a, const FmOAuthCfg *c, const char *service, char *err, size_t errcap,
               volatile int *cancel);
/* Loads the tokens of a signed-in account, refreshing a stale access token. */
FmErr oa_open(OaSess *s, FmCloudAcct *a, const FmOAuthCfg *c, const char *service, char *err, size_t errcap,
              volatile int *cancel);
/* One request; auth adds "Authorization: Bearer". FM_OK = a reply arrived (any status). */
FmErr oa_call(OaSess *s, const char *url, const FmNetReq *rq, bool auth, FmNetResp *out);
/* method + memory body (type = its Content-Type, or NULL) + extra header lines. */
FmErr oa_send(OaSess *s, const char *method, const char *url, const char *type, const char *body,
              const char *headers, FmNetResp *out);
/* The user's sentence for a failed call (transport error e, or a non-2xx reply). */
FmErr oa_fail(OaSess *s, const FmNetResp *r, FmErr e);
/* Same, without a session (tests). Returns the FmErr. */
FmErr oa_error_text(const char *service, int status, const char *body, size_t len, char *out, size_t cap);
/* GET/POST into local_path.part, renamed to local_path on 2xx. A 3xx with a
** Location (rq->no_redirect) is fetched again from there without the token. */
FmErr oa_download(OaSess *s, const char *url, const FmNetReq *rq, const char *local_path, FmNetProgress cb,
                  void *user);
bool  oa_ok(const FmNetResp *r);                         /* 2xx */

/* "quoted JSON string"; ascii escapes everything past 0x7F (HTTP headers). */
void  oa_json_quote(const char *s, char *out, size_t cap, bool ascii);

/* Chunked uploads: maps one chunk's progress onto the whole file. */
typedef struct OaProg { FmNetProgress cb; void *user; u64 base, chunk, total; } OaProg;
bool  oa_prog(void *user, u64 done, u64 total);

/* ---- adapter parsers (exposed for the self test) ----------------------------------- */

FmErr gdrive_parse_list(const char *json, size_t len, FmCloudList *out, char *next, size_t ncap);
bool  gdrive_parse_entry(const char *json, size_t len, FmCloudEntry *e);
bool  gdrive_parse_quota(const char *json, size_t len, u64 *used, u64 *total);
FmErr dropbox_parse_list(const char *json, size_t len, FmCloudList *out, char *cursor, size_t ccap, bool *more);
bool  dropbox_parse_quota(const char *json, size_t len, u64 *used, u64 *total);
void  dropbox_api_path(const char *id, char *out, size_t cap);   /* "a/b/" -> "/a/b" */
FmErr onedrive_parse_list(const char *json, size_t len, FmCloudList *out, char *next, size_t ncap);
bool  onedrive_parse_quota(const char *json, size_t len, u64 *used, u64 *total);
/* "12345-" / "0-99,200-" -> the first missing byte */
i64   onedrive_next_range(const char *json, size_t len);

#endif
