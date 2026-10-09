/* fnet.c -- HTTPS GET (see fnet.h).
**
** Design decisions:
**   - Nothing is linked. WinHTTP (Windows) and libcurl (POSIX) are opened at
**     run time with our own minimal declarations, so tcc builds, old systems
**     and machines without libcurl still start; net_available() says no.
**   - On Windows the session asks for TLS 1.2 and 1.3 explicitly (Windows 7
**     only enables TLS 1.2 when asked) and for gzip decoding where supported.
**   - One request per call, no connection pool: requests are rare (a search,
**     a page of thumbnails) and this keeps memory at the response size.
*/
#include "fnet.h"
#include "fplat.h"
#include "fsdl.h"

#define NET_UA "mmcfm/0.1"
#define NET_UA_W L"mmcfm/0.1"

void net_urlencode(const char *s, char *out, size_t cap) {
  static const char hx[] = "0123456789ABCDEF";
  size_t o = 0;
  for (; *s && o + 4 < cap; s++) {
    u8 c = (u8)*s;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.~", c)) {
      out[o++] = (char)c;
    } else {
      out[o++] = '%';
      out[o++] = hx[c >> 4];
      out[o++] = hx[c & 15];
    }
  }
  out[o] = 0;
}

void net_resp_free(FmNetResp *r) {
  fm_free(r->data);
  r->data = NULL;
  r->len = 0;
  fm_free(r->headers);
  r->headers = NULL;
}

bool net_resp_header(const FmNetResp *r, const char *name, char *out, size_t cap) {
  if (cap) out[0] = 0;
  if (!r->headers) return false;
  size_t nl = strlen(name);
  for (const char *p = r->headers; *p;) {
    const char *e = p + strcspn(p, "\r\n");
    if ((size_t)(e - p) > nl && p[nl] == ':' && !fm_strnicmp(p, name, nl)) {
      const char *v = p + nl + 1;
      while (v < e && (*v == ' ' || *v == '\t')) v++;
      size_t n = FM_MIN((size_t)(e - v), cap ? cap - 1 : 0);
      memcpy(out, v, n);
      if (cap) out[n] = 0;
      return true;
    }
    p = e;
    while (*p == '\r' || *p == '\n') p++;
  }
  return false;
}

#if !defined(FM_WEB)
/* Growable body buffer shared by the backends. */
typedef struct Body {
  u8 *data;
  size_t len, cap, max;
  FILE *f;
  bool over;
  /* streaming: chunks go to `sink` instead; `head` runs once before them */
  FmNetHead head;
  FmNetData sink;
  void *su;
  /* POST: the request body (NULL = GET) */
  const void *post;
  size_t post_len;
  /* net_request: method, a file slice as the body, keep the reply headers */
  const char *method;
  FILE *upf;
  i64 up_len, up_off;
  char up_path[FM_PATH_MAX];  /* Android: Java streams the file itself */
  bool keep_headers, no_redirect;
} Body;

static bool body_put(Body *b, const void *p, size_t n) {
  if (b->sink) return b->sink(b->su, (const u8 *)p, n);
  if (b->f) return fwrite(p, 1, n, b->f) == n;
  if (b->len + n > b->max) { b->over = true; return false; }
  if (b->len + n + 1 > b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 16384;
    while (nc < b->len + n + 1) nc *= 2;
    if (nc > b->max + 1) nc = b->max + 1;
    b->data = (u8 *)fm_realloc(b->data, nc);
    b->cap = nc;
  }
  memcpy(b->data + b->len, p, n);
  b->len += n;
  b->data[b->len] = 0;
  return true;
}
#else
typedef struct Body {
  u8 *data; size_t len, cap, max; FILE *f; bool over; FmNetHead head; FmNetData sink; void *su;
  const void *post; size_t post_len;
  const char *method; FILE *upf; i64 up_len, up_off; char up_path[FM_PATH_MAX]; bool keep_headers, no_redirect;
} Body;
#endif

/* ============================================================================ */
#if defined(FM_WIN)

#include "fwin.h"

