/* foauth.c -- OAuth 2.0 browser sign-in with PKCE and a loopback redirect
** (foauth.h, RFC 6749 / 7636 / 8252), token refresh and the session line.
**
** Design decisions:
**   - The redirect listener binds 127.0.0.1 port 0 (the system picks a free
**     port) and, for "localhost" redirects, ::1 on the same port as well, so
**     a browser that resolves localhost to IPv6 first still lands here.
**   - redirect_uri has no path ("http://127.0.0.1:53111"): Google, Dropbox
**     and Microsoft all match a registered "http://127.0.0.1" / "http://
**     localhost" with any port only when the path is the same (empty).
**   - Every wait is a 250 ms select, so cancel is seen quickly; the whole
**     sign-in gives up after 5 minutes. A request that is not the redirect
**     (favicon, a wrong state from another tab) gets a 404 / an error page and
**     the wait goes on; a redirect carrying ?error= ends the sign-in.
**   - Winsock is loaded at run time (ws2_32.dll, like WinHTTP in fnet.c):
**     nothing new to link, tcc keeps building and Windows XP+ is fine. The
**     socket structs are spelled out here instead of including winsock2.h,
**     whose FD_ISSET would import __WSAFDIsSet.
**   - The session line is "o1 <expires> <refresh|-> <access|->": the access
**     token goes last and is dropped (expires 1, refreshed on next use) when
**     the line would not fit FmCloudAcct.session.
**   - The web build has no listening sockets: sign-in says so.
*/
#include "foauth.h"
#include "foauth_int.h"
#include "fcrypt.h"
#include "fjson.h"
#include "fnet.h"
#include "fplat.h"
#include "fsdl.h"

#define OA_WAIT_MS 250
#define OA_TIMEOUT_MS (5 * 60 * 1000)

bool (*g_oauth_browser)(const char *url, void *user);
void *g_oauth_browser_user;

/* ============================================================================ */
/* sockets                                                                      */
/* ============================================================================ */

#if defined(FM_WIN)

#include "fwin.h"

typedef UINT_PTR RawSock;
typedef struct { u16 family; u8 port[2]; u8 addr[4]; u8 zero[8]; } OaSin4;
typedef struct { u16 family; u8 port[2]; u32 flow; u8 addr[16]; u32 scope; } OaSin6;
typedef struct { unsigned int count; RawSock fd[64]; } OaFdSet;
typedef struct { long sec, usec; } OaTimeval;

typedef int (WINAPI *WsStartupFn)(WORD, void *);
typedef RawSock (WINAPI *WsSocketFn)(int, int, int);
typedef int (WINAPI *WsAddrFn)(RawSock, const void *, int);
typedef int (WINAPI *WsListenFn)(RawSock, int);
typedef RawSock (WINAPI *WsAcceptFn)(RawSock, void *, int *);
typedef int (WINAPI *WsNameFn)(RawSock, void *, int *);
typedef int (WINAPI *WsSelectFn)(int, OaFdSet *, OaFdSet *, OaFdSet *, const OaTimeval *);
typedef int (WINAPI *WsRecvFn)(RawSock, char *, int, int);
typedef int (WINAPI *WsSendFn)(RawSock, const char *, int, int);
typedef int (WINAPI *WsCloseFn)(RawSock);
typedef int (WINAPI *WsOptFn)(RawSock, int, int, const char *, int);

enum { OA_AF_INET = 2, OA_AF_INET6 = 23, OA_SOCK_STREAM = 1, OA_IPPROTO_TCP = 6, OA_SOL_SOCKET = 0xffff,
       OA_SO_EXCLUSIVEADDRUSE = ~4, OA_IPPROTO_IPV6 = 41, OA_IPV6_V6ONLY = 27 };

