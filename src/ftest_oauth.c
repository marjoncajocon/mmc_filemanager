/* ftest_oauth.c -- OAuth sign-in and the Google Drive / Dropbox / OneDrive
** adapters, all offline.
**
** Design decisions:
**   - The whole browser flow runs for real on loopback: oauth_login in a
**     thread, a mock token server on another port (a thread with a socket),
**     and the test "plays the browser" by reading the sign-in URL from the
**     g_oauth_browser hook and sending the redirect itself. The mock checks
**     the PKCE verifier against the challenge from that URL.
**   - Adapters are tested through their parsers with canned replies in the
**     services' documented shapes (pages, cursors, nextLink, errors, quota);
**     the 401 -> refresh -> retry path runs against the mock.
*/
#include "ftest.h"
#include "foauth_int.h"
#include "fplat.h"
#include "fsdl.h"
#include "fcrypt.h"

extern const FmCloud g_cloud_gdrive, g_cloud_dropbox, g_cloud_onedrive;

/* ---- small vectors ------------------------------------------------------------------ */

static void test_pkce(void) {
  char ch[64];
  oauth_pkce_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk", ch, sizeof ch);   /* RFC 7636 app. B */
  TEST_CHECK(!strcmp(ch, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"));
  const u8 b[] = { 0xfb, 0xff, 0xbf };
  oauth_b64url(b, 3, ch, sizeof ch);
  TEST_CHECK(!strcmp(ch, "-_-_"));
  oauth_b64url(b, 1, ch, sizeof ch);
  TEST_CHECK(!strcmp(ch, "-w"));
}

static void test_redirect(void) {
  char code[256], st[64], er[256];
  TEST_CHECK(oauth_parse_redirect("GET /?state=abc&code=4%2F0AX-y_z&scope=x+y HTTP/1.1\r\nHost: h\r\n\r\n", code,
                                  sizeof code, st, sizeof st, er, sizeof er));
  TEST_CHECK(!strcmp(code, "4/0AX-y_z") && !strcmp(st, "abc") && !er[0]);
  TEST_CHECK(oauth_parse_redirect("GET /?error=access_denied&error_description=The+user+said+no&state=s HTTP/1.1",
                                  code, sizeof code, st, sizeof st, er, sizeof er));
  TEST_CHECK(!code[0] && !strcmp(st, "s") && !strcmp(er, "access_denied: The user said no"));
  TEST_CHECK(!oauth_parse_redirect("GET /favicon.ico HTTP/1.1", code, sizeof code, st, sizeof st, er, sizeof er));
  TEST_CHECK(!oauth_parse_redirect("POST /?code=x&state=y HTTP/1.1", code, sizeof code, st, sizeof st, er, sizeof er));
  TEST_CHECK(!oauth_parse_redirect("GET /?state=only HTTP/1.1", code, sizeof code, st, sizeof st, er, sizeof er));
  TEST_CHECK(!oauth_parse_redirect("", code, sizeof code, st, sizeof st, er, sizeof er));
}

static void test_tokens(void) {
  FmOAuthTok t;
  char err[256];
  memset(&t, 0, sizeof t);
  const char *g = "{\"access_token\":\"ya29.a0Af\",\"expires_in\":3599,\"refresh_token\":\"1//0gX\","
                  "\"scope\":\"https://www.googleapis.com/auth/drive\",\"token_type\":\"Bearer\"}";
  TEST_CHECK(oauth_parse_token(g, strlen(g), &t, err, sizeof err) == FM_OK);
  TEST_CHECK(!strcmp(t.access, "ya29.a0Af") && !strcmp(t.refresh, "1//0gX"));
  TEST_CHECK(t.expires > plat_time_unix() + 3500 && oauth_fresh(&t));
  const char *r = "{\"access_token\":\"new\",\"expires_in\":10,\"token_type\":\"Bearer\"}";   /* no new refresh */
  TEST_CHECK(oauth_parse_token(r, strlen(r), &t, err, sizeof err) == FM_OK);
  TEST_CHECK(!strcmp(t.access, "new") && !strcmp(t.refresh, "1//0gX") && !oauth_fresh(&t));
  const char *ms = "{\"error\":\"invalid_grant\",\"error_description\":\"AADSTS70008: expired\"}";
  TEST_CHECK(oauth_parse_token(ms, strlen(ms), &t, err, sizeof err) == FM_ERR_PASSWORD);
  TEST_CHECK(strstr(err, "Signed out") != NULL);
  const char *ic = "{\"error\":\"invalid_client\",\"error_description\":\"The OAuth client was not found.\"}";
  TEST_CHECK(oauth_parse_token(ic, strlen(ic), &t, err, sizeof err) == FM_ERR_ACCESS);
  TEST_CHECK(strstr(err, "client id") && strstr(err, "not found"));
  TEST_CHECK(oauth_parse_token("<html>", 6, &t, err, sizeof err) == FM_ERR_FORMAT);
  const char *sp = "{\"access_token\":\"has space\"}";
  TEST_CHECK(oauth_parse_token(sp, strlen(sp), &t, err, sizeof err) == FM_ERR_FORMAT);

  /* save / load */
  FmOAuthTok a, b;
  memset(&a, 0, sizeof a);
  fm_strlcpy(a.access, "AT.x-y_z", sizeof a.access);
  fm_strlcpy(a.refresh, "1//RT", sizeof a.refresh);
  a.expires = 1790000000;
  char line[4096];
  oauth_save(&a, line, sizeof line);
  TEST_CHECK(!strcmp(line, "o1 1790000000 1//RT AT.x-y_z"));
  TEST_CHECK(oauth_load(&b, line) && !strcmp(b.access, a.access) && !strcmp(b.refresh, a.refresh) &&
             b.expires == a.expires);
  a.refresh[0] = 0;
  oauth_save(&a, line, sizeof line);
  TEST_CHECK(oauth_load(&b, line) && !b.refresh[0] && !strcmp(b.access, a.access));
  /* an access token too long for the session: dropped, refreshed on next use */
  memset(a.access, 'A', 3000);
  a.access[3000] = 0;
  fm_strlcpy(a.refresh, "RT2", sizeof a.refresh);
  char small[600];
  oauth_save(&a, small, sizeof small);
  TEST_CHECK(!strcmp(small, "o1 1 RT2 -"));
  TEST_CHECK(oauth_load(&b, small) && !b.access[0] && !strcmp(b.refresh, "RT2") && !oauth_fresh(&b));
  oauth_save(&a, line, sizeof line);
  TEST_CHECK(oauth_load(&b, line) && strlen(b.access) == 3000);
  TEST_CHECK(!oauth_load(&b, "") && !oauth_load(&b, "o1 x y z") && !oauth_load(&b, "o1 5 - -") &&
             !oauth_load(&b, "garbage"));
}

/* ---- mock token / API server ----------------------------------------------------------- */

typedef struct Mock {
  FmOaSock ls;
  int port;
  volatile int stop;
  char challenge[64];
  int token_calls, api_calls;
  /* the fake cloud */
  u64 up_got;              /* resumable / session bytes received in order */
  u32 up_crc;
  int fail_once;           /* answer the next chunk with 503 */
  int auth_leaks;          /* bearer seen where it must not be */
  char last_arg[1024];     /* Dropbox-API-Arg of the last content call */
} Mock;

static bool form_get(const char *body, const char *key, char *out, size_t cap) {
  size_t kn = strlen(key);
  for (const char *p = body; p && *p;) {
    if (!strncmp(p, key, kn) && p[kn] == '=') {
      p += kn + 1;
      size_t n = strcspn(p, "&");
      size_t o = 0;
      for (size_t i = 0; i < n && o + 1 < cap; i++) {
        if (p[i] == '%' && i + 2 < n) {
          char h[3] = { p[i + 1], p[i + 2], 0 };
          out[o++] = (char)strtol(h, NULL, 16);
          i += 2;
        } else {
          out[o++] = p[i] == '+' ? ' ' : p[i];
        }
      }
      out[o] = 0;
      return true;
    }
    p = strchr(p, '&');
    if (p) p++;
  }
  if (cap) out[0] = 0;
  return false;
}

static void mock_send(FmOaSock c, int status, const char *extra, const char *type, const char *body, size_t n) {
  char head[1024];
  fm_snprintf(head, sizeof head, "HTTP/1.1 %d X\r\nContent-Type: %s\r\nContent-Length: %d\r\n%sConnection: close\r\n\r\n",
              status, type, (int)n, extra ? extra : "");
  oa_sock_send(c, head, (int)strlen(head));
  if (n) oa_sock_send(c, body, (int)n);
}

static void mock_reply(FmOaSock c, int status, const char *json) {
  mock_send(c, status, NULL, "application/json", json, strlen(json));
}

static bool hdr_get(const char *head, const char *name, char *out, size_t cap) {
  char key[80];
  fm_snprintf(key, sizeof key, "\n%s:", name);
  const char *p = fm_stristr(head, key);
  if (cap) out[0] = 0;
  if (!p) return false;
  p += strlen(key);
  while (*p == ' ') p++;
  size_t n = strcspn(p, "\r\n");
  if (n >= cap) n = cap - 1;
  memcpy(out, p, n);
  out[n] = 0;
  return true;
}

/* "bytes a-b/total" -> a, b+1 */
static bool content_range(const char *head, u64 *a, u64 *b) {
  char v[128];
  if (!hdr_get(head, "Content-Range", v, sizeof v) || strncmp(v, "bytes ", 6) || v[6] == '*') return false;
  *a = (u64)atof(v + 6);
  const char *d = strchr(v, '-');
  *b = d ? (u64)atof(d + 1) + 1 : 0;
  return d != NULL;
}

static u64 range_total(const char *head) {
  char v[128];
  if (!hdr_get(head, "Content-Range", v, sizeof v) || !strchr(v, '/')) return 0;
  return (u64)atof(strchr(v, '/') + 1);
}

/* a chunk of an upload: kept only when it continues the bytes so far */
static void take_chunk(Mock *m, const char *head, const char *body, size_t blen) {
  u64 a, b;
  if (content_range(head, &a, &b) && a == m->up_got && b - a == blen) {
    m->up_crc = crc32_update(m->up_crc, body, blen);
    m->up_got = b;
  }
}

static void fake_cloud(Mock *m, FmOaSock c, const char *head, const char *body, size_t blen) {
  char tgt[2048], v[1024], loc[256];
  const char *sp = strchr(head, ' ');
  size_t tn = sp ? strcspn(sp + 1, " ") : 0;
  if (tn >= sizeof tgt) tn = sizeof tgt - 1;
  memcpy(tgt, sp ? sp + 1 : "", tn);
  tgt[tn] = 0;
  bool authed = hdr_get(head, "Authorization", v, sizeof v) && !strcmp(v, "Bearer AT-T");

  /* ---- Google Drive ---- */
  if (!strncmp(tgt, "/www.googleapis.com/drive/v3/files?", 35)) {
    if (!authed) { mock_reply(c, 401, "{}"); return; }
    if (strstr(tgt, "fields=files(id)")) { mock_reply(c, 200, "{\"files\":[]}"); return; }     /* gd_find */
    if (strstr(tgt, "pageToken=T2")) {
      mock_reply(c, 200, "{\"files\":[{\"id\":\"1f\",\"name\":\"b.txt\",\"mimeType\":\"text/plain\",\"size\":\"5\"}]}");
    } else if (strstr(tgt, "%27root%27%20in%20parents")) {
      mock_reply(c, 200, "{\"nextPageToken\":\"T2\",\"files\":[{\"id\":\"1d\",\"name\":\"A\",\"mimeType\":"
                         "\"application/vnd.google-apps.folder\"}]}");
    } else {
      mock_reply(c, 400, "{\"error\":{\"code\":400,\"message\":\"bad q\"}}");
    }
    return;
  }
  if (!strncmp(tgt, "/www.googleapis.com/drive/v3/files/1f?alt=media", 47)) {
    if (authed) mock_send(c, 200, NULL, "text/plain", "hello drive", 11);
    else mock_reply(c, 401, "{}");
    return;
  }
  if (!strncmp(tgt, "/www.googleapis.com/drive/v3/files/1doc/export?mimeType=application%2Fvnd.openxml", 80)) {
    mock_send(c, 200, NULL, "application/octet-stream", "DOCX", 4);
    return;
  }
  if (!strncmp(tgt, "/www.googleapis.com/drive/v3/files/gone?alt=media", 49)) {
    mock_reply(c, 404, "{\"error\":{\"code\":404,\"message\":\"File not found: gone.\"}}");
    return;
  }
  if (!strncmp(tgt, "/www.googleapis.com/upload/drive/v3/files?uploadType=resumable&upload_id=S1", 76)) {
    if (m->fail_once && blen) {
      m->fail_once = 0;
      mock_reply(c, 503, "{\"error\":{\"message\":\"backend error\"}}");
      return;
    }
    take_chunk(m, head, body, blen);
    if (m->up_got && m->up_got == range_total(head)) {
      char js[256];
      fm_snprintf(js, sizeof js, "{\"id\":\"1big\",\"name\":\"big.bin\",\"mimeType\":\"application/octet-stream\","
                  "\"size\":\"%llu\"}", (unsigned long long)m->up_got);
      mock_reply(c, 200, js);
    } else if (m->up_got) {
      fm_snprintf(loc, sizeof loc, "Range: bytes=0-%llu\r\n", (unsigned long long)m->up_got - 1);
      mock_send(c, 308, loc, "text/plain", "", 0);
    } else {
      mock_send(c, 308, NULL, "text/plain", "", 0);
    }
    return;
  }
  if (!strncmp(tgt, "/www.googleapis.com/upload/drive/v3/files?uploadType=resumable", 62)) {
    if (!authed || !hdr_get(head, "X-Upload-Content-Length", v, sizeof v) || !strstr(body, "\"name\":\"big.bin\"")) {
      mock_reply(c, 400, "{\"error\":{\"message\":\"bad init\"}}");
      return;
    }
    m->up_got = 0;
    m->up_crc = 0;
    mock_send(c, 200, "Location: https://www.googleapis.com/upload/drive/v3/files?uploadType=resumable&upload_id=S1\r\n",
              "application/json", "", 0);
    return;
  }
  if (!strncmp(tgt, "/www.googleapis.com/upload/drive/v3/files?uploadType=multipart", 62)) {
    if (!hdr_get(head, "Content-Type", v, sizeof v) || strncmp(v, "multipart/related; boundary=", 28) ||
        !strstr(body, "\"name\":\"small.txt\"") || !strstr(body, "\r\n\r\nsmall body\r\n--")) {
      mock_reply(c, 400, "{\"error\":{\"message\":\"bad multipart\"}}");
      return;
    }
    mock_reply(c, 200, "{\"id\":\"1s\",\"name\":\"small.txt\",\"mimeType\":\"text/plain\",\"size\":\"10\"}");
    return;
  }

  /* ---- OneDrive ---- */
  if (!strncmp(tgt, "/graph.microsoft.com/v1.0/me/drive/root/children?", 49)) {
    if (!authed) { mock_reply(c, 401, "{}"); return; }
    if (strstr(tgt, "$skiptoken=X"))
      mock_reply(c, 200, "{\"value\":[{\"id\":\"F!2\",\"name\":\"b.txt\",\"size\":5,\"file\":{}}]}");
    else
      mock_reply(c, 200, "{\"value\":[{\"id\":\"F!1\",\"name\":\"A\",\"folder\":{}}],\"@odata.nextLink\":"
                         "\"https://graph.microsoft.com/v1.0/me/drive/root/children?$skiptoken=X\"}");
    return;
  }
  if (!strcmp(tgt, "/graph.microsoft.com/v1.0/me/drive/items/F%212/content")) {
    fm_snprintf(loc, sizeof loc, "Location: http://127.0.0.1:%d/dl/F2\r\n", m->port);
    mock_send(c, authed ? 302 : 401, loc, "text/plain", "", 0);
    return;
  }
  if (!strcmp(tgt, "/dl/F2")) {
    if (hdr_get(head, "Authorization", v, sizeof v)) m->auth_leaks++;
    mock_send(c, 200, NULL, "text/plain", "hello onedrive", 14);
    return;
  }
  if (!strcmp(tgt, "/graph.microsoft.com/v1.0/me/drive/root:/big.bin:/createUploadSession")) {
    m->up_got = 0;
    m->up_crc = 0;
    mock_reply(c, authed ? 200 : 401, "{\"uploadUrl\":\"https://graph.microsoft.com/up/S2\"}");
    return;
  }
  if (!strcmp(tgt, "/graph.microsoft.com/up/S2")) {
    if (hdr_get(head, "Authorization", v, sizeof v)) m->auth_leaks++;
    take_chunk(m, head, body, blen);
    if (m->up_got && m->up_got == range_total(head)) {
      mock_reply(c, 201, "{\"id\":\"F!big\",\"name\":\"big.bin\",\"size\":12582912,\"file\":{}}");
    } else {
      char js[128];
      fm_snprintf(js, sizeof js, "{\"nextExpectedRanges\":[\"%llu-\"]}", (unsigned long long)m->up_got);
      mock_reply(c, 202, js);
    }
    return;
  }
  if (!strncmp(tgt, "/graph.microsoft.com/v1.0/me/drive/items/F%211:/sm%C3%A9.txt:/content?", 70)) {
    bool ok = authed && blen == 10 && !memcmp(body, "small body", 10);
    mock_reply(c, ok ? 201 : 400, ok ? "{\"id\":\"F!s\",\"name\":\"sm\\u00e9.txt\",\"size\":10,\"file\":{}}" : "{}");
    return;
  }

  /* ---- Dropbox ---- */
  if (!strcmp(tgt, "/api.dropboxapi.com/2/files/list_folder")) {
    bool ok = authed && strstr(body, "\"path\":\"\"");
    mock_reply(c, ok ? 200 : 400, ok ? "{\"entries\":[{\".tag\":\"folder\",\"name\":\"A\",\"path_display\":\"/A\"}],"
                                       "\"cursor\":\"C1\",\"has_more\":true}" : "bad");
    return;
  }
  if (!strcmp(tgt, "/api.dropboxapi.com/2/files/list_folder/continue")) {
    bool ok = authed && strstr(body, "\"cursor\":\"C1\"");
    mock_reply(c, ok ? 200 : 400, ok ? "{\"entries\":[{\".tag\":\"file\",\"name\":\"b.txt\",\"path_display\":\"/b.txt\","
                                       "\"size\":5}],\"cursor\":\"C2\",\"has_more\":false}" : "bad");
    return;
  }
  if (!strcmp(tgt, "/content.dropboxapi.com/2/files/download")) {
    hdr_get(head, "Dropbox-API-Arg", m->last_arg, sizeof m->last_arg);
    mock_send(c, authed ? 200 : 401, NULL, "application/octet-stream", "hello dropbox", 13);
    return;
  }
  if (!strcmp(tgt, "/content.dropboxapi.com/2/files/upload")) {
    hdr_get(head, "Dropbox-API-Arg", m->last_arg, sizeof m->last_arg);
    bool ok = authed && blen == 10 && !memcmp(body, "small body", 10);
    mock_reply(c, ok ? 200 : 400, ok ? "{\"name\":\"up.txt\",\"path_display\":\"/A/up.txt\",\"size\":10}" : "bad");
    return;
  }
  if (!strcmp(tgt, "/api.dropboxapi.com/2/files/delete_v2")) {
    mock_reply(c, 409, "{\"error_summary\":\"path_lookup/not_found/..\",\"error\":{\".tag\":\"path_lookup\"}}");
    return;
  }
  if (!strcmp(tgt, "/api.dropboxapi.com/2/users/get_space_usage")) {
    mock_reply(c, 200, "{\"used\":10,\"allocation\":{\".tag\":\"individual\",\"allocated\":100}}");
    return;
  }
  mock_reply(c, 404, "{\"error\":{\"message\":\"no such mock\"}}");
}

static void mock_handle(Mock *m, FmOaSock c) {
  char req[16384];
  int n = 0;
  char *he = NULL;
  for (int tries = 0; tries < 40 && n < (int)sizeof req - 1 && !he; tries++) {
    int r = oa_sock_recv(c, req + n, (int)sizeof req - 1 - n, 100);
    if (r == 0) break;
    if (r > 0) n += r;
    req[n] = 0;
    he = strstr(req, "\r\n\r\n");
  }
  if (!he) { oa_sock_close(c); return; }
  *he = 0;                                         /* req = the head */
  char v[64];
  size_t clen = hdr_get(req, "Content-Length", v, sizeof v) ? (size_t)atof(v) : 0;
  size_t have = (size_t)(req + n - (he + 4));
  char *body = (char *)fm_alloc(clen + have + 1);
  memcpy(body, he + 4, have);
  for (int idle = 0; have < clen && idle < 50;) {
    int r = oa_sock_recv(c, body + have, (int)FM_MIN(clen - have, (size_t)1 << 20), 100);
    if (r == 0) break;
    if (r < 0) { idle++; continue; }
    have += (size_t)r;
    idle = 0;
  }
  body[have] = 0;
  char v1[1024], v2[1024];
  if (!strncmp(req, "POST /token ", 12)) {
    m->token_calls++;
    form_get(body, "grant_type", v1, sizeof v1);
    if (!strcmp(v1, "authorization_code")) {
      char ch[64];
      form_get(body, "code_verifier", v2, sizeof v2);
      oauth_pkce_challenge(v2, ch, sizeof ch);
      form_get(body, "code", v1, sizeof v1);
      bool ok = !strcmp(v1, "c0de-42") && !strcmp(ch, m->challenge) && strlen(v2) >= 43;
      form_get(body, "client_id", v1, sizeof v1);
      ok = ok && !strcmp(v1, "test-client");
      form_get(body, "redirect_uri", v1, sizeof v1);
      ok = ok && !strncmp(v1, "http://127.0.0.1:", 17);
      if (ok) mock_reply(c, 200, "{\"access_token\":\"AT-1\",\"refresh_token\":\"RT-1\",\"expires_in\":3600,"
                                 "\"token_type\":\"Bearer\"}");
      else mock_reply(c, 400, "{\"error\":\"invalid_grant\",\"error_description\":\"bad code or verifier\"}");
    } else if (!strcmp(v1, "refresh_token")) {
      form_get(body, "refresh_token", v1, sizeof v1);
      if (!strcmp(v1, "RT-1")) mock_reply(c, 200, "{\"access_token\":\"AT-2\",\"expires_in\":3599}");
      else mock_reply(c, 400, "{\"error\":\"invalid_grant\",\"error_description\":\"Token has been revoked.\"}");
    } else {
      mock_reply(c, 400, "{\"error\":\"unsupported_grant_type\"}");
    }
  } else if (!strncmp(req, "GET /api ", 9)) {
    m->api_calls++;
    if (hdr_get(req, "Authorization", v1, sizeof v1) && !strcmp(v1, "Bearer AT-2")) mock_reply(c, 200, "{\"ok\":true}");
    else mock_reply(c, 401, "{\"error\":{\"code\":\"InvalidAuthenticationToken\",\"message\":\"expired\"}}");
  } else {
    fake_cloud(m, c, req, body, have);
  }
  fm_free(body);
  oa_sock_close(c);
}

static int mock_thread(void *u) {
  Mock *m = (Mock *)u;
  while (!m->stop) {
    if (oa_sock_wait(&m->ls, 1, 50) != 0) continue;
    FmOaSock c = oa_sock_accept(m->ls);
    if (c != OA_SOCK_NONE) mock_handle(m, c);
  }
  return 0;
}

/* ---- the browser side ------------------------------------------------------------------ */

static char g_seen_url[8192];
static volatile int g_seen;
static volatile int *g_cancel_on_open;

static bool fake_browser(const char *url, void *user) {
  FM_UNUSED(user);
  fm_strlcpy(g_seen_url, url, sizeof g_seen_url);
  if (g_cancel_on_open) *g_cancel_on_open = 1;
  SDL_MemoryBarrierRelease();
  g_seen = 1;
  return true;
}

typedef struct LoginRun {
  FmOAuthCfg cfg;
  FmOAuthTok tok;
  FmErr e;
  char err[256];
  volatile int cancel;
} LoginRun;

static int login_thread(void *u) {
  LoginRun *l = (LoginRun *)u;
  l->e = oauth_login(&l->cfg, &l->tok, l->err, sizeof l->err, &l->cancel);
  return 0;
}

static bool wait_seen(void) {
  for (int i = 0; i < 500 && !g_seen; i++) SDL_Delay(10);
  SDL_MemoryBarrierAcquire();
  return g_seen != 0;
}

/* one request to the loopback listener as a browser would send it; the reply's first 512 bytes */
static int browse(int port, const char *target, char *reply, size_t cap) {
  FmOaSock c = oa_sock_connect(port);
  if (c == OA_SOCK_NONE) return -1;
  char req[1024];
  fm_snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUser-Agent: test\r\n\r\n", target, port);
  oa_sock_send(c, req, (int)strlen(req));
  size_t n = 0;
  for (int i = 0; i < 50 && n + 1 < cap; i++) {
    int r = oa_sock_recv(c, reply + n, (int)(cap - 1 - n), 100);
    if (r == 0) break;
    if (r > 0) n += (size_t)r;
  }
  reply[n] = 0;
  oa_sock_close(c);
  return atoi(reply + (n > 9 ? 9 : n));
}

static void test_loopback(void) {
  if (!net_available()) {
    printf("  (skip loopback flow: %s)\n", net_backend());
    return;
  }
  Mock m;
  memset(&m, 0, sizeof m);
  m.ls = oa_sock_listen(false, &m.port);
  TEST_CHECK(m.ls != OA_SOCK_NONE && m.port > 0);
  if (m.ls == OA_SOCK_NONE) return;
  SDL_Thread *mt = fm_thread_create(mock_thread, "oauth-mock", &m);
  char auth[128], token[128];
  fm_snprintf(auth, sizeof auth, "http://127.0.0.1:%d/authorize?prompt=x", m.port);
  fm_snprintf(token, sizeof token, "http://127.0.0.1:%d/token", m.port);

  LoginRun *l = (LoginRun *)fm_calloc(1, sizeof *l);
  l->cfg.auth_url = auth;
  l->cfg.token_url = token;
  l->cfg.client_id = "test-client";
  l->cfg.scope = "files.read offline";
  l->cfg.extra = "access_type=offline";
  g_oauth_browser = fake_browser;
  g_seen = 0;
  g_cancel_on_open = NULL;
  SDL_Thread *lt = fm_thread_create(login_thread, "oauth-login", l);
  TEST_CHECK(wait_seen());
  /* the sign-in URL the app opened */
  const char *u = g_seen_url;
  char redir[128], st[64], v[256];
  TEST_CHECK(!strncmp(u, auth, strlen(auth)) && u[strlen(auth)] == '&');
  const char *q = strchr(u, '?') + 1;
  TEST_CHECK(form_get(q, "response_type", v, sizeof v) && !strcmp(v, "code"));
  TEST_CHECK(form_get(q, "client_id", v, sizeof v) && !strcmp(v, "test-client"));
  TEST_CHECK(form_get(q, "scope", v, sizeof v) && !strcmp(v, "files.read offline"));
  TEST_CHECK(strstr(q, "scope=files.read%20offline") != NULL);
  TEST_CHECK(form_get(q, "code_challenge_method", v, sizeof v) && !strcmp(v, "S256"));
  TEST_CHECK(form_get(q, "access_type", v, sizeof v) && !strcmp(v, "offline"));
  TEST_CHECK(form_get(q, "code_challenge", m.challenge, sizeof m.challenge) && strlen(m.challenge) == 43);
  TEST_CHECK(form_get(q, "state", st, sizeof st) && strlen(st) >= 20);
  TEST_CHECK(form_get(q, "redirect_uri", redir, sizeof redir) && !strncmp(redir, "http://127.0.0.1:", 17));
  int port = atoi(redir + 17);
  TEST_CHECK(port > 0 && port != m.port);
  char reply[2048], target[256];
  TEST_CHECK(browse(port, "/favicon.ico", reply, sizeof reply) == 404);          /* ignored */
  TEST_CHECK(browse(port, "/?code=evil&state=wrong", reply, sizeof reply) == 400);   /* not ours: ignored */
  fm_snprintf(target, sizeof target, "/?code=c0de-42&state=%s&scope=x", st);
  TEST_CHECK(browse(port, target, reply, sizeof reply) == 200);
  TEST_CHECK(strstr(reply, "close this window") != NULL);
  SDL_WaitThread(lt, NULL);
  TEST_CHECK(l->e == FM_OK);
  if (l->e != FM_OK) printf("  login: %s\n", l->err);
  TEST_CHECK(!strcmp(l->tok.access, "AT-1") && !strcmp(l->tok.refresh, "RT-1"));
  TEST_CHECK(l->tok.expires > plat_time_unix() + 3000);
  TEST_CHECK(m.token_calls == 1);

  /* refresh */
  char err[256];
  FmOAuthTok t = l->tok;
  TEST_CHECK(oauth_refresh(&l->cfg, &t, err, sizeof err, NULL) == FM_OK);
  TEST_CHECK(!strcmp(t.access, "AT-2") && !strcmp(t.refresh, "RT-1"));

  /* an adapter call: the service says 401 -> refresh -> the same call again */
  FmCloudAcct *a = (FmCloudAcct *)fm_calloc(1, sizeof *a);
  t = l->tok;                                          /* AT-1, which the API no longer takes */
  oauth_save(&t, a->session, sizeof a->session);
  OaSess s;
  char url[128];
  fm_snprintf(url, sizeof url, "http://127.0.0.1:%d/api", m.port);
  TEST_CHECK(oa_open(&s, a, &l->cfg, "Mock", err, sizeof err, NULL) == FM_OK && !a->session_changed);
  FmNetResp r;
  TEST_CHECK(oa_send(&s, "GET", url, NULL, NULL, NULL, &r) == FM_OK && r.status == 200);
  net_resp_free(&r);
  TEST_CHECK(m.api_calls == 2 && a->session_changed && strstr(a->session, " AT-2"));
  /* a stale token is refreshed before the call */
  t.expires = plat_time_unix() - 10;
  oauth_save(&t, a->session, sizeof a->session);
  a->session_changed = false;
  TEST_CHECK(oa_open(&s, a, &l->cfg, "Mock", err, sizeof err, NULL) == FM_OK && a->session_changed);
  TEST_CHECK(!strcmp(s.tok.access, "AT-2"));
  /* a revoked refresh token: signed out */
  fm_strlcpy(t.refresh, "RT-revoked", sizeof t.refresh);
  oauth_save(&t, a->session, sizeof a->session);
  TEST_CHECK(oa_open(&s, a, &l->cfg, "Mock", err, sizeof err, NULL) == FM_ERR_PASSWORD);
  TEST_CHECK(strstr(err, "Signed out") != NULL);
  a->session[0] = 0;
  TEST_CHECK(oa_open(&s, a, &l->cfg, "Mock", err, sizeof err, NULL) == FM_ERR_PASSWORD);
  fm_free(a);

  /* cancel while waiting for the browser */
  memset(&l->tok, 0, sizeof l->tok);
  l->cancel = 0;
  g_seen = 0;
  g_cancel_on_open = &l->cancel;
  u64 t0 = plat_now_ms();
  login_thread(l);
  TEST_CHECK(l->e == FM_ERR_CANCEL && plat_now_ms() - t0 < 2000);
  g_cancel_on_open = NULL;

  /* the user says no in the browser */
  g_seen = 0;
  l->cancel = 0;
  lt = fm_thread_create(login_thread, "oauth-login", l);
  TEST_CHECK(wait_seen());
  q = strchr(g_seen_url, '?') + 1;
  form_get(q, "redirect_uri", redir, sizeof redir);
  form_get(q, "state", st, sizeof st);
  fm_snprintf(target, sizeof target, "/?error=access_denied&state=%s", st);
  TEST_CHECK(browse(atoi(redir + 17), target, reply, sizeof reply) == 200);
  SDL_WaitThread(lt, NULL);
  TEST_CHECK(l->e == FM_ERR_CANCEL && strstr(l->err, "declined"));

  /* Microsoft-style "localhost" redirect */
  g_seen = 0;
  l->cfg.redirect_host = "localhost";
  lt = fm_thread_create(login_thread, "oauth-login", l);
  TEST_CHECK(wait_seen());
  q = strchr(g_seen_url, '?') + 1;
  form_get(q, "redirect_uri", redir, sizeof redir);
  TEST_CHECK(!strncmp(redir, "http://localhost:", 17));
  l->cancel = 1;
  SDL_WaitThread(lt, NULL);
  TEST_CHECK(l->e == FM_ERR_CANCEL);

  g_oauth_browser = NULL;
  fm_free(l);
  m.stop = 1;
  SDL_WaitThread(mt, NULL);
  oa_sock_close(m.ls);
}

/* ---- adapters ---------------------------------------------------------------------------- */

static void test_gdrive(void) {
  const char *p1 =
      "{\"nextPageToken\":\"~!!~AI9FV7Q\",\"files\":["
      "{\"id\":\"1AbC_folder\",\"name\":\"Photos\",\"mimeType\":\"application/vnd.google-apps.folder\","
      "\"modifiedTime\":\"2026-01-02T03:04:05.123Z\"},"
      "{\"id\":\"1file-x\",\"name\":\"a \\u00e9.jpg\",\"mimeType\":\"image/jpeg\",\"size\":\"123456\","
      "\"modifiedTime\":\"2026-10-09T10:00:00.000Z\",\"md5Checksum\":\"0123456789abcdef0123456789abcdef\"},"
      "{\"id\":\"1doc\",\"name\":\"Report\",\"mimeType\":\"application/vnd.google-apps.document\"},"
      "{\"id\":\"1form\",\"name\":\"Survey\",\"mimeType\":\"application/vnd.google-apps.form\"}]}";
  FmCloudList l;
  memset(&l, 0, sizeof l);
  char next[256];
  TEST_CHECK(gdrive_parse_list(p1, strlen(p1), &l, next, sizeof next) == FM_OK);
  TEST_CHECK(!strcmp(next, "~!!~AI9FV7Q"));
  TEST_CHECK(l.count == 3);
  if (l.count == 3) {
    TEST_CHECK(l.items[0].dir && !strcmp(l.items[0].id, "1AbC_folder") && !strcmp(l.items[0].name, "Photos"));
    TEST_CHECK(l.items[0].mtime == 1767323045);
    TEST_CHECK(!l.items[1].dir && l.items[1].size == 123456 && !strcmp(l.items[1].name, "a \xC3\xA9.jpg"));
    TEST_CHECK(!strcmp(l.items[1].hash, "md5:0123456789abcdef0123456789abcdef"));
    TEST_CHECK(!strcmp(l.items[1].mime, "image/jpeg"));
    TEST_CHECK(!strcmp(l.items[2].name, "Report.docx") &&
               !strcmp(l.items[2].mime, "application/vnd.google-apps.document"));
  }
  const char *p2 = "{\"files\":[{\"id\":\"z\",\"name\":\"last.txt\",\"mimeType\":\"text/plain\",\"size\":\"0\"}]}";
  TEST_CHECK(gdrive_parse_list(p2, strlen(p2), &l, next, sizeof next) == FM_OK && !next[0] && l.count == 4);
  cloud_list_free(&l);
  TEST_CHECK(gdrive_parse_list("{\"error\":{}}", 12, &l, next, sizeof next) == FM_ERR_FORMAT && l.error[0]);
  cloud_list_free(&l);
  u64 used = 0, total = 0;
  const char *q1 = "{\"storageQuota\":{\"limit\":\"16106127360\",\"usage\":\"5368709120\",\"usageInDrive\":\"1\"}}";
  TEST_CHECK(gdrive_parse_quota(q1, strlen(q1), &used, &total) && used == 5368709120ull && total == 16106127360ull);
  const char *q2 = "{\"storageQuota\":{\"usage\":\"42\"}}";
  TEST_CHECK(gdrive_parse_quota(q2, strlen(q2), &used, &total) && used == 42 && total == 0);
  FmCloudEntry e;
  const char *up = "{\"id\":\"1new\",\"name\":\"up.bin\",\"mimeType\":\"application/octet-stream\",\"size\":\"9\"}";
  TEST_CHECK(gdrive_parse_entry(up, strlen(up), &e) && !strcmp(e.id, "1new") && e.size == 9);
}

static void test_dropbox(void) {
  char p[256];
  dropbox_api_path("", p, sizeof p);
  TEST_CHECK(!strcmp(p, ""));
  dropbox_api_path("photos/2026/", p, sizeof p);
  TEST_CHECK(!strcmp(p, "/photos/2026"));
  dropbox_api_path("a/b.jpg", p, sizeof p);
  TEST_CHECK(!strcmp(p, "/a/b.jpg"));
  const char *j =
      "{\"entries\":[{\".tag\":\"folder\",\"name\":\"Photos\",\"path_lower\":\"/photos\",\"path_display\":\"/Photos\","
      "\"id\":\"id:a4ayc_80_OEAAAAAAAAAXw\"},"
      "{\".tag\":\"file\",\"name\":\"Pr\\u00e9sentation.pdf\",\"path_display\":\"/Photos/Pr\\u00e9sentation.pdf\","
      "\"client_modified\":\"2015-05-12T15:50:38Z\",\"server_modified\":\"2015-05-12T15:50:38Z\",\"size\":7212,"
      "\"content_hash\":\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\"},"
      "{\".tag\":\"deleted\",\"name\":\"old\",\"path_display\":\"/old\"}],"
      "\"cursor\":\"ZtkX9_EHj3x7PMkVuFIhwKYXEpwpLwyxp9vMKomUhllil9q7eWiAu\",\"has_more\":true}";
  FmCloudList l;
  memset(&l, 0, sizeof l);
  char cur[256];
  bool more = false;
  TEST_CHECK(dropbox_parse_list(j, strlen(j), &l, cur, sizeof cur, &more) == FM_OK);
  TEST_CHECK(more && !strncmp(cur, "ZtkX9_", 6));
  TEST_CHECK(l.count == 2);
  if (l.count == 2) {
    TEST_CHECK(l.items[0].dir && !strcmp(l.items[0].id, "Photos/"));
    TEST_CHECK(!l.items[1].dir && !strcmp(l.items[1].id, "Photos/Pr\xC3\xA9sentation.pdf") && l.items[1].size == 7212);
    TEST_CHECK(l.items[1].mtime == 1431445838 && !strncmp(l.items[1].hash, "dbx:e3b0", 8));
  }
  const char *j2 = "{\"entries\":[],\"cursor\":\"c2\",\"has_more\":false}";
  TEST_CHECK(dropbox_parse_list(j2, strlen(j2), &l, cur, sizeof cur, &more) == FM_OK && !more && l.count == 2);
  cloud_list_free(&l);
  u64 used = 0, total = 0;
  const char *q = "{\"used\":314159265,\"allocation\":{\".tag\":\"individual\",\"allocated\":2147483648}}";
  TEST_CHECK(dropbox_parse_quota(q, strlen(q), &used, &total) && used == 314159265 && total == 2147483648ull);
}

static void test_onedrive(void) {
  const char *j =
      "{\"@odata.context\":\"x\",\"value\":["
      "{\"id\":\"D4648F06C91D9D3D!54927\",\"name\":\"Docs\",\"folder\":{\"childCount\":3},"
      "\"lastModifiedDateTime\":\"2026-03-04T05:06:07Z\"},"
      "{\"id\":\"D4648F06C91D9D3D!1\",\"name\":\"song.mp3\",\"size\":4096,\"file\":{\"mimeType\":\"audio/mpeg\","
      "\"hashes\":{\"sha1Hash\":\"DA39A3EE5E6B4B0D3255BFEF95601890AFD80709\",\"quickXorHash\":\"AAAA\"}},"
      "\"fileSystemInfo\":{\"lastModifiedDateTime\":\"2020-01-01T00:00:00+01:00\"}},"
      "{\"id\":\"01BYE5RZ\",\"name\":\"Notebook\",\"package\":{\"type\":\"oneNote\"}}],"
      "\"@odata.nextLink\":\"https://graph.microsoft.com/v1.0/me/drive/root/children?$skiptoken=abc\"}";
  FmCloudList l;
  memset(&l, 0, sizeof l);
  char next[512];
  TEST_CHECK(onedrive_parse_list(j, strlen(j), &l, next, sizeof next) == FM_OK);
  TEST_CHECK(!strcmp(next, "https://graph.microsoft.com/v1.0/me/drive/root/children?$skiptoken=abc"));
  TEST_CHECK(l.count == 3);
  if (l.count == 3) {
    TEST_CHECK(l.items[0].dir && !strcmp(l.items[0].id, "D4648F06C91D9D3D!54927"));
    TEST_CHECK(!l.items[1].dir && l.items[1].size == 4096 && !strcmp(l.items[1].mime, "audio/mpeg"));
    TEST_CHECK(!strcmp(l.items[1].hash, "sha1:DA39A3EE5E6B4B0D3255BFEF95601890AFD80709"));
    TEST_CHECK(l.items[1].mtime == 1577833200);
    TEST_CHECK(l.items[2].dir);
  }
  const char *evil = "{\"value\":[],\"@odata.nextLink\":\"https://evil.example/steal\"}";
  TEST_CHECK(onedrive_parse_list(evil, strlen(evil), &l, next, sizeof next) == FM_OK && !next[0]);
  cloud_list_free(&l);
  u64 used = 0, total = 0;
  const char *q = "{\"quota\":{\"deleted\":0,\"remaining\":1,\"state\":\"normal\",\"total\":5368709120,\"used\":1024}}";
  TEST_CHECK(onedrive_parse_quota(q, strlen(q), &used, &total) && used == 1024 && total == 5368709120ull);
  const char *r1 = "{\"expirationDateTime\":\"x\",\"nextExpectedRanges\":[\"26-\"]}";
  TEST_CHECK(onedrive_next_range(r1, strlen(r1)) == 26);
  const char *r2 = "{\"nextExpectedRanges\":[\"12345-55232\",\"77829-99999\"]}";
  TEST_CHECK(onedrive_next_range(r2, strlen(r2)) == 12345);
  TEST_CHECK(onedrive_next_range("{}", 2) == -1);
}

static void test_errors(void) {
  char m[256];
  const char *g = "{\"error\":{\"code\":403,\"message\":\"The user's Drive storage quota has been exceeded.\","
                  "\"errors\":[{\"reason\":\"storageQuotaExceeded\"}]}}";
  TEST_CHECK(oa_error_text("Google Drive", 403, g, strlen(g), m, sizeof m) == FM_ERR_FULL);
  const char *g2 = "{\"error\":{\"code\":403,\"message\":\"Google Drive API has not been used in project 1 before\","
                   "\"errors\":[{\"reason\":\"accessNotConfigured\"}]}}";
  TEST_CHECK(oa_error_text("Google Drive", 403, g2, strlen(g2), m, sizeof m) == FM_ERR_ACCESS && strstr(m, "API"));
  const char *g3 = "{\"error\":{\"code\":403,\"message\":\"Insufficient Permission\",\"errors\":[{\"reason\":\"insufficientPermissions\"}]}}";
  TEST_CHECK(oa_error_text("Google Drive", 403, g3, strlen(g3), m, sizeof m) == FM_ERR_ACCESS &&
             strstr(m, "Insufficient Permission"));
  const char *d1 = "{\"error_summary\":\"path/not_found/..\",\"error\":{\".tag\":\"path\"}}";
  TEST_CHECK(oa_error_text("Dropbox", 409, d1, strlen(d1), m, sizeof m) == FM_ERR_NOT_FOUND);
  const char *d2 = "{\"error_summary\":\"path/conflict/folder/...\",\"error\":{}}";
  TEST_CHECK(oa_error_text("Dropbox", 409, d2, strlen(d2), m, sizeof m) == FM_ERR_EXISTS);
  const char *d3 = "{\"error_summary\":\"path/insufficient_space/..\",\"error\":{}}";
  TEST_CHECK(oa_error_text("Dropbox", 409, d3, strlen(d3), m, sizeof m) == FM_ERR_FULL);
  const char *d4 = "{\"error_summary\":\"path/malformed_path/\",\"error\":{}}";
  TEST_CHECK(oa_error_text("Dropbox", 409, d4, strlen(d4), m, sizeof m) == FM_ERR_IO && strstr(m, "malformed"));
  const char *d5 = "Error in call to API function \"files/list_folder\": request body: unknown field 'x'";
  TEST_CHECK(oa_error_text("Dropbox", 400, d5, strlen(d5), m, sizeof m) == FM_ERR_IO && strstr(m, "unknown field"));
  const char *o1 = "{\"error\":{\"code\":\"itemNotFound\",\"message\":\"Item does not exist\"}}";
  TEST_CHECK(oa_error_text("OneDrive", 404, o1, strlen(o1), m, sizeof m) == FM_ERR_NOT_FOUND);
  const char *o2 = "{\"error\":{\"code\":\"nameAlreadyExists\",\"message\":\"exists\"}}";
  TEST_CHECK(oa_error_text("OneDrive", 409, o2, strlen(o2), m, sizeof m) == FM_ERR_EXISTS);
  TEST_CHECK(oa_error_text("OneDrive", 401, NULL, 0, m, sizeof m) == FM_ERR_PASSWORD && strstr(m, "Signed out"));
  TEST_CHECK(oa_error_text("OneDrive", 507, NULL, 0, m, sizeof m) == FM_ERR_FULL);
  TEST_CHECK(oa_error_text("OneDrive", 502, NULL, 0, m, sizeof m) == FM_ERR_IO);

  oa_json_quote("a\"b\\c\n\xC3\xA9\xF0\x9F\x98\x80", m, sizeof m, true);
  TEST_CHECK(!strcmp(m, "\"a\\\"b\\\\c\\u000a\\u00e9\\ud83d\\ude00\""));
  oa_json_quote("\xC3\xA9", m, sizeof m, false);
  TEST_CHECK(!strcmp(m, "\"\xC3\xA9\""));

  OaProg pg = { NULL, NULL, 100, 10, 1000 };
  TEST_CHECK(oa_prog(&pg, 5, 10));

  const FmCloud *cs[3] = { &g_cloud_gdrive, &g_cloud_dropbox, &g_cloud_onedrive };
  for (int i = 0; i < 3; i++) {
    const FmCloud *c = cs[i];
    TEST_CHECK((c->flags & CLOUD_OAUTH) && (c->flags & CLOUD_UPLOAD) && (c->flags & CLOUD_STREAM));
    TEST_CHECK(c->login && c->list && c->download && c->upload && c->mkdir && c->remove && c->rename && c->move &&
               c->quota && c->stream_url && !c->open_link && c->about && strlen(c->about) > 40);
  }
  /* no session: every call says "sign in" without touching the network */
  FmCloudAcct a;
  memset(&a, 0, sizeof a);
  FmCloudList l;
  memset(&l, 0, sizeof l);
  for (int i = 0; i < 3; i++) {
    TEST_CHECK(cs[i]->list(&a, "", &l, NULL) == FM_ERR_PASSWORD && strstr(l.error, "Signed out"));
    char err[256];
    TEST_CHECK(cs[i]->login(&a, err, sizeof err, NULL) == FM_ERR_ACCESS && strstr(err, "client id"));
  }
  cloud_list_free(&l);
}

static bool bump_prog(void *u, u64 d, u64 t) {
  FM_UNUSED(t);
  *(u64 *)u = d;
  return true;
}

static void test_prog(void) {
  u64 last = 0;
  OaProg pg;
  pg.cb = bump_prog;
  pg.user = &last;
  pg.base = 8;
  pg.chunk = 4;
  pg.total = 20;
  TEST_CHECK(oa_prog(&pg, 2, 4) && last == 10);
  TEST_CHECK(oa_prog(&pg, 100, 300) && last == 10);       /* the reply's progress is not the upload's */
}

/* ---- the adapters end to end against the fake cloud -------------------------------------- */

static void make_file(const char *path, u64 size, u32 *crc) {
  FILE *f = fm_fopen(path, "wb");
  u8 buf[4096];
  *crc = 0;
  for (u64 i = 0; f && i < size;) {
    size_t n = (size_t)FM_MIN((u64)sizeof buf, size - i);
    for (size_t k = 0; k < n; k++) buf[k] = (u8)((i + k) * 7 + ((i + k) >> 11));
    fwrite(buf, 1, n, f);
    *crc = crc32_update(*crc, buf, n);
    i += n;
  }
  if (f) fclose(f);
}

static bool file_is(const char *path, const char *want) {
  char buf[256];
  FILE *f = fm_fopen(path, "rb");
  if (!f) return false;
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;
  return n == strlen(want) && !memcmp(buf, want, n);
}

static bool part_gone(const char *path) {
  char p[FM_PATH_MAX];
  fm_snprintf(p, sizeof p, "%s.part", path);
  return !plat_exists(p);
}

static bool max_prog(void *u, u64 d, u64 t) {
  u64 *m = (u64 *)u;
  if (d > m[0]) m[0] = d;
  m[1] = t;
  return true;
}

static void test_fake_cloud(const char *tmp) {
  if (!net_available()) return;
  Mock *m = (Mock *)fm_calloc(1, sizeof *m);
  m->ls = oa_sock_listen(false, &m->port);
  TEST_CHECK(m->ls != OA_SOCK_NONE);
  if (m->ls == OA_SOCK_NONE) { fm_free(m); return; }
  SDL_Thread *mt = fm_thread_create(mock_thread, "oauth-mock", m);
  fm_snprintf(g_oa_test_base, sizeof g_oa_test_base, "http://127.0.0.1:%d", m->port);

  FmCloudAcct *a = (FmCloudAcct *)fm_calloc(1, sizeof *a);
  FmOAuthTok t;
  memset(&t, 0, sizeof t);
  fm_strlcpy(t.access, "AT-T", sizeof t.access);
  fm_strlcpy(t.refresh, "RT-X", sizeof t.refresh);
  t.expires = plat_time_unix() + 3600;
  oauth_save(&t, a->session, sizeof a->session);
  fm_strlcpy(a->client_id, "cid", sizeof a->client_id);

  FmCloudList l;
  FmCloudEntry e, out;
  char err[256], p[FM_PATH_MAX], small[FM_PATH_MAX], big[FM_PATH_MAX];
  u32 big_crc, scrc;
  u64 prog[2];
  const u64 kBig = 12u << 20;
  fm_path_join(small, sizeof small, tmp, "small.txt");
  make_file(small, 0, &scrc);
  FILE *f = fm_fopen(small, "wb");
  if (f) { fputs("small body", f); fclose(f); }
  fm_path_join(big, sizeof big, tmp, "big.bin");
  make_file(big, kBig, &big_crc);

  /* Google Drive */
  const FmCloud *c = &g_cloud_gdrive;
  memset(&l, 0, sizeof l);
  TEST_CHECK(c->list(a, "", &l, NULL) == FM_OK && l.count == 2);
  if (l.count == 2) TEST_CHECK(l.items[0].dir && !strcmp(l.items[1].id, "1f") && l.items[1].size == 5);
  if (l.error[0]) printf("  gdrive list: %s\n", l.error);
  cloud_list_free(&l);
  memset(&e, 0, sizeof e);
  fm_strlcpy(e.id, "1f", sizeof e.id);
  fm_path_join(p, sizeof p, tmp, "d1.txt");
  TEST_CHECK(c->download(a, &e, p, NULL, NULL, err, sizeof err, NULL) == FM_OK && file_is(p, "hello drive") && part_gone(p));
  fm_strlcpy(e.id, "1doc", sizeof e.id);
  fm_strlcpy(e.mime, "application/vnd.google-apps.document", sizeof e.mime);
  fm_path_join(p, sizeof p, tmp, "d2.docx");
  TEST_CHECK(c->download(a, &e, p, NULL, NULL, err, sizeof err, NULL) == FM_OK && file_is(p, "DOCX"));
  fm_strlcpy(e.id, "gone", sizeof e.id);
  e.mime[0] = 0;
  fm_path_join(p, sizeof p, tmp, "d3.txt");
  TEST_CHECK(c->download(a, &e, p, NULL, NULL, err, sizeof err, NULL) == FM_ERR_NOT_FOUND && !plat_exists(p) &&
             part_gone(p));
  TEST_CHECK(c->upload(a, "", small, "small.txt", &out, NULL, NULL, err, sizeof err, NULL) == FM_OK &&
             !strcmp(out.id, "1s"));
  m->fail_once = 1;                                     /* one 503: ask the session, go on */
  prog[0] = prog[1] = 0;
  FmErr ue = c->upload(a, "", big, "big.bin", &out, max_prog, prog, err, sizeof err, NULL);
  TEST_CHECK(ue == FM_OK && m->up_got == kBig && m->up_crc == big_crc && out.size == kBig);
  if (ue != FM_OK) printf("  gdrive upload: %s\n", err);
  TEST_CHECK(prog[0] == kBig && prog[1] == kBig && !m->fail_once);
  char url[512], hdr[512];
  fm_strlcpy(e.id, "1f", sizeof e.id);
  TEST_CHECK(c->stream_url(a, &e, url, sizeof url, hdr, sizeof hdr, err, sizeof err, NULL) == FM_OK);
  TEST_CHECK(strstr(url, "/drive/v3/files/1f?alt=media") && !strcmp(hdr, "Authorization: Bearer AT-T\r\n"));
  TEST_CHECK(!a->session_changed);

  /* OneDrive */
  c = &g_cloud_onedrive;
  memset(&l, 0, sizeof l);
  TEST_CHECK(c->list(a, "", &l, NULL) == FM_OK && l.count == 2);
  if (l.error[0]) printf("  onedrive list: %s\n", l.error);
  if (l.count == 2) TEST_CHECK(l.items[0].dir && !strcmp(l.items[1].id, "F!2"));
  cloud_list_free(&l);
  memset(&e, 0, sizeof e);
  fm_strlcpy(e.id, "F!2", sizeof e.id);
  fm_path_join(p, sizeof p, tmp, "o1.txt");
  TEST_CHECK(c->download(a, &e, p, NULL, NULL, err, sizeof err, NULL) == FM_OK && file_is(p, "hello onedrive"));
  TEST_CHECK(c->upload(a, "F!1", small, "sm\xC3\xA9.txt", &out, NULL, NULL, err, sizeof err, NULL) == FM_OK &&
             !strcmp(out.id, "F!s") && !strcmp(out.name, "sm\xC3\xA9.txt"));
  prog[0] = prog[1] = 0;
  ue = c->upload(a, "", big, "big.bin", &out, max_prog, prog, err, sizeof err, NULL);
  TEST_CHECK(ue == FM_OK && m->up_got == kBig && m->up_crc == big_crc && !strcmp(out.id, "F!big"));
  if (ue != FM_OK) printf("  onedrive upload: %s\n", err);
  TEST_CHECK(prog[0] == kBig && m->auth_leaks == 0);

  /* Dropbox */
  c = &g_cloud_dropbox;
  memset(&l, 0, sizeof l);
  TEST_CHECK(c->list(a, "", &l, NULL) == FM_OK && l.count == 2);
  if (l.error[0]) printf("  dropbox list: %s\n", l.error);
  if (l.count == 2) TEST_CHECK(!strcmp(l.items[0].id, "A/") && l.items[0].dir && !strcmp(l.items[1].id, "b.txt"));
  cloud_list_free(&l);
  memset(&e, 0, sizeof e);
  fm_strlcpy(e.id, "Photos/Pr\xC3\xA9s.pdf", sizeof e.id);
  fm_path_join(p, sizeof p, tmp, "x1.pdf");
  TEST_CHECK(c->download(a, &e, p, NULL, NULL, err, sizeof err, NULL) == FM_OK && file_is(p, "hello dropbox"));
  TEST_CHECK(!strcmp(m->last_arg, "{\"path\":\"/Photos/Pr\\u00e9s.pdf\"}"));
  TEST_CHECK(c->upload(a, "A/", small, "up.txt", &out, NULL, NULL, err, sizeof err, NULL) == FM_OK &&
             !strcmp(out.id, "A/up.txt") && out.size == 10);
  TEST_CHECK(strstr(m->last_arg, "\"path\":\"/A/up.txt\"") && strstr(m->last_arg, "\"mode\":\"overwrite\""));
  TEST_CHECK(c->remove(a, &e, err, sizeof err, NULL) == FM_ERR_NOT_FOUND);
  u64 used = 0, total = 0;
  TEST_CHECK(c->quota(a, &used, &total, err, sizeof err, NULL) == FM_OK && used == 10 && total == 100);

  g_oa_test_base[0] = 0;
  fm_free(a);
  m->stop = 1;
  SDL_WaitThread(mt, NULL);
  oa_sock_close(m->ls);
  fm_free(m);
}

int test_oauth(const char *tmp) {
  int before = g_test_fail;
  test_pkce();
  test_redirect();
  test_tokens();
  test_gdrive();
  test_dropbox();
  test_onedrive();
  test_errors();
  test_prog();
  test_loopback();
  test_fake_cloud(tmp);
  return g_test_fail - before;
}
