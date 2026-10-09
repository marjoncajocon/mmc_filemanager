/* foauth.h -- OAuth 2.0 sign-in for desktop and phone apps: the browser
** flow with PKCE and a loopback redirect (RFC 8252), and token refresh.
**
** oauth_login opens the provider's sign-in page in the system browser with
** redirect_uri http://127.0.0.1:<free port> , listens on that port for the
** one redirect, checks state, exchanges the code (with the PKCE verifier) for
** tokens. Google, Microsoft and Dropbox all accept loopback redirects for
** "desktop" / "public" clients. The user registers the client id (free) and
** pastes it into Settings, like the YouTube API key.
**
** Threading: blocking; call on a worker thread. `cancel` is polled (the
** listener wakes every 250 ms); the wait gives up after 5 minutes.
*/
#ifndef FOAUTH_H
#define FOAUTH_H

#include "fcore.h"

typedef struct FmOAuthCfg {
  const char *auth_url;       /* "https://accounts.google.com/o/oauth2/v2/auth" */
  const char *token_url;      /* "https://oauth2.googleapis.com/token" */
  const char *client_id;
  const char *client_secret;  /* "" or NULL when the service does not want one */
  const char *scope;          /* space separated */
  const char *extra;          /* more auth query parameters ("access_type=offline&prompt=consent"), or NULL */
  const char *redirect_host;  /* "127.0.0.1" when NULL; "localhost" for Microsoft, whose portal only
                              ** registers http://localhost (any port matches; ::1 is listened on too) */
} FmOAuthCfg;

typedef struct FmOAuthTok {
  char access[4096];          /* Microsoft access tokens run past 2 KB */
  char refresh[2048];
  i64 expires;                /* unix seconds; 0 = unknown */
} FmOAuthTok;

/* The whole browser sign-in; on success t is filled. */
FmErr oauth_login(const FmOAuthCfg *c, FmOAuthTok *t, char *err, size_t errcap, volatile int *cancel);
/* New access token from t->refresh (refresh kept unless a new one comes). */
FmErr oauth_refresh(const FmOAuthCfg *c, FmOAuthTok *t, char *err, size_t errcap, volatile int *cancel);
/* The access token is there and good for at least another minute. */
bool  oauth_fresh(const FmOAuthTok *t);
/* Tokens <-> one line of text, for FmCloudAcct.session. */
void  oauth_save(const FmOAuthTok *t, char *out, size_t cap);
bool  oauth_load(FmOAuthTok *t, const char *s);

/* Exposed for the self test: the redirect request line parsed. */
bool  oauth_parse_redirect(const char *request_line, char *code, size_t ccap, char *state, size_t scap, char *error,
                           size_t ecap);

#endif