static struct {
  int state;
  WsStartupFn startup; WsSocketFn socket; WsAddrFn bind, connect; WsListenFn listen; WsAcceptFn accept;
  WsNameFn getsockname; WsSelectFn select; WsRecvFn recv; WsSendFn send; WsCloseFn close; WsOptFn setopt;
} ws;
static SDL_SpinLock g_ws_lock;

static bool ws_load(void) {
  SDL_AtomicLock(&g_ws_lock);
  if (!ws.state) {
    HMODULE m = LoadLibraryW(L"ws2_32.dll");
#define WS(f, n) ws.f = m ? (void *)GetProcAddress(m, n) : NULL
    WS(startup, "WSAStartup"); WS(socket, "socket"); WS(bind, "bind"); WS(connect, "connect");
    WS(listen, "listen"); WS(accept, "accept"); WS(getsockname, "getsockname"); WS(select, "select");
    WS(recv, "recv"); WS(send, "send"); WS(close, "closesocket"); WS(setopt, "setsockopt");
#undef WS
    ws.state = -1;
    if (ws.startup && ws.socket && ws.bind && ws.connect && ws.listen && ws.accept && ws.getsockname &&
        ws.select && ws.recv && ws.send && ws.close && ws.setopt) {
      static u64 wsadata[64];                      /* WSADATA is ~400 bytes */
      if (ws.startup(0x0202, wsadata) == 0) ws.state = 1;
    }
  }
  SDL_AtomicUnlock(&g_ws_lock);
  return ws.state > 0;
}

static RawSock raw(FmOaSock s) { return (RawSock)s; }

FmOaSock oa_sock_listen(bool v6, int *port) {
  if (!ws_load()) return OA_SOCK_NONE;
  RawSock s = ws.socket(v6 ? OA_AF_INET6 : OA_AF_INET, OA_SOCK_STREAM, OA_IPPROTO_TCP);
  if (s == (RawSock)~(UINT_PTR)0) return OA_SOCK_NONE;
  int one = 1;
  ws.setopt(s, OA_SOL_SOCKET, OA_SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);   /* no port stealing */
  if (v6) ws.setopt(s, OA_IPPROTO_IPV6, OA_IPV6_V6ONLY, (const char *)&one, sizeof one);
  int r;
  if (v6) {
    OaSin6 a;
    memset(&a, 0, sizeof a);
    a.family = OA_AF_INET6;
    a.port[0] = (u8)(*port >> 8); a.port[1] = (u8)*port;
    a.addr[15] = 1;
    r = ws.bind(s, &a, sizeof a);
  } else {
    OaSin4 a;
    memset(&a, 0, sizeof a);
    a.family = OA_AF_INET;
    a.port[0] = (u8)(*port >> 8); a.port[1] = (u8)*port;
    a.addr[0] = 127; a.addr[3] = 1;
    r = ws.bind(s, &a, sizeof a);
  }
  if (r != 0 || ws.listen(s, 8) != 0) { ws.close(s); return OA_SOCK_NONE; }
  OaSin6 got;
  int n = sizeof got;
  memset(&got, 0, sizeof got);
  if (ws.getsockname(s, &got, &n) != 0) { ws.close(s); return OA_SOCK_NONE; }
  *port = got.port[0] << 8 | got.port[1];
  return (FmOaSock)s;
}

FmOaSock oa_sock_accept(FmOaSock s) {
  if (!ws_load()) return OA_SOCK_NONE;
  RawSock c = ws.accept(raw(s), NULL, NULL);
  return c == (RawSock)~(UINT_PTR)0 ? OA_SOCK_NONE : (FmOaSock)c;
}

FmOaSock oa_sock_connect(int port) {
  if (!ws_load()) return OA_SOCK_NONE;
  RawSock s = ws.socket(OA_AF_INET, OA_SOCK_STREAM, OA_IPPROTO_TCP);
  if (s == (RawSock)~(UINT_PTR)0) return OA_SOCK_NONE;
  OaSin4 a;
  memset(&a, 0, sizeof a);
  a.family = OA_AF_INET;
  a.port[0] = (u8)(port >> 8); a.port[1] = (u8)port;
  a.addr[0] = 127; a.addr[3] = 1;
  if (ws.connect(s, &a, sizeof a) != 0) { ws.close(s); return OA_SOCK_NONE; }
  return (FmOaSock)s;
}