typedef void *HINET;
typedef HINET (WINAPI *OpenFn)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef HINET (WINAPI *ConnectFn)(HINET, LPCWSTR, WORD, DWORD);
typedef HINET (WINAPI *OpenReqFn)(HINET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD);
typedef BOOL (WINAPI *SendFn)(HINET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
typedef BOOL (WINAPI *RecvFn)(HINET, LPVOID);
typedef BOOL (WINAPI *QueryHdrFn)(HINET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
typedef BOOL (WINAPI *AvailFn)(HINET, LPDWORD);
typedef BOOL (WINAPI *ReadFn)(HINET, LPVOID, DWORD, LPDWORD);
typedef BOOL (WINAPI *CloseFn)(HINET);
typedef BOOL (WINAPI *TimeoutsFn)(HINET, int, int, int, int);
typedef BOOL (WINAPI *SetOptFn)(HINET, DWORD, LPVOID, DWORD);
typedef BOOL (WINAPI *AddHdrFn)(HINET, LPCWSTR, DWORD, DWORD);
typedef BOOL (WINAPI *WriteFn)(HINET, LPCVOID, DWORD, LPDWORD);

enum {
  WH_ACCESS_DEFAULT = 0, WH_ACCESS_AUTOMATIC = 4,       /* automatic proxy: Windows 8.1+ */
  WH_FLAG_SECURE = 0x00800000,
  WH_QUERY_STATUS = 19, WH_QUERY_CTYPE = 1, WH_QUERY_CLEN = 5, WH_QUERY_NUMBER = 0x20000000,
  WH_OPT_SECURE_PROTOCOLS = 84, WH_TLS11 = 0x200, WH_TLS12 = 0x800, WH_TLS13 = 0x2000,
  WH_OPT_DECOMPRESSION = 118, WH_DECOMP_ALL = 3, WH_OPT_REDIRECT_POLICY = 88, WH_REDIRECT_ALWAYS_SAFE = 1,
  WH_ADDREQ_ADD = 0x20000000, WH_QUERY_CUSTOM = 65535, WH_QUERY_RAW_CRLF = 22, WH_REDIRECT_NEVER = 0,
};

static struct {
  int state;
  OpenFn open; ConnectFn connect; OpenReqFn openreq; SendFn send; RecvFn recv;
  QueryHdrFn qhdr; AvailFn avail; ReadFn read; CloseFn close; TimeoutsFn timeouts;
  SetOptFn setopt; AddHdrFn addhdr; WriteFn write;
} wh;
static SDL_SpinLock g_wh_lock;

static bool wh_load(void) {
  SDL_AtomicLock(&g_wh_lock);
  if (!wh.state) {
    HMODULE m = LoadLibraryW(L"winhttp.dll");
#define WH(f, n) wh.f = m ? (void *)GetProcAddress(m, n) : NULL
    WH(open, "WinHttpOpen"); WH(connect, "WinHttpConnect"); WH(openreq, "WinHttpOpenRequest");
    WH(send, "WinHttpSendRequest"); WH(recv, "WinHttpReceiveResponse"); WH(qhdr, "WinHttpQueryHeaders");
    WH(avail, "WinHttpQueryDataAvailable"); WH(read, "WinHttpReadData"); WH(close, "WinHttpCloseHandle");
    WH(timeouts, "WinHttpSetTimeouts"); WH(setopt, "WinHttpSetOption"); WH(addhdr, "WinHttpAddRequestHeaders");
    WH(write, "WinHttpWriteData");
#undef WH
    wh.state = (wh.open && wh.connect && wh.openreq && wh.send && wh.recv && wh.qhdr && wh.avail && wh.read &&
                wh.close && wh.timeouts && wh.setopt && wh.addhdr && wh.write) ? 1 : -1;
  }
  SDL_AtomicUnlock(&g_wh_lock);
  return wh.state > 0;
}

bool net_available(void) { return wh_load(); }
const char *net_backend(void) { return wh_load() ? "WinHTTP" : "WinHTTP is missing"; }

static wchar_t *to_w(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = (wchar_t *)fm_alloc((size_t)(n > 0 ? n : 1) * sizeof(wchar_t));
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  else w[0] = 0;
  return w;
}

/* https://host[:port]/path?query -> parts; false for anything else */
static bool split_url(const char *url, bool *tls, char *host, size_t hcap, int *port, const char **path) {
  const char *p;
  if (!fm_strnicmp(url, "https://", 8)) { *tls = true; p = url + 8; *port = 443; }
  else if (!fm_strnicmp(url, "http://", 7)) { *tls = false; p = url + 7; *port = 80; }
  else return false;
  size_t n = strcspn(p, ":/?#");
  if (!n || n >= hcap) return false;
  memcpy(host, p, n);
  host[n] = 0;
  p += n;
  if (*p == ':') { *port = atoi(p + 1); p += strcspn(p, "/?#"); }
  *path = *p ? p : "/";
  return *port > 0 && *port < 65536;
}

static FmErr wh_request(const char *url, const char *headers, Body *b, FmNetProgress cb, void *user,
                        FmNetResp *out, volatile int *cancel) {
  memset(out, 0, sizeof *out);
  if (!wh_load()) { fm_strlcpy(out->error, "WinHTTP is not available", sizeof out->error); return FM_ERR_UNSUPPORTED; }
  bool tls;
  char host[256];
  int port;
  const char *path;
  if (!split_url(url, &tls, host, sizeof host, &port, &path)) {
    fm_strlcpy(out->error, "not an http(s) URL", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  FmErr err = FM_ERR_IO;
  HINET ses = wh.open(NET_UA_W, WH_ACCESS_AUTOMATIC, NULL, NULL, 0);
  if (!ses) ses = wh.open(NET_UA_W, WH_ACCESS_DEFAULT, NULL, NULL, 0);   /* before Windows 8.1 */
  HINET con = NULL, req = NULL;
  wchar_t *whost = to_w(host), *wpath = to_w(path), *whdr = headers && *headers ? to_w(headers) : NULL;
  if (!ses) goto done;
  DWORD protos = WH_TLS12 | WH_TLS13;
  if (!wh.setopt(ses, WH_OPT_SECURE_PROTOCOLS, &protos, sizeof protos)) {
    protos = WH_TLS11 | WH_TLS12;                                 /* systems that refuse TLS 1.3 */
    wh.setopt(ses, WH_OPT_SECURE_PROTOCOLS, &protos, sizeof protos);
  }
  DWORD decomp = WH_DECOMP_ALL;
  wh.setopt(ses, WH_OPT_DECOMPRESSION, &decomp, sizeof decomp);   /* Windows 8.1+, ignored before */
  wh.timeouts(ses, 10000, 10000, 15000, 30000);
  con = wh.connect(ses, whost, (WORD)port, 0);
  if (!con) goto done;
  {
    wchar_t *wm = b->method ? to_w(b->method) : NULL;
    req = wh.openreq(con, wm ? wm : b->post ? L"POST" : L"GET", wpath, NULL, NULL, NULL, tls ? WH_FLAG_SECURE : 0);
    fm_free(wm);
  }
  if (!req) goto done;
  DWORD redir = b->no_redirect ? WH_REDIRECT_NEVER : WH_REDIRECT_ALWAYS_SAFE;
  wh.setopt(req, WH_OPT_REDIRECT_POLICY, &redir, sizeof redir);
  if (whdr) wh.addhdr(req, whdr, (DWORD)-1, WH_ADDREQ_ADD);
  if (cancel && *cancel) { err = FM_ERR_CANCEL; goto done; }
  if (b->upf) {
    /* a file body in pieces; past 4 GB the length goes in a header of its own */
    if (b->up_len > 0xFFFFFFFFll) {
      wchar_t cl[64];
      _snwprintf(cl, 64, L"Content-Length: %lld\r\n", (long long)b->up_len);
      wh.addhdr(req, cl, (DWORD)-1, WH_ADDREQ_ADD);
    }
    DWORD tot = b->up_len > 0xFFFFFFFFll ? 0 : (DWORD)b->up_len;
    if (!wh.send(req, NULL, 0, NULL, 0, tot, 0)) {
      fm_snprintf(out->error, sizeof out->error, "connection failed (error %lu)", (unsigned long)GetLastError());
      goto done;
    }
    static const size_t kPiece = 256 * 1024;
    u8 *piece = (u8 *)fm_alloc(kPiece);
    i64 sent = 0;
    bool ok = true;
    while (sent < b->up_len) {
      if (cancel && *cancel) { err = FM_ERR_CANCEL; ok = false; break; }
      size_t want = (size_t)FM_MIN((i64)kPiece, b->up_len - sent);
      size_t got = fread(piece, 1, want, b->upf);
      if (got != want) { fm_strlcpy(out->error, "cannot read the file", sizeof out->error); err = FM_ERR_IO; ok = false; break; }
      DWORD w = 0;
      if (!wh.write(req, piece, (DWORD)got, &w) || w != got) {
        fm_snprintf(out->error, sizeof out->error, "upload failed (error %lu)", (unsigned long)GetLastError());
        ok = false;
        break;
      }
      sent += (i64)got;
      if (cb && !cb(user, (u64)sent, (u64)b->up_len)) { err = FM_ERR_CANCEL; ok = false; break; }
    }
    fm_free(piece);
    if (!ok) goto done;
    if (!wh.recv(req, NULL)) {
      fm_snprintf(out->error, sizeof out->error, "no reply (error %lu)", (unsigned long)GetLastError());
      goto done;
    }
  } else if (!wh.send(req, NULL, 0, (void *)b->post, (DWORD)b->post_len, (DWORD)b->post_len, 0) ||
             !wh.recv(req, NULL)) {
    fm_snprintf(out->error, sizeof out->error, "connection failed (error %lu)", (unsigned long)GetLastError());
    goto done;
  }
  if (b->keep_headers) {
    DWORD hs = 0;
    wh.qhdr(req, WH_QUERY_RAW_CRLF, NULL, NULL, &hs, NULL);
    if (hs > 0 && hs < (1u << 20)) {
      wchar_t *hw = (wchar_t *)fm_alloc(hs + sizeof(wchar_t));
      if (wh.qhdr(req, WH_QUERY_RAW_CRLF, NULL, hw, &hs, NULL)) {
        hw[hs / sizeof(wchar_t)] = 0;
        int n = WideCharToMultiByte(CP_UTF8, 0, hw, -1, NULL, 0, NULL, NULL);
        if (n > 0) {
          out->headers = (char *)fm_alloc((size_t)n);
          WideCharToMultiByte(CP_UTF8, 0, hw, -1, out->headers, n, NULL, NULL);
        }
      }
      fm_free(hw);
    }
  }
  DWORD status = 0, sz = sizeof status;
  wh.qhdr(req, WH_QUERY_STATUS | WH_QUERY_NUMBER, NULL, &status, &sz, NULL);
  out->status = (int)status;
  wchar_t wt[96];
  sz = sizeof wt;
  if (wh.qhdr(req, WH_QUERY_CTYPE, NULL, wt, &sz, NULL))
    WideCharToMultiByte(CP_UTF8, 0, wt, -1, out->type, (int)sizeof out->type, NULL, NULL);
  DWORD clen = 0;
  sz = sizeof clen;
  u64 total = wh.qhdr(req, WH_QUERY_CLEN | WH_QUERY_NUMBER, NULL, &clen, &sz, NULL) ? clen : 0;
  out->length = total ? (i64)total : -1;
  {
    wchar_t hv[128];
    char a[128];
    sz = sizeof hv;
    if (wh.qhdr(req, WH_QUERY_CUSTOM, L"Accept-Ranges", hv, &sz, NULL)) {
      WideCharToMultiByte(CP_UTF8, 0, hv, -1, a, (int)sizeof a, NULL, NULL);
      out->ranges = fm_strnicmp(a, "bytes", 5) == 0;
    }
    if (status == 206) out->ranges = true;
    sz = sizeof hv;
    if (wh.qhdr(req, WH_QUERY_CUSTOM, L"Content-Range", hv, &sz, NULL)) {
      WideCharToMultiByte(CP_UTF8, 0, hv, -1, a, (int)sizeof a, NULL, NULL);
      const char *sl = strchr(a, '/');
      if (sl && sl[1] != '*') out->total = (i64)_atoi64(sl + 1);
    }
    sz = sizeof hv;
    if (wh.qhdr(req, WH_QUERY_CUSTOM, L"icy-metaint", hv, &sz, NULL)) {
      WideCharToMultiByte(CP_UTF8, 0, hv, -1, a, (int)sizeof a, NULL, NULL);
      out->icy_metaint = atoi(a);
    }
    sz = sizeof hv;
    if (wh.qhdr(req, WH_QUERY_CUSTOM, L"icy-name", hv, &sz, NULL))
      WideCharToMultiByte(CP_UTF8, 0, hv, -1, out->icy_name, (int)sizeof out->icy_name, NULL, NULL);
  }
  if (b->head) b->head(b->su, out);
  u8 chunk[16384];
  u64 done_bytes = 0;
  for (;;) {
    if (cancel && *cancel) { err = FM_ERR_CANCEL; goto done; }
    DWORD avail = 0, got = 0;
    if (!wh.avail(req, &avail)) { fm_strlcpy(out->error, "connection lost", sizeof out->error); goto done; }
    if (!avail) break;
    if (avail > sizeof chunk) avail = sizeof chunk;
    if (!wh.read(req, chunk, avail, &got)) { fm_strlcpy(out->error, "read failed", sizeof out->error); goto done; }
    if (!got) break;
    if (!body_put(b, chunk, got)) {
      fm_strlcpy(out->error, b->over ? "response too large" : "write failed", sizeof out->error);
      err = b->over ? FM_ERR_FULL : FM_ERR_IO;
      goto done;
    }
    done_bytes += got;
    if (cb && !cb(user, done_bytes, total)) { err = FM_ERR_CANCEL; goto done; }
  }
  err = FM_OK;
done:
  if (req) wh.close(req);
  if (con) wh.close(con);
  if (ses) wh.close(ses);
  fm_free(whost);
  fm_free(wpath);
  fm_free(whdr);
  return err;
}

#define REQUEST wh_request

/* ============================================================================ */
#elif !defined(FM_ANDROID) && !defined(FM_WEB)

#include <dlfcn.h>

typedef void CURL;
struct curl_slist;
typedef size_t (*CurlWriteFn)(char *, size_t, size_t, void *);
typedef int (*CurlXferFn)(void *, i64, i64, i64, i64);

enum {
  CURLOPT_URL_ = 10002, CURLOPT_WRITEFUNCTION_ = 20011, CURLOPT_WRITEDATA_ = 10001, CURLOPT_FOLLOWLOCATION_ = 52,
  CURLOPT_HTTPHEADER_ = 10023, CURLOPT_USERAGENT_ = 10018, CURLOPT_XFERINFOFUNCTION_ = 20219,
  CURLOPT_XFERINFODATA_ = 10057, CURLOPT_NOPROGRESS_ = 43, CURLOPT_CONNECTTIMEOUT_ = 78,
  CURLOPT_LOW_SPEED_TIME_ = 20, CURLOPT_LOW_SPEED_LIMIT_ = 19, CURLOPT_ACCEPT_ENCODING_ = 10102,
  CURLOPT_MAXREDIRS_ = 68, CURLOPT_NOSIGNAL_ = 99, CURLOPT_HEADERFUNCTION_ = 20079,
  CURLOPT_HEADERDATA_ = 10029, CURLOPT_HTTP09_ALLOWED_ = 285, CURLOPT_POSTFIELDS_ = 10015,
  CURLOPT_POSTFIELDSIZE_LARGE_ = 30120, CURLOPT_CUSTOMREQUEST_ = 10036, CURLOPT_UPLOAD_ = 46,
  CURLOPT_READFUNCTION_ = 20012, CURLOPT_READDATA_ = 10009, CURLOPT_INFILESIZE_LARGE_ = 30115,
  CURLOPT_NOBODY_ = 44, CURLOPT_POST_ = 47,
  CURLINFO_RESPONSE_CODE_ = 0x200002, CURLINFO_CONTENT_TYPE_ = 0x100012,
};

static struct {
  int state;
  CURL *(*init)(void);
  int (*setopt)(CURL *, int, ...);
  int (*perform)(CURL *);
  int (*getinfo)(CURL *, int, ...);
  void (*cleanup)(CURL *);
  struct curl_slist *(*slist_append)(struct curl_slist *, const char *);
  void (*slist_free)(struct curl_slist *);
  const char *(*version)(void);
  const char *(*strerror)(int);
  int (*global_init)(long);
} cu;
static SDL_SpinLock g_cu_lock;

static bool cu_load(void) {
  SDL_AtomicLock(&g_cu_lock);
  if (!cu.state) {
    static const char *const kNames[] = {
#ifdef FM_MACOS
      "libcurl.4.dylib", "libcurl.dylib", "/usr/lib/libcurl.4.dylib",
#else
      "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so",
#endif
    };
    void *h = NULL;
    for (int i = 0; i < FM_COUNT(kNames) && !h; i++) h = dlopen(kNames[i], RTLD_NOW | RTLD_LOCAL);
#define CU(f, n) *(void **)&cu.f = h ? dlsym(h, n) : NULL
    CU(init, "curl_easy_init"); CU(setopt, "curl_easy_setopt"); CU(perform, "curl_easy_perform");
    CU(getinfo, "curl_easy_getinfo"); CU(cleanup, "curl_easy_cleanup"); CU(slist_append, "curl_slist_append");
    CU(slist_free, "curl_slist_free_all"); CU(version, "curl_version"); CU(strerror, "curl_easy_strerror");
    CU(global_init, "curl_global_init");
#undef CU
    cu.state = (cu.init && cu.setopt && cu.perform && cu.getinfo && cu.cleanup && cu.slist_append &&
                cu.slist_free && cu.global_init) ? 1 : -1;
    if (cu.state > 0) cu.global_init(3);                  /* CURL_GLOBAL_ALL, once */
  }
  SDL_AtomicUnlock(&g_cu_lock);
  return cu.state > 0;
}

bool net_available(void) { return cu_load(); }
const char *net_backend(void) {
  if (!cu_load()) return "libcurl is not installed";
  return cu.version ? cu.version() : "libcurl";
}

typedef struct CuCtx {
  Body *b; FmNetProgress cb; void *user; volatile int *cancel; bool stop;
  CURL *h; FmNetResp *out; bool head_sent;
} CuCtx;

/* one response header line; a new status line (redirects) starts over */
/* net_request: the raw header lines of the final reply */
static void cu_keep(CuCtx *c, const char *p, size_t len, bool status_line) {
  FmNetResp *o = c->out;
  if (status_line) { fm_free(o->headers); o->headers = NULL; return; }   /* redirects start over */
  if (len < 3) return;                                                  /* the blank line */
  size_t have = o->headers ? strlen(o->headers) : 0;
  if (have + len > (1u << 20)) return;
  o->headers = (char *)fm_realloc(o->headers, have + len + 1);
  memcpy(o->headers + have, p, len);
  o->headers[have + len] = 0;
}

static size_t cu_header(char *p, size_t sz, size_t n, void *u) {
  CuCtx *c = (CuCtx *)u;
  size_t len = sz * n;
  char line[256];
  fm_strlcpy(line, p, FM_MIN(len + 1, sizeof line));
  line[strcspn(line, "\r\n")] = 0;
  FmNetResp *o = c->out;
  bool status_line = !fm_strnicmp(line, "HTTP/", 5) || !fm_strnicmp(line, "ICY ", 4);
  if (c->b->keep_headers) cu_keep(c, p, len, status_line);
  if (status_line) {
    o->length = -1;
    o->ranges = false;
    o->icy_metaint = 0;
    const char *sp = strchr(line, ' ');
    if (sp && atoi(sp + 1) == 206) o->ranges = true;
  } else if (!fm_strnicmp(line, "content-length:", 15)) {
    o->length = (i64)strtoll(line + 15, NULL, 10);
  } else if (!fm_strnicmp(line, "accept-ranges:", 14)) {
    const char *v = line + 14;
    while (*v == ' ') v++;
    if (!fm_strnicmp(v, "bytes", 5)) o->ranges = true;
  } else if (!fm_strnicmp(line, "content-range:", 14)) {
    const char *sl = strchr(line, '/');
    if (sl && sl[1] != '*') o->total = (i64)strtoll(sl + 1, NULL, 10);
  } else if (!fm_strnicmp(line, "icy-metaint:", 12)) {
    o->icy_metaint = atoi(line + 12);
  } else if (!fm_strnicmp(line, "icy-name:", 9)) {
    const char *v = line + 9;
    while (*v == ' ') v++;
    fm_strlcpy(o->icy_name, v, sizeof o->icy_name);
  }
  return len;
}

static size_t cu_write(char *p, size_t sz, size_t n, void *u) {
  CuCtx *c = (CuCtx *)u;
  if (!c->head_sent) {
    c->head_sent = true;
    long st = 0;
    cu.getinfo(c->h, CURLINFO_RESPONSE_CODE_, &st);
    c->out->status = (int)st;
    char *ct = NULL;
    if (cu.getinfo(c->h, CURLINFO_CONTENT_TYPE_, &ct) == 0 && ct) fm_strlcpy(c->out->type, ct, sizeof c->out->type);
    if (c->b->head) c->b->head(c->b->su, c->out);
  }
  if ((c->cancel && *c->cancel) || !body_put(c->b, p, sz * n)) { c->stop = true; return 0; }
  return sz * n;
}

static int cu_xfer(void *u, i64 dltotal, i64 dlnow, i64 ult, i64 uln) {
  CuCtx *c = (CuCtx *)u;
  if (c->cancel && *c->cancel) return 1;
  bool sending = c->b->upf && ult > 0 && uln < ult;     /* the body first, then the reply */
  if (c->cb && !c->cb(c->user, (u64)(sending ? uln : dlnow), (u64)(sending ? ult : dltotal))) return 1;
  return 0;
}

/* net_request: the file body in pieces */
static size_t cu_read(char *buf, size_t sz, size_t n, void *u) {
  CuCtx *c = (CuCtx *)u;
  if (c->cancel && *c->cancel) return 0x10000000;       /* CURL_READFUNC_ABORT */
  return fread(buf, 1, sz * n, c->b->upf);
}

static FmErr cu_request(const char *url, const char *headers, Body *b, FmNetProgress cb, void *user,
                        FmNetResp *out, volatile int *cancel) {
  memset(out, 0, sizeof *out);
  if (!cu_load()) { fm_strlcpy(out->error, "libcurl is not installed", sizeof out->error); return FM_ERR_UNSUPPORTED; }
  CURL *h = cu.init();
  if (!h) return FM_ERR_NOMEM;
  CuCtx ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.b = b;
  ctx.cb = cb;
  ctx.user = user;
  ctx.cancel = cancel;
  ctx.h = h;
  ctx.out = out;
  out->length = -1;
  struct curl_slist *hl = NULL;
  if (headers) {
    const char *p = headers;
    while (*p) {
      size_t n = strcspn(p, "\r\n");
      if (n) { char line[512]; fm_strlcpy(line, p, FM_MIN(n + 1, sizeof line)); hl = cu.slist_append(hl, line); }
      p += n;
      while (*p == '\r' || *p == '\n') p++;
    }
  }
  cu.setopt(h, CURLOPT_URL_, url);
  cu.setopt(h, CURLOPT_USERAGENT_, NET_UA);
  cu.setopt(h, CURLOPT_FOLLOWLOCATION_, 1L);
  cu.setopt(h, CURLOPT_MAXREDIRS_, 8L);
  cu.setopt(h, CURLOPT_NOSIGNAL_, 1L);
  cu.setopt(h, CURLOPT_CONNECTTIMEOUT_, 10L);
  cu.setopt(h, CURLOPT_LOW_SPEED_LIMIT_, 1L);
  cu.setopt(h, CURLOPT_LOW_SPEED_TIME_, 30L);
  cu.setopt(h, CURLOPT_ACCEPT_ENCODING_, "");
  cu.setopt(h, CURLOPT_WRITEFUNCTION_, (CurlWriteFn)cu_write);
  cu.setopt(h, CURLOPT_WRITEDATA_, (void *)&ctx);
  cu.setopt(h, CURLOPT_HEADERFUNCTION_, (CurlWriteFn)cu_header);
  cu.setopt(h, CURLOPT_HEADERDATA_, (void *)&ctx);
  cu.setopt(h, CURLOPT_HTTP09_ALLOWED_, 1L);   /* SHOUTcast v1 answers "ICY 200 OK" */
  cu.setopt(h, CURLOPT_XFERINFOFUNCTION_, (CurlXferFn)cu_xfer);
  cu.setopt(h, CURLOPT_XFERINFODATA_, (void *)&ctx);
  cu.setopt(h, CURLOPT_NOPROGRESS_, 0L);
  if (hl) cu.setopt(h, CURLOPT_HTTPHEADER_, hl);
  if (b->no_redirect) cu.setopt(h, CURLOPT_FOLLOWLOCATION_, 0L);
  if (b->upf) {                                          /* UPLOAD sends a body; the method is set below */
    cu.setopt(h, CURLOPT_UPLOAD_, 1L);
    cu.setopt(h, CURLOPT_READFUNCTION_, (CurlWriteFn)cu_read);
    cu.setopt(h, CURLOPT_READDATA_, (void *)&ctx);
    cu.setopt(h, CURLOPT_INFILESIZE_LARGE_, b->up_len);
  } else if (b->post) {
    cu.setopt(h, CURLOPT_POSTFIELDSIZE_LARGE_, (i64)b->post_len);
    cu.setopt(h, CURLOPT_POSTFIELDS_, b->post);
  }
  if (b->method && !strcmp(b->method, "HEAD")) cu.setopt(h, CURLOPT_NOBODY_, 1L);
  else if (b->method) cu.setopt(h, CURLOPT_CUSTOMREQUEST_, b->method);
  int rc = cu.perform(h);
  long status = 0;
  cu.getinfo(h, CURLINFO_RESPONSE_CODE_, &status);
  out->status = (int)status;
  char *ct = NULL;
  if (cu.getinfo(h, CURLINFO_CONTENT_TYPE_, &ct) == 0 && ct) fm_strlcpy(out->type, ct, sizeof out->type);
  FmErr err = FM_OK;
  if (rc) {
    if (cancel && *cancel) err = FM_ERR_CANCEL;
    else if (b->over) { err = FM_ERR_FULL; fm_strlcpy(out->error, "response too large", sizeof out->error); }
    else {
      err = FM_ERR_IO;
      fm_strlcpy(out->error, cu.strerror ? cu.strerror(rc) : "transfer failed", sizeof out->error);
    }
  }
  if (hl) cu.slist_free(hl);
  cu.cleanup(h);
  return err;
}

#define REQUEST cu_request

/* ============================================================================ */
#elif defined(FM_ANDROID)

/* Android: HttpURLConnection through FmNet.java (system TLS and certificates).
** Worker threads attach through SDL_AndroidGetJNIEnv; FindClass there would
** use the system class loader, so the class comes from the activity's loader
** once and is kept as a global reference. */
#include <jni.h>

static struct {
  int state;
  jclass cls, conn;
  jmethodID open, header, read, close, hblock;
  jfieldID status, length, type, error;
} jx;
static SDL_SpinLock g_jn_lock;

static bool jn_clear(JNIEnv *e) {
  if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionClear(e); return true; }
  return false;
}

static jclass jn_class(JNIEnv *e, jobject loader, jmethodID load, const char *name) {
  jstring s = (*e)->NewStringUTF(e, name);
  jclass c = (jclass)(*e)->CallObjectMethod(e, loader, load, s);
  (*e)->DeleteLocalRef(e, s);
  if (jn_clear(e) || !c) return NULL;
  jclass g = (jclass)(*e)->NewGlobalRef(e, c);
  (*e)->DeleteLocalRef(e, c);
  return g;
}

static JNIEnv *jn_load(void) {
  JNIEnv *e = (JNIEnv *)SDL_AndroidGetJNIEnv();
  if (!e) return NULL;
  SDL_AtomicLock(&g_jn_lock);
  if (!jx.state) {
    jx.state = -1;
    jobject act = (jobject)SDL_AndroidGetActivity();
    if (act) {
      jclass ac = (*e)->GetObjectClass(e, act);
      jmethodID gl = (*e)->GetMethodID(e, ac, "getClassLoader", "()Ljava/lang/ClassLoader;");
      jobject loader = gl ? (*e)->CallObjectMethod(e, act, gl) : NULL;
      jn_clear(e);
      if (loader) {
        jclass lc = (*e)->GetObjectClass(e, loader);
        jmethodID load = (*e)->GetMethodID(e, lc, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        if (load) {
          jx.cls = jn_class(e, loader, load, "io.github.mmc.filemanager.FmNet");
          jx.conn = jn_class(e, loader, load, "io.github.mmc.filemanager.FmNet$Conn");
        }
        jn_clear(e);
        (*e)->DeleteLocalRef(e, lc);
        (*e)->DeleteLocalRef(e, loader);
      }
      (*e)->DeleteLocalRef(e, ac);
      (*e)->DeleteLocalRef(e, act);
    }
    if (jx.cls && jx.conn) {
      jx.open = (*e)->GetStaticMethodID(e, jx.cls, "open",
                                        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[B"
                                        "Ljava/lang/String;Ljava/lang/String;JJZ)"
                                        "Lio/github/mmc/filemanager/FmNet$Conn;");
      jx.hblock = (*e)->GetStaticMethodID(e, jx.cls, "headerBlock",
                                          "(Lio/github/mmc/filemanager/FmNet$Conn;)Ljava/lang/String;");
      jx.header = (*e)->GetStaticMethodID(e, jx.cls, "header",
                                          "(Lio/github/mmc/filemanager/FmNet$Conn;Ljava/lang/String;)Ljava/lang/String;");
      jx.read = (*e)->GetStaticMethodID(e, jx.cls, "read", "(Lio/github/mmc/filemanager/FmNet$Conn;[B)I");
      jx.close = (*e)->GetStaticMethodID(e, jx.cls, "close", "(Lio/github/mmc/filemanager/FmNet$Conn;)V");
      jx.status = (*e)->GetFieldID(e, jx.conn, "status", "I");
      jx.length = (*e)->GetFieldID(e, jx.conn, "length", "J");
      jx.type = (*e)->GetFieldID(e, jx.conn, "type", "Ljava/lang/String;");
      jx.error = (*e)->GetFieldID(e, jx.conn, "error", "Ljava/lang/String;");
      jn_clear(e);
      if (jx.open && jx.header && jx.read && jx.close && jx.hblock && jx.status && jx.length && jx.type && jx.error)
        jx.state = 1;
    }
  }
  SDL_AtomicUnlock(&g_jn_lock);
  return jx.state > 0 ? e : NULL;
}

bool net_available(void) { return jn_load() != NULL; }
const char *net_backend(void) { return jn_load() ? "HttpURLConnection" : "the Java network helper is missing"; }

/* a String field or header of the connection into out ("" when null) */
static void jn_str(JNIEnv *e, jstring s, char *out, size_t cap) {
  out[0] = 0;
  if (!s) return;
  const char *u = (*e)->GetStringUTFChars(e, s, NULL);
  if (u) {
    fm_strlcpy(out, u, cap);
    (*e)->ReleaseStringUTFChars(e, s, u);
  }
  (*e)->DeleteLocalRef(e, s);
}

static void jn_header(JNIEnv *e, jobject c, const char *name, char *out, size_t cap) {
  jstring n = (*e)->NewStringUTF(e, name);
  jstring v = (jstring)(*e)->CallStaticObjectMethod(e, jx.cls, jx.header, c, n);
  (*e)->DeleteLocalRef(e, n);
  if (jn_clear(e)) v = NULL;
  jn_str(e, v, out, cap);
}

static FmErr jn_request(const char *url, const char *headers, Body *b, FmNetProgress cb, void *user,
                        FmNetResp *out, volatile int *cancel) {
  memset(out, 0, sizeof *out);
  out->length = -1;
  JNIEnv *e = jn_load();
  if (!e) { fm_strlcpy(out->error, net_backend(), sizeof out->error); return FM_ERR_UNSUPPORTED; }
  if ((*e)->PushLocalFrame(e, 16) < 0) { jn_clear(e); return FM_ERR_NOMEM; }
  jstring ju = (*e)->NewStringUTF(e, url);
  jstring jh = (*e)->NewStringUTF(e, headers ? headers : "");
  jstring ja = (*e)->NewStringUTF(e, NET_UA);
  jbyteArray jp = NULL;
  if (b->post && b->post_len < (1u << 30)) {
    jp = (*e)->NewByteArray(e, (jsize)b->post_len);
    if (jp) (*e)->SetByteArrayRegion(e, jp, 0, (jsize)b->post_len, (const jbyte *)b->post);
  }
  /* a file body is streamed by Java from the file itself (path, offset, length) */
  jstring jm = b->method ? (*e)->NewStringUTF(e, b->method) : NULL;
  jstring jf = b->up_path[0] ? (*e)->NewStringUTF(e, b->up_path) : NULL;
  jobject c = (ju && jh && ja && (jp || !b->post))
                  ? (*e)->CallStaticObjectMethod(e, jx.cls, jx.open, ju, jh, ja, jp, jm, jf, (jlong)b->up_off,
                                                 (jlong)b->up_len, (jboolean)!b->no_redirect)
                  : NULL;
  if (jn_clear(e) || !c) {
    (*e)->PopLocalFrame(e, NULL);
    fm_strlcpy(out->error, "could not start the request", sizeof out->error);
    return FM_ERR_IO;
  }
  FmErr err = FM_OK;
  out->status = (int)(*e)->GetIntField(e, c, jx.status);
  if (!out->status) {
    jn_str(e, (jstring)(*e)->GetObjectField(e, c, jx.error), out->error, sizeof out->error);
    (*e)->PopLocalFrame(e, NULL);
    return FM_ERR_IO;
  }
  out->length = (i64)(*e)->GetLongField(e, c, jx.length);
  jn_str(e, (jstring)(*e)->GetObjectField(e, c, jx.type), out->type, sizeof out->type);
  char v[160];
  jn_header(e, c, "Accept-Ranges", v, sizeof v);
  out->ranges = out->status == 206 || !fm_strnicmp(v, "bytes", 5);
  jn_header(e, c, "Content-Range", v, sizeof v);
  const char *sl = strchr(v, '/');
  if (sl && sl[1] != '*') out->total = (i64)strtoll(sl + 1, NULL, 10);
  jn_header(e, c, "icy-metaint", v, sizeof v);
  out->icy_metaint = atoi(v);
  jn_header(e, c, "icy-name", out->icy_name, sizeof out->icy_name);
  if (b->keep_headers) {
    jstring hb = (jstring)(*e)->CallStaticObjectMethod(e, jx.cls, jx.hblock, c);
    if (jn_clear(e)) hb = NULL;
    if (hb) {
      const char *u = (*e)->GetStringUTFChars(e, hb, NULL);
      if (u) {
        out->headers = fm_strdup(u);
        (*e)->ReleaseStringUTFChars(e, hb, u);
      }
      (*e)->DeleteLocalRef(e, hb);
    }
  }
  if (b->head) b->head(b->su, out);

  enum { CHUNK = 64 * 1024 };
  jbyteArray arr = (*e)->NewByteArray(e, CHUNK);
  u8 *tmp = arr ? (u8 *)fm_alloc(CHUNK) : NULL;
  u64 done = 0;
  while (tmp) {
    if (cancel && *cancel) { err = FM_ERR_CANCEL; break; }
    jint n = (*e)->CallStaticIntMethod(e, jx.cls, jx.read, c, arr);
    if (jn_clear(e)) n = -1;
    if (n == 0) break;
    if (n < 0) {
      jn_str(e, (jstring)(*e)->GetObjectField(e, c, jx.error), out->error, sizeof out->error);
      err = FM_ERR_IO;
      break;
    }
    (*e)->GetByteArrayRegion(e, arr, 0, n, (jbyte *)tmp);
    if (!body_put(b, tmp, (size_t)n)) {
      if (cancel && *cancel) err = FM_ERR_CANCEL;
      else {
        fm_strlcpy(out->error, b->over ? "response too large" : "write failed", sizeof out->error);
        err = b->over ? FM_ERR_FULL : FM_ERR_IO;
      }
      break;
    }
    done += (u64)n;
    if (cb && !cb(user, done, out->length > 0 ? (u64)out->length : 0)) { err = FM_ERR_CANCEL; break; }
  }
  if (!tmp) err = FM_ERR_NOMEM;
  fm_free(tmp);
  (*e)->CallStaticVoidMethod(e, jx.cls, jx.close, c);
  jn_clear(e);
  (*e)->PopLocalFrame(e, NULL);
  return err;
}

#define REQUEST jn_request

/* ============================================================================ */
#else

bool net_available(void) { return false; }
const char *net_backend(void) { return "networking is not available on this platform yet"; }

static FmErr no_request(const char *url, const char *headers, Body *b, FmNetProgress cb, void *user,
                        FmNetResp *out, volatile int *cancel) {
  FM_UNUSED(url); FM_UNUSED(headers); FM_UNUSED(b); FM_UNUSED(cb); FM_UNUSED(user); FM_UNUSED(cancel);
  memset(out, 0, sizeof *out);
  fm_strlcpy(out->error, net_backend(), sizeof out->error);
  return FM_ERR_UNSUPPORTED;
}

#define REQUEST no_request

#endif

/* ---- public ------------------------------------------------------------------ */

FmErr net_get(const char *url, const char *headers, size_t max_bytes, FmNetResp *out, volatile int *cancel) {
  Body b;
  memset(&b, 0, sizeof b);
  b.max = max_bytes ? max_bytes : (16u << 20);
  FmErr err = REQUEST(url, headers, &b, NULL, NULL, out, cancel);
  if (err == FM_OK && !b.data) b.data = (u8 *)fm_calloc(1, 1);   /* empty body, still a string */
  if (err != FM_OK) { fm_free(b.data); b.data = NULL; b.len = 0; }
  out->data = b.data;
  out->len = b.len;
  return err;
}

FmErr net_post(const char *url, const char *headers, const void *body, size_t len, size_t max_bytes, FmNetResp *out,
               volatile int *cancel) {
  Body b;
  memset(&b, 0, sizeof b);
  b.max = max_bytes ? max_bytes : (16u << 20);
  b.post = body ? body : "";
  b.post_len = body ? len : 0;
  FmErr err = REQUEST(url, headers, &b, NULL, NULL, out, cancel);
  if (err == FM_OK && !b.data) b.data = (u8 *)fm_calloc(1, 1);
  if (err != FM_OK) { fm_free(b.data); b.data = NULL; b.len = 0; }
  out->data = b.data;
  out->len = b.len;
  return err;
}

FmErr net_request(const char *url, const FmNetReq *rq, FmNetResp *out, volatile int *cancel) {
  Body b;
  memset(&b, 0, sizeof b);
  memset(out, 0, sizeof *out);
  b.max = rq->max_reply ? rq->max_reply : (16u << 20);
  b.method = rq->method;
  b.keep_headers = true;
  b.no_redirect = rq->no_redirect;
  if (rq->body) {
    b.post = rq->body;
    b.post_len = rq->body_len;
  } else if (rq->body_file) {
    b.upf = fm_fopen(rq->body_file, "rb");
    if (!b.upf || (rq->body_off && fm_fseek64(b.upf, rq->body_off, SEEK_SET) != 0)) {
      if (b.upf) fclose(b.upf);
      fm_strlcpy(out->error, "cannot read the file to send", sizeof out->error);
      return FM_ERR_IO;
    }
    b.up_off = rq->body_off;
    b.up_len = rq->body_file_len;
    fm_strlcpy(b.up_path, rq->body_file, sizeof b.up_path);
  } else if (rq->method && (!strcmp(rq->method, "PUT") || !strcmp(rq->method, "POST") || !strcmp(rq->method, "PATCH"))) {
    b.post = "";                               /* an empty body still says Content-Length: 0 */
  }
  if (rq->out_file) {
    b.f = fm_fopen(rq->out_file, "wb");
    if (!b.f) {
      if (b.upf) fclose(b.upf);
      fm_strlcpy(out->error, "cannot create the file", sizeof out->error);
      return FM_ERR_IO;
    }
  }
  FmErr err = REQUEST(url, rq->headers, &b, rq->progress, rq->user, out, cancel);
  if (b.upf) fclose(b.upf);
  if (b.f && fclose(b.f) != 0 && err == FM_OK) err = FM_ERR_IO;
  if (!b.f) {
    if (err == FM_OK && !b.data) b.data = (u8 *)fm_calloc(1, 1);
    if (err != FM_OK) { fm_free(b.data); b.data = NULL; b.len = 0; }
    out->data = b.data;
    out->len = b.len;
  }
  return err;
}

FmErr net_get_stream(const char *url, const char *headers, FmNetHead head, FmNetData data, void *user,
                     FmNetResp *out, volatile int *cancel) {
  Body b;
  memset(&b, 0, sizeof b);
  b.head = head;
  b.sink = data;
  b.su = user;
  FmErr err = REQUEST(url, headers, &b, NULL, NULL, out, cancel);
  out->data = NULL;
  out->len = 0;
  return err;
}

FmErr net_download(const char *url, const char *headers, const char *path, FmNetProgress cb, void *user,
                   FmNetResp *out, volatile int *cancel) {
  char part[FM_PATH_MAX];
  fm_snprintf(part, sizeof part, "%s.part", path);
  Body b;
  memset(&b, 0, sizeof b);
  b.f = fm_fopen(part, "wb");
  if (!b.f) {
    memset(out, 0, sizeof *out);
    fm_strlcpy(out->error, "cannot create the file", sizeof out->error);
    return FM_ERR_IO;
  }
  FmErr err = REQUEST(url, headers, &b, cb, user, out, cancel);
  if (fclose(b.f) != 0 && err == FM_OK) err = FM_ERR_IO;
  if (err == FM_OK && (out->status < 200 || out->status >= 300)) err = FM_ERR_IO;
  if (err == FM_OK) {
    plat_remove_file(path);
    err = plat_rename(part, path);
  }
  if (err != FM_OK) plat_remove_file(part);
  out->data = NULL;
  out->len = 0;
  return err;
}