int oa_sock_wait(const FmOaSock *s, int n, int ms) {
  if (!ws_load()) return -2;
  OaFdSet set;
  set.count = 0;
  for (int i = 0; i < n && i < 64; i++) if (s[i] != OA_SOCK_NONE) set.fd[set.count++] = raw(s[i]);
  if (!set.count) return -2;
  OaTimeval tv = { ms / 1000, (ms % 1000) * 1000 };
  int r = ws.select(0, &set, NULL, NULL, &tv);
  if (r < 0) return -2;
  if (r == 0) return -1;
  for (unsigned k = 0; k < set.count; k++)          /* select leaves only the ready ones */
    for (int i = 0; i < n; i++) if (s[i] != OA_SOCK_NONE && raw(s[i]) == set.fd[k]) return i;
  return -1;
}

int oa_sock_recv(FmOaSock s, void *buf, int cap, int ms) {
  if (oa_sock_wait(&s, 1, ms) != 0) return -1;
  int r = ws.recv(raw(s), (char *)buf, cap, 0);
  return r < 0 ? -1 : r;
}

bool oa_sock_send(FmOaSock s, const void *p, int n) {
  if (!ws_load()) return false;
  const char *c = (const char *)p;
  while (n > 0) {
    int r = ws.send(raw(s), c, n, 0);
    if (r <= 0) return false;
    c += r;
    n -= r;
  }
  return true;
}

void oa_sock_close(FmOaSock s) {
  if (s != OA_SOCK_NONE && ws_load()) ws.close(raw(s));
}

#elif !defined(FM_WEB)

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

FmOaSock oa_sock_listen(bool v6, int *port) {
  int s = socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
  if (s < 0) return OA_SOCK_NONE;
  fcntl(s, F_SETFD, FD_CLOEXEC);
  int one = 1;
  int r;
  if (v6) {
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof a);
    a.sin6_family = AF_INET6;
    a.sin6_port = htons((u16)*port);
    a.sin6_addr = in6addr_loopback;
    r = bind(s, (struct sockaddr *)&a, sizeof a);
  } else {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((u16)*port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    r = bind(s, (struct sockaddr *)&a, sizeof a);
  }
  if (r != 0 || listen(s, 8) != 0) { close(s); return OA_SOCK_NONE; }
  struct sockaddr_storage got;
  socklen_t n = sizeof got;
  if (getsockname(s, (struct sockaddr *)&got, &n) != 0) { close(s); return OA_SOCK_NONE; }
  *port = ntohs(v6 ? ((struct sockaddr_in6 *)&got)->sin6_port : ((struct sockaddr_in *)&got)->sin_port);
  return (FmOaSock)s;
}

FmOaSock oa_sock_accept(FmOaSock s) {
  int c = accept((int)s, NULL, NULL);
  if (c < 0) return OA_SOCK_NONE;
  fcntl(c, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  return (FmOaSock)c;
}

FmOaSock oa_sock_connect(int port) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return OA_SOCK_NONE;
  fcntl(s, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((u16)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) { close(s); return OA_SOCK_NONE; }
  return (FmOaSock)s;
}

int oa_sock_wait(const FmOaSock *s, int n, int ms) {
  struct pollfd p[4];
  int k = 0, map[4];
  for (int i = 0; i < n && k < 4; i++) {
    if (s[i] == OA_SOCK_NONE) continue;
    p[k].fd = (int)s[i];
    p[k].events = POLLIN;
    p[k].revents = 0;
    map[k++] = i;
  }
  if (!k) return -2;
  int r = poll(p, (nfds_t)k, ms);
  if (r < 0) return errno == EINTR ? -1 : -2;
  if (r == 0) return -1;
  for (int i = 0; i < k; i++) if (p[i].revents) return map[i];
  return -1;
}

int oa_sock_recv(FmOaSock s, void *buf, int cap, int ms) {
  if (oa_sock_wait(&s, 1, ms) != 0) return -1;
  ssize_t r = recv((int)s, buf, (size_t)cap, 0);
  return r < 0 ? -1 : (int)r;
}

bool oa_sock_send(FmOaSock s, const void *p, int n) {
  const char *c = (const char *)p;
#ifdef MSG_NOSIGNAL
  int fl = MSG_NOSIGNAL;
#else
  int fl = 0;
#endif
  while (n > 0) {
    ssize_t r = send((int)s, c, (size_t)n, fl);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return false;
    c += r;
    n -= (int)r;
  }
  return true;
}

void oa_sock_close(FmOaSock s) {
  if (s != OA_SOCK_NONE) close((int)s);
}

#else  /* web: no listening sockets */

FmOaSock oa_sock_listen(bool v6, int *port) { FM_UNUSED(v6); FM_UNUSED(port); return OA_SOCK_NONE; }
FmOaSock oa_sock_accept(FmOaSock s) { FM_UNUSED(s); return OA_SOCK_NONE; }
FmOaSock oa_sock_connect(int port) { FM_UNUSED(port); return OA_SOCK_NONE; }
int  oa_sock_wait(const FmOaSock *s, int n, int ms) { FM_UNUSED(s); FM_UNUSED(n); FM_UNUSED(ms); return -2; }
int  oa_sock_recv(FmOaSock s, void *buf, int cap, int ms) {
  FM_UNUSED(s); FM_UNUSED(buf); FM_UNUSED(cap); FM_UNUSED(ms);
  return -1;
}
bool oa_sock_send(FmOaSock s, const void *p, int n) { FM_UNUSED(s); FM_UNUSED(p); FM_UNUSED(n); return false; }
void oa_sock_close(FmOaSock s) { FM_UNUSED(s); }

#endif

/* ============================================================================ */
/* PKCE, redirect, token replies                                                */
/* ============================================================================ */

void oauth_b64url(const u8 *p, size_t n, char *out, size_t cap) {
  static const char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  size_t o = 0;
  for (size_t i = 0; i < n && o + 5 < cap; i += 3) {
    u32 v = (u32)p[i] << 16 | (i + 1 < n ? (u32)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
    out[o++] = k[v >> 18 & 63];
    out[o++] = k[v >> 12 & 63];
    if (i + 1 < n) out[o++] = k[v >> 6 & 63];
    if (i + 2 < n) out[o++] = k[v & 63];
  }
  if (cap) out[o] = 0;
}

void oauth_pkce_challenge(const char *verifier, char *out, size_t cap) {
  FmSha256 h;
  u8 d[32];
  sha256_init(&h);
  sha256_update(&h, verifier, strlen(verifier));
  sha256_final(&h, d);
  oauth_b64url(d, sizeof d, out, cap);
}

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* query value [p, e) percent-decoded into out */
static void url_decode(const char *p, const char *e, char *out, size_t cap) {
  size_t o = 0;
  while (p < e && o + 1 < cap) {
    if (*p == '%' && e - p >= 3 && hexv(p[1]) >= 0 && hexv(p[2]) >= 0) {
      out[o++] = (char)(hexv(p[1]) << 4 | hexv(p[2]));
      p += 3;
    } else {
      out[o++] = *p == '+' ? ' ' : *p;
      p++;
    }
  }
  if (cap) out[o] = 0;
}

bool oauth_parse_redirect(const char *line, char *code, size_t ccap, char *state, size_t scap, char *error,
                          size_t ecap) {
  if (ccap) code[0] = 0;
  if (scap) state[0] = 0;
  if (ecap) error[0] = 0;
  if (!line || strncmp(line, "GET ", 4) != 0) return false;
  const char *t = line + 4;
  const char *te = t + strcspn(t, " \r\n");
  const char *q = memchr(t, '?', (size_t)(te - t));
  if (!q) return false;
  char desc[200];
  desc[0] = 0;
  for (const char *p = q + 1; p < te;) {
    const char *amp = memchr(p, '&', (size_t)(te - p));
    const char *pe = amp ? amp : te;
    const char *eq = memchr(p, '=', (size_t)(pe - p));
    if (eq) {
      size_t kn = (size_t)(eq - p);
      if (kn == 4 && !memcmp(p, "code", 4)) url_decode(eq + 1, pe, code, ccap);
      else if (kn == 5 && !memcmp(p, "state", 5)) url_decode(eq + 1, pe, state, scap);
      else if (kn == 5 && !memcmp(p, "error", 5)) url_decode(eq + 1, pe, error, ecap);
      else if (kn == 17 && !memcmp(p, "error_description", 17)) url_decode(eq + 1, pe, desc, sizeof desc);
    }
    p = pe + (amp ? 1 : 0);
  }
  if (ecap && error[0] && desc[0]) {
    fm_strlcat(error, ": ", ecap);
    fm_strlcat(error, desc, ecap);
  }
  return (ccap && code[0]) || (ecap && error[0]);
}

static bool token_ok(const char *s, size_t cap) {
  size_t n = strlen(s);
  if (!n || n >= cap) return false;
  for (; *s; s++) if ((u8)*s <= ' ' || (u8)*s >= 0x7F) return false;
  return true;
}

FmErr oauth_parse_token(const char *json, size_t len, FmOAuthTok *t, char *err, size_t errcap) {
  FmJson j;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(err, "The sign-in server sent a reply this app does not understand", errcap);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *r = json_root(&j);
  const char *acc = json_str(json_get(r, "access_token"), "");
  FmErr e = FM_OK;
  if (*acc) {
    const char *ref = json_str(json_get(r, "refresh_token"), "");
    if (!token_ok(acc, sizeof t->access) || (*ref && !token_ok(ref, sizeof t->refresh))) {
      fm_strlcpy(err, "The sign-in server sent a token this app cannot keep", errcap);
      e = FM_ERR_FORMAT;
    } else {
      fm_strlcpy(t->access, acc, sizeof t->access);
      if (*ref) fm_strlcpy(t->refresh, ref, sizeof t->refresh);
      double ex = json_num(json_get(r, "expires_in"), 0);
      t->expires = ex > 0 && ex < 1e9 ? plat_time_unix() + (i64)ex : 0;
    }
  } else {
    const char *code = json_str(json_get(r, "error"), "");
    const FmJsonNode *eo = json_get(r, "error");     /* some servers nest it: {"error":{"message":..}} */
    const char *desc = json_str(json_get(r, "error_description"), json_str(json_get(eo, "message"), ""));
    if (!strcmp(code, "invalid_grant")) {
      fm_strlcpy(err, "Signed out: sign in again", errcap);
      e = FM_ERR_PASSWORD;
    } else if (!strcmp(code, "invalid_client") || !strcmp(code, "unauthorized_client")) {
      fm_snprintf(err, errcap, "The client id was refused (%s): check it in Settings", *desc ? desc : code);
      e = FM_ERR_ACCESS;
    } else {
      fm_snprintf(err, errcap, "Sign-in failed: %s", *desc ? desc : *code ? code : "no token in the reply");
      e = FM_ERR_ACCESS;
    }
  }
  json_free(&j);
  return e;
}

/* ============================================================================ */
/* token endpoint                                                               */
/* ============================================================================ */

static void form_add(char *out, size_t cap, const char *key, const char *val) {
  if (!val || !*val) return;
  size_t n = strlen(val) * 3 + 1;
  char *enc = (char *)fm_alloc(n);
  net_urlencode(val, enc, n);
  if (*out) fm_strlcat(out, "&", cap);
  fm_strlcat(out, key, cap);
  fm_strlcat(out, "=", cap);
  fm_strlcat(out, enc, cap);
  fm_free(enc);
}

static FmErr token_post(const FmOAuthCfg *c, const char *body, FmOAuthTok *t, char *err, size_t errcap,
                        volatile int *cancel) {
  FmNetResp r;
  FmErr e = net_post(c->token_url, "Content-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n",
                     body, strlen(body), 256 * 1024, &r, cancel);
  if (e != FM_OK) {
    if (e == FM_ERR_CANCEL) fm_strlcpy(err, "Cancelled", errcap);
    else fm_snprintf(err, errcap, "Network error: %s", r.error[0] ? r.error : fm_err_str(e));
    net_resp_free(&r);
    return e;
  }
  if (r.status >= 500 || (r.status != 200 && (!r.data || r.data[0] != '{'))) {
    fm_snprintf(err, errcap, "The sign-in server answered HTTP %d", r.status);
    net_resp_free(&r);
    return FM_ERR_IO;
  }
  e = oauth_parse_token((const char *)r.data, r.len, t, err, errcap);
  net_resp_free(&r);
  return e;
}

FmErr oauth_refresh(const FmOAuthCfg *c, FmOAuthTok *t, char *err, size_t errcap, volatile int *cancel) {
  if (!t->refresh[0]) {
    fm_strlcpy(err, "Signed out: sign in again", errcap);
    return FM_ERR_PASSWORD;
  }
  char body[4096];
  body[0] = 0;
  form_add(body, sizeof body, "grant_type", "refresh_token");
  form_add(body, sizeof body, "refresh_token", t->refresh);
  form_add(body, sizeof body, "client_id", c->client_id);
  form_add(body, sizeof body, "client_secret", c->client_secret);
  FmOAuthTok n = *t;
  FmErr e = token_post(c, body, &n, err, errcap, cancel);
  if (e == FM_OK) *t = n;
  wipe(&n, sizeof n);
  wipe(body, sizeof body);
  return e;
}

bool oauth_fresh(const FmOAuthTok *t) {
  if (!t->access[0]) return false;
  return t->expires == 0 || t->expires > plat_time_unix() + 60;
}

void oauth_save(const FmOAuthTok *t, char *out, size_t cap) {
  const char *rf = t->refresh[0] ? t->refresh : "-";
  const char *ac = t->access[0] ? t->access : "-";
  int n = fm_snprintf(out, cap, "o1 %lld %s %s", (long long)t->expires, rf, ac);
  if (n < 0 || (size_t)n >= cap) fm_snprintf(out, cap, "o1 1 %s -", rf);   /* refresh on next use */
}

bool oauth_load(FmOAuthTok *t, const char *s) {
  memset(t, 0, sizeof *t);
  if (!s || strncmp(s, "o1 ", 3) != 0) return false;
  s += 3;
  i64 ex = 0;                                       /* no strtoll: old msvcrt (tcc) lacks it */
  const char *d = s;
  while (*d >= '0' && *d <= '9' && d - s < 18) ex = ex * 10 + (*d++ - '0');
  if (d == s || *d != ' ') return false;
  s = d + 1;
  size_t n = strcspn(s, " ");
  if (!n || n >= sizeof t->refresh || s[n] != ' ') return false;
  memcpy(t->refresh, s, n);
  t->refresh[n] = 0;
  s += n + 1;
  n = strcspn(s, " \r\n");
  if (!n || n >= sizeof t->access) { memset(t, 0, sizeof *t); return false; }
  memcpy(t->access, s, n);
  t->access[n] = 0;
  if (!strcmp(t->refresh, "-")) t->refresh[0] = 0;
  if (!strcmp(t->access, "-")) t->access[0] = 0;
  t->expires = (i64)ex;
  if (!t->refresh[0] && !t->access[0]) return false;
  return true;
}

/* ============================================================================ */
/* the browser flow                                                             */
/* ============================================================================ */

static void http_reply(FmOaSock c, int status, const char *title, const char *msg) {
  char body[1024], head[256];
  fm_snprintf(body, sizeof body,
              "<!doctype html><html><head><meta charset=\"utf-8\"><title>%s</title></head>"
              "<body style=\"font-family:sans-serif;text-align:center;margin-top:20vh\">"
              "<h2>%s</h2><p>%s</p></body></html>", title, title, msg);
  fm_snprintf(head, sizeof head,
              "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %d\r\n"
              "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
              status, status == 200 ? "OK" : status == 404 ? "Not Found" : "Bad Request", (int)strlen(body));
  oa_sock_send(c, head, (int)strlen(head));
  oa_sock_send(c, body, (int)strlen(body));
}

/* text for the page: no markup from the provider's error string */
static void html_plain(const char *s, char *out, size_t cap) {
  size_t o = 0;
  for (; *s && o + 1 < cap; s++) out[o++] = (*s == '<' || *s == '>' || *s == '&' || *s == '"') ? ' ' : *s;
  out[o] = 0;
}

static bool open_browser(const char *url) {
  if (g_oauth_browser) return g_oauth_browser(url, g_oauth_browser_user);
  return SDL_OpenURL(url) == 0;
}

FmErr oauth_login(const FmOAuthCfg *c, FmOAuthTok *t, char *err, size_t errcap, volatile int *cancel) {
  if (!c->client_id || !*c->client_id) {
    fm_strlcpy(err, "Add your own OAuth client id in Settings first", errcap);
    return FM_ERR_ACCESS;
  }
  const char *host = c->redirect_host && *c->redirect_host ? c->redirect_host : "127.0.0.1";
  int port = 0;
  FmOaSock ls[2];
  ls[0] = oa_sock_listen(false, &port);
  ls[1] = OA_SOCK_NONE;
  if (ls[0] == OA_SOCK_NONE) {
#ifdef FM_WEB
    fm_strlcpy(err, "Browser sign-in is not available in the web version", errcap);
    return FM_ERR_UNSUPPORTED;
#else
    fm_strlcpy(err, "Cannot listen for the sign-in reply on this device", errcap);
    return FM_ERR_IO;
#endif
  }
  if (!strcmp(host, "localhost")) {
    int p6 = port;
    ls[1] = oa_sock_listen(true, &p6);              /* best effort: no IPv6 is fine */
  }

  u8 rnd[48];
  char verifier[80], challenge[64], state[32], redirect[64];
  plat_random(rnd, sizeof rnd);
  oauth_b64url(rnd, 32, verifier, sizeof verifier);          /* 43 chars */
  oauth_b64url(rnd + 32, 16, state, sizeof state);
  oauth_pkce_challenge(verifier, challenge, sizeof challenge);
  fm_snprintf(redirect, sizeof redirect, "http://%s:%d", host, port);

  size_t ucap = 8192;
  char *url = (char *)fm_alloc(ucap);
  fm_strlcpy(url, c->auth_url, ucap);
  char *params = (char *)fm_alloc(ucap);
  params[0] = 0;
  form_add(params, ucap, "response_type", "code");
  form_add(params, ucap, "client_id", c->client_id);
  form_add(params, ucap, "redirect_uri", redirect);
  form_add(params, ucap, "scope", c->scope);
  form_add(params, ucap, "state", state);
  form_add(params, ucap, "code_challenge", challenge);
  form_add(params, ucap, "code_challenge_method", "S256");
  if (c->extra && *c->extra) {
    fm_strlcat(params, "&", ucap);
    fm_strlcat(params, c->extra, ucap);
  }
  fm_strlcat(url, strchr(c->auth_url, '?') ? "&" : "?", ucap);
  fm_strlcat(url, params, ucap);
  fm_free(params);

  FmErr e = FM_ERR_IO;
  char code[1024];
  code[0] = 0;
  if (!open_browser(url)) {
    fm_strlcpy(err, "No web browser found to sign in", errcap);
    e = FM_ERR_UNSUPPORTED;
    goto done;
  }
  u64 deadline = plat_now_ms() + OA_TIMEOUT_MS;
  for (;;) {
    if (cancel && *cancel) { fm_strlcpy(err, "Cancelled", errcap); e = FM_ERR_CANCEL; goto done; }
    if (plat_now_ms() > deadline) {
      fm_strlcpy(err, "Sign-in timed out: nothing came back from the browser in 5 minutes", errcap);
      e = FM_ERR_CANCEL;
      goto done;
    }
    int w = oa_sock_wait(ls, 2, OA_WAIT_MS);
    if (w == -1) continue;
    if (w < 0) { fm_strlcpy(err, "The sign-in listener failed", errcap); goto done; }
    FmOaSock cs = oa_sock_accept(ls[w]);
    if (cs == OA_SOCK_NONE) continue;
    char req[8192];
    int n = 0;
    u64 until = plat_now_ms() + 5000;
    while (n < (int)sizeof req - 1 && plat_now_ms() < until) {
      if (cancel && *cancel) break;
      int r = oa_sock_recv(cs, req + n, (int)sizeof req - 1 - n, OA_WAIT_MS);
      if (r == 0) break;
      if (r < 0) continue;
      n += r;
      req[n] = 0;
      if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) break;
    }
    req[n] = 0;
    char st[64], perr[256];
    if (!oauth_parse_redirect(req, code, sizeof code, st, sizeof st, perr, sizeof perr)) {
      http_reply(cs, 404, "Not found", "This address only takes the sign-in reply.");
      oa_sock_close(cs);
      continue;
    }
    if (strcmp(st, state) != 0) {                      /* not ours: another tab, a stale page, an attacker */
      code[0] = 0;
      http_reply(cs, 400, "Sign-in not recognised", "This reply belongs to another sign-in. Start again from mmcfm.");
      oa_sock_close(cs);
      continue;
    }
    if (perr[0]) {
      char plain[256];
      html_plain(perr, plain, sizeof plain);
      http_reply(cs, 200, "Sign-in cancelled", plain);
      oa_sock_close(cs);
      if (!strncmp(perr, "access_denied", 13)) {
        fm_strlcpy(err, "Sign-in was declined in the browser", errcap);
        e = FM_ERR_CANCEL;
      } else {
        fm_snprintf(err, errcap, "Sign-in failed: %s", perr);
        e = FM_ERR_ACCESS;
      }
      goto done;
    }
    http_reply(cs, 200, "Signed in", "You can close this window and go back to mmcfm.");
    oa_sock_close(cs);
    break;
  }

  {
    char *body = (char *)fm_alloc(8192);
    body[0] = 0;
    form_add(body, 8192, "grant_type", "authorization_code");
    form_add(body, 8192, "code", code);
    form_add(body, 8192, "redirect_uri", redirect);
    form_add(body, 8192, "client_id", c->client_id);
    form_add(body, 8192, "client_secret", c->client_secret);
    form_add(body, 8192, "code_verifier", verifier);
    FmOAuthTok n;
    memset(&n, 0, sizeof n);
    e = token_post(c, body, &n, err, errcap, cancel);
    if (e == FM_OK) *t = n;
    wipe(&n, sizeof n);
    wipe(body, 8192);
    fm_free(body);
  }
done:
  oa_sock_close(ls[0]);
  oa_sock_close(ls[1]);
  wipe(verifier, sizeof verifier);
  wipe(code, sizeof code);
  fm_free(url);
  return e;
}
