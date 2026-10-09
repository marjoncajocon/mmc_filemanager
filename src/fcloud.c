/* fcloud.c -- the cloud registry, the saved accounts and the adapters' helpers.
**
** Design decisions:
**   - The six adapters sit in one table; a service still being written has
**     NULL functions and the UI shows it as "Coming soon". A seventh,
**     in-memory adapter (fcloud_mock.c) joins the table only for the self
**     test and --demo-cloud.
**   - Accounts live in PLACE_CONFIG/cloud.txt, one "[account]" block of
**     key=value lines each, written to a temporary file and renamed. The
**     secret (password, app password, S3 secret key), the OAuth client
**     secret and the session are protected for the user: DPAPI on Windows
**     (crypt32.dll loaded at run time, nothing linked; "d:" values),
**     elsewhere and as the fallback a keyed XOR stream ("o:" values). The
**     XOR is obfuscation, NOT encryption: it keeps secrets out of greps,
**     screenshots of the file and naive backups, but anyone who can read
**     the config folder and this source can undo it. A value that no longer
**     unprotects (another user, another PC) loads as empty, which the UI
**     reads as "signed out".
**   - Every account gets a serial number for the session. Panels, history
**     and jobs refer to accounts by serial, so removing one never makes a
**     panel point at its neighbour.
**   - Workers never write the live account: they take a snapshot under the
**     lock, call the adapter on the copy and hand back a changed session
**     (refreshed tokens) with cloud_acct_writeback; the main thread saves in
**     cloud_acct_pump. So two transfers on one account each see one call at
**     a time on their own copy, and the UI never waits for the network.
**   - Temporary accounts (a public link, the demo) are listed like the
**     others but never saved.
*/
#include "fcloud.h"
#include "fcloud_app.h"
#include "fapp.h"
#include "fsdl.h"
#ifdef FM_WIN
#  include "fwin.h"
#endif

extern const FmCloud g_cloud_gdrive, g_cloud_dropbox, g_cloud_onedrive, g_cloud_mega, g_cloud_webdav, g_cloud_s3;
extern const FmCloud g_cloud_mock;

/* ---- registry ------------------------------------------------------------- */

static const FmCloud *const kClouds[] = {
  &g_cloud_gdrive, &g_cloud_dropbox, &g_cloud_onedrive, &g_cloud_mega, &g_cloud_webdav, &g_cloud_s3,
};
static bool g_mock_on;

void cloud_mock_enable(bool on) { g_mock_on = on; }

int cloud_count(void) { return FM_COUNT(kClouds) + (g_mock_on ? 1 : 0); }

const FmCloud *cloud_at(int i) {
  if (i >= 0 && i < FM_COUNT(kClouds)) return kClouds[i];
  if (i == FM_COUNT(kClouds) && g_mock_on) return &g_cloud_mock;
  return NULL;
}

const FmCloud *cloud_find(const char *key) {
  if (!key) return NULL;
  for (int i = 0; i < cloud_count(); i++)
    if (!strcmp(cloud_at(i)->key, key)) return cloud_at(i);
  /* the mock stays findable for accounts made while it was on */
  if (!strcmp(key, g_cloud_mock.key)) return &g_cloud_mock;
  return NULL;
}

bool cloud_available(const FmCloud *c) { return c && c->list != NULL; }

/* ---- base64 --------------------------------------------------------------- */

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const u8 *p, size_t n, char *out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < n && o + 5 < cap; i += 3) {
    u32 v = (u32)p[i] << 16 | (i + 1 < n ? (u32)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
    out[o++] = kB64[(v >> 18) & 63];
    out[o++] = kB64[(v >> 12) & 63];
    out[o++] = i + 1 < n ? kB64[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? kB64[v & 63] : '=';
  }
  out[o] = 0;
}

static int b64_val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

/* Returns the decoded length, or -1 on a bad character or a full buffer. */
static int b64_decode(const char *s, u8 *out, size_t cap) {
  u32 acc = 0;
  int bits = 0;
  size_t o = 0;
  for (; *s && *s != '='; s++) {
    int v = b64_val(*s);
    if (v < 0) return -1;
    acc = acc << 6 | (u32)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= cap) return -1;
      out[o++] = (u8)(acc >> bits);
    }
  }
  return (int)o;
}

/* ---- protecting secrets --------------------------------------------------- */

#define SECRET_MAX (sizeof(((FmCloudAcct *)0)->session))   /* the largest protected field */

/* Keystream for the obfuscation: xorshift64* seeded from a fixed salt, the
** user's home folder and an 8-byte per-value nonce. Not encryption. */
static void obf_xor(u8 *p, size_t n, const u8 nonce[8]) {
  u64 h = 1469598103934665603ull;
  const char *salt = "mmcfm-cloud-v1";
  char home[FM_PATH_MAX] = "";
  plat_place(PLACE_HOME, home, sizeof home);
  for (const char *s = salt; *s; s++) { h ^= (u8)*s; h *= 1099511628211ull; }
  for (const char *s = home; *s; s++) { h ^= (u8)*s; h *= 1099511628211ull; }
  for (int i = 0; i < 8; i++) { h ^= nonce[i]; h *= 1099511628211ull; }
  if (!h) h = 0x9E3779B97F4A7C15ull;
  for (size_t i = 0; i < n; i++) {
    h ^= h >> 12; h ^= h << 25; h ^= h >> 27;
    p[i] ^= (u8)((h * 2685821657736338717ull) >> 56);
  }
}

#ifdef FM_WIN
typedef struct CBlob { DWORD cb; BYTE *pb; } CBlob;
typedef BOOL (WINAPI *ProtectFn)(CBlob *, LPCWSTR, CBlob *, void *, void *, DWORD, CBlob *);
typedef BOOL (WINAPI *UnprotectFn)(CBlob *, LPWSTR *, CBlob *, void *, void *, DWORD, CBlob *);
static ProtectFn g_protect;
static UnprotectFn g_unprotect;
static bool g_dpapi_tried;
static int g_force_obf;                    /* self test: the fallback path */

static bool dpapi_load(void) {
  if (!g_dpapi_tried) {
    g_dpapi_tried = true;
    HMODULE m = LoadLibraryW(L"crypt32.dll");
    if (m) {
      g_protect = (ProtectFn)(void *)GetProcAddress(m, "CryptProtectData");
      g_unprotect = (UnprotectFn)(void *)GetProcAddress(m, "CryptUnprotectData");
    }
    if (!g_protect || !g_unprotect) g_protect = NULL, g_unprotect = NULL;
  }
  return g_protect != NULL && !g_force_obf;
}

static CBlob entropy(void) {
  static BYTE e[] = "mmcfm cloud accounts";
  CBlob b = { (DWORD)(sizeof e - 1), e };
  return b;
}
#endif

void cloud_secret_force_obfuscation(bool on) {
#ifdef FM_WIN
  g_force_obf = on;
#else
  FM_UNUSED(on);
#endif
}

void cloud_protect(const char *plain, char *out, size_t cap) {
  out[0] = 0;
  size_t n = strlen(plain);
  if (n == 0) return;
#ifdef FM_WIN
  if (dpapi_load()) {
    CBlob in = { (DWORD)n, (BYTE *)plain }, ent = entropy(), res = { 0, NULL };
    if (g_protect(&in, L"mmcfm", &ent, NULL, NULL, 1 /* CRYPTPROTECT_UI_FORBIDDEN */, &res) && res.pb) {
      if (cap > 2 && ((size_t)res.cb + 2) / 3 * 4 + 3 <= cap) {
        fm_strlcpy(out, "d:", cap);
        b64_encode(res.pb, res.cb, out + 2, cap - 2);
      }
      LocalFree(res.pb);
      if (out[0]) return;
    }
  }
#endif
  u8 *buf = (u8 *)fm_alloc(n + 8);
  plat_random(buf, 8);
  memcpy(buf + 8, plain, n);
  obf_xor(buf + 8, n, buf);
  if ((n + 8 + 2) / 3 * 4 + 3 <= cap) {
    fm_strlcpy(out, "o:", cap);
    b64_encode(buf, n + 8, out + 2, cap - 2);
  }
  memset(buf, 0, n + 8);
  fm_free(buf);
}

bool cloud_unprotect(const char *val, char *out, size_t cap) {
  out[0] = 0;
  if (!val[0]) return true;
  bool dp = !strncmp(val, "d:", 2), ob = !strncmp(val, "o:", 2);
  if (!dp && !ob) return false;
  size_t blen = strlen(val) / 4 * 3 + 3;
  u8 *buf = (u8 *)fm_alloc(blen + 1);
  int n = b64_decode(val + 2, buf, blen);
  bool ok = false;
  if (n > 0 && ob && n >= 8) {
    obf_xor(buf + 8, (size_t)n - 8, buf);
    if ((size_t)n - 8 < cap) {
      memcpy(out, buf + 8, (size_t)n - 8);
      out[n - 8] = 0;
      ok = true;
    }
  }
#ifdef FM_WIN
  if (n > 0 && dp) {
    dpapi_load();
    if (g_unprotect) {
      CBlob in = { (DWORD)n, buf }, ent = entropy(), res = { 0, NULL };
      if (g_unprotect(&in, NULL, &ent, NULL, NULL, 1, &res) && res.pb) {
        if ((size_t)res.cb < cap) {
          memcpy(out, res.pb, res.cb);
          out[res.cb] = 0;
          ok = true;
        }
        memset(res.pb, 0, res.cb);
        LocalFree(res.pb);
      }
    }
  }
#endif
  memset(buf, 0, blen + 1);
  fm_free(buf);
  return ok;
}

/* ---- accounts --------------------------------------------------------------- */

#define ACCT_MAX 24

typedef struct Slot {
  FmCloudAcct a;
  int serial;
  bool temp;
} Slot;

static Slot *g_acct[ACCT_MAX];
static int g_nacct;
static int g_next_serial = 1;
static bool g_loaded, g_noload, g_dirty;
static SDL_mutex *g_mu;
static char g_store[FM_PATH_MAX];          /* "" = PLACE_CONFIG/cloud.txt */
static u32 g_stamp;                        /* bumped on every change: places rebuild */

u32 cloud_acct_stamp(void) { return g_stamp; }

void cloud_acct_lock(void) {
  if (!g_mu) g_mu = SDL_CreateMutex();
  SDL_LockMutex(g_mu);
}

void cloud_acct_unlock(void) { SDL_UnlockMutex(g_mu); }

static bool store_path(char *out, size_t cap) {
  if (g_store[0]) { fm_strlcpy(out, g_store, cap); return true; }
  char dir[FM_PATH_MAX];
  if (!plat_place(PLACE_CONFIG, dir, sizeof dir)) return false;
  return fm_path_join(out, cap, dir, "cloud.txt");
}

/* key=value with \\, \n and \r escaped */
static void put_line(FILE *f, const char *key, const char *val) {
  if (!val[0]) return;
  fputs(key, f);
  fputc('=', f);
  for (const char *s = val; *s; s++) {
    if (*s == '\\') fputs("\\\\", f);
    else if (*s == '\n') fputs("\\n", f);
    else if (*s == '\r') fputs("\\r", f);
    else fputc(*s, f);
  }
  fputc('\n', f);
}

static void unescape(char *s) {
  char *o = s;
  for (; *s; s++) {
    if (*s == '\\' && s[1]) {
      s++;
      *o++ = *s == 'n' ? '\n' : *s == 'r' ? '\r' : *s;
    } else {
      *o++ = *s;
    }
  }
  *o = 0;
}

static void put_secret(FILE *f, const char *key, const char *val) {
  if (!val[0]) return;
  char *enc = (char *)fm_alloc(SECRET_MAX * 2 + 64);
  cloud_protect(val, enc, SECRET_MAX * 2 + 64);
  put_line(f, key, enc);
  fm_free(enc);
}

static Slot *slot_new(void) {
  if (g_nacct >= ACCT_MAX) return NULL;
  Slot *s = (Slot *)fm_calloc(1, sizeof *s);
  s->serial = g_next_serial++;
  g_stamp++;
  g_acct[g_nacct++] = s;
  return s;
}

static void set_field(FmCloudAcct *a, const char *k, char *v) {
  unescape(v);
  char *plain = NULL;
  if (!strcmp(k, "secret") || !strcmp(k, "client_secret") || !strcmp(k, "session")) {
    plain = (char *)fm_alloc(SECRET_MAX + 1);
    if (!cloud_unprotect(v, plain, SECRET_MAX + 1)) plain[0] = 0;   /* another user / PC: signed out */
    v = plain;
  }
  if (!strcmp(k, "provider")) fm_strlcpy(a->provider, v, sizeof a->provider);
  else if (!strcmp(k, "label")) fm_strlcpy(a->label, v, sizeof a->label);
  else if (!strcmp(k, "user")) fm_strlcpy(a->user, v, sizeof a->user);
  else if (!strcmp(k, "server")) fm_strlcpy(a->server, v, sizeof a->server);
  else if (!strcmp(k, "client_id")) fm_strlcpy(a->client_id, v, sizeof a->client_id);
  else if (!strcmp(k, "secret")) fm_strlcpy(a->secret, v, sizeof a->secret);
  else if (!strcmp(k, "client_secret")) fm_strlcpy(a->client_secret, v, sizeof a->client_secret);
  else if (!strcmp(k, "session")) fm_strlcpy(a->session, v, sizeof a->session);
  if (plain) {
    memset(plain, 0, SECRET_MAX + 1);
    fm_free(plain);
  }
}

static void load(void) {
  g_loaded = true;
  if (g_noload) return;
  char path[FM_PATH_MAX];
  if (!store_path(path, sizeof path)) return;
  FILE *f = fm_fopen(path, "rb");
  if (!f) return;
  char *line = (char *)fm_alloc(SECRET_MAX * 2 + 256);
  Slot *cur = NULL;
  while (fgets(line, (int)(SECRET_MAX * 2 + 256), f)) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!line[0] || line[0] == '#') continue;
    if (!strcmp(line, "[account]")) {
      cur = slot_new();
      continue;
    }
    char *eq = strchr(line, '=');
    if (!cur || !eq) continue;
    *eq = 0;
    set_field(&cur->a, line, eq + 1);
  }
  memset(line, 0, SECRET_MAX * 2 + 256);
  fm_free(line);
  fclose(f);
  /* drop blocks without a provider (a damaged file) */
  for (int i = g_nacct - 1; i >= 0; i--)
    if (!g_acct[i]->a.provider[0]) cloud_acct_remove(i);
}

static void ensure_loaded(void) {
  if (!g_loaded) load();
}

void cloud_init(bool shot) {
  g_noload = shot;
}

void cloud_acct_set_store(const char *path) {
  for (int i = 0; i < g_nacct; i++) {
    memset(g_acct[i], 0, sizeof *g_acct[i]);
    fm_free(g_acct[i]);
  }
  g_nacct = 0;
  g_dirty = false;
  fm_strlcpy(g_store, path ? path : "", sizeof g_store);
  g_loaded = false;
  if (path) g_noload = false;
}

void cloud_shutdown(void) {
  cloud_acct_pump();
  for (int i = 0; i < g_nacct; i++) {
    memset(g_acct[i], 0, sizeof *g_acct[i]);
    fm_free(g_acct[i]);
  }
  g_nacct = 0;
  g_loaded = false;
}

int cloud_acct_count(void) {
  ensure_loaded();
  return g_nacct;
}

FmCloudAcct *cloud_acct_at(int i) {
  ensure_loaded();
  return (i >= 0 && i < g_nacct) ? &g_acct[i]->a : NULL;
}

int cloud_acct_serial(int i) {
  ensure_loaded();
  return (i >= 0 && i < g_nacct) ? g_acct[i]->serial : 0;
}

bool cloud_acct_is_temp(int i) {
  return i >= 0 && i < g_nacct && g_acct[i]->temp;
}

int cloud_acct_index(int serial) {
  ensure_loaded();
  for (int i = 0; i < g_nacct; i++)
    if (g_acct[i]->serial == serial) return i;
  return -1;
}

FmCloudAcct *cloud_acct_by_serial(int serial) {
  return cloud_acct_at(cloud_acct_index(serial));
}

static int add(const FmCloudAcct *a, bool temp) {
  ensure_loaded();
  cloud_acct_lock();
  Slot *s = slot_new();
  if (s) {
    s->a = *a;
    s->a.session_changed = false;
    s->temp = temp;
    if (!s->a.label[0]) {
      const FmCloud *c = cloud_find(a->provider);
      fm_strlcpy(s->a.label, a->user[0] ? a->user : c ? c->name : a->provider, sizeof s->a.label);
    }
  }
  cloud_acct_unlock();
  if (!s) return -1;
  if (!temp) cloud_acct_save();
  return g_nacct - 1;
}

int cloud_acct_add(const FmCloudAcct *a) { return add(a, false); }
int cloud_acct_add_temp(const FmCloudAcct *a) { return add(a, true); }

void cloud_acct_keep(int i) {
  if (i < 0 || i >= g_nacct || !g_acct[i]->temp) return;
  g_acct[i]->temp = false;
  cloud_acct_save();
}

void cloud_acct_remove(int i) {
  if (i < 0 || i >= g_nacct) return;
  cloud_acct_lock();
  Slot *s = g_acct[i];
  bool temp = s->temp;
  memmove(&g_acct[i], &g_acct[i + 1], (size_t)(g_nacct - i - 1) * sizeof *g_acct);
  g_nacct--;
  g_stamp++;
  memset(s, 0, sizeof *s);
  fm_free(s);
  cloud_acct_unlock();
  if (!temp && g_loaded) cloud_acct_save();
}

void cloud_acct_save(void) {
  ensure_loaded();
  g_stamp++;
  g_dirty = false;
  if (g_noload) return;
  char path[FM_PATH_MAX], tmp[FM_PATH_MAX + 8];
  if (!store_path(path, sizeof path)) return;
  char dir[FM_PATH_MAX];
  fm_strlcpy(dir, path, sizeof dir);
  if (fm_path_parent(dir)) plat_mkdirs(dir);
  fm_snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *f = fm_fopen(tmp, "wb");
  if (!f) {
    fm_log("cloud: cannot write %s", tmp);
    return;
  }
  fputs("# MMC File Manager cloud accounts. Passwords, keys and sign-ins are protected for this user.\n", f);
  int saved = 0;
  cloud_acct_lock();
  for (int i = 0; i < g_nacct; i++) {
    const Slot *s = g_acct[i];
    if (s->temp) continue;
    const FmCloudAcct *a = &s->a;
    fputs("\n[account]\n", f);
    put_line(f, "provider", a->provider);
    put_line(f, "label", a->label);
    put_line(f, "user", a->user);
    put_line(f, "server", a->server);
    put_line(f, "client_id", a->client_id);
    put_secret(f, "client_secret", a->client_secret);
    put_secret(f, "secret", a->secret);
    put_secret(f, "session", a->session);
    saved++;
  }
  cloud_acct_unlock();
  bool ok = fclose(f) == 0;
  if (ok) {
    plat_remove_file(path);
    ok = plat_rename(tmp, path) == FM_OK;
  }
  if (!ok) {
    plat_remove_file(tmp);
    fm_log("cloud: saving %s failed", path);
  }
  FM_UNUSED(saved);
}

bool cloud_acct_snapshot(int serial, FmCloudAcct *out) {
  ensure_loaded();
  bool ok = false;
  cloud_acct_lock();
  for (int i = 0; i < g_nacct; i++)
    if (g_acct[i]->serial == serial) {
      *out = g_acct[i]->a;
      out->session_changed = false;
      ok = true;
      break;
    }
  cloud_acct_unlock();
  return ok;
}

void cloud_acct_writeback(int serial, FmCloudAcct *a, bool signed_in) {
  if (!a->session_changed && !signed_in) return;
  cloud_acct_lock();
  for (int i = 0; i < g_nacct; i++) {
    if (g_acct[i]->serial != serial) continue;
    FmCloudAcct *d = &g_acct[i]->a;
    fm_strlcpy(d->session, a->session, sizeof d->session);
    if (signed_in) {
      /* login may swap the password for a session, or learn the e-mail */
      fm_strlcpy(d->secret, a->secret, sizeof d->secret);
      if (a->user[0]) fm_strlcpy(d->user, a->user, sizeof d->user);
    }
    if (!g_acct[i]->temp) g_dirty = true;
    break;
  }
  a->session_changed = false;
  cloud_acct_unlock();
  app_wake();
}

void cloud_acct_pump(void) {
  if (g_dirty) cloud_acct_save();
}

/* ---- listings ----------------------------------------------------------------- */

void cloud_list_free(FmCloudList *l) {
  fm_free(l->items);
  memset(l, 0, sizeof *l);
}

FmCloudEntry *cloud_list_add(FmCloudList *l) {
  if (l->count == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 64;
    l->items = (FmCloudEntry *)fm_realloc(l->items, (size_t)l->cap * sizeof *l->items);
  }
  FmCloudEntry *e = &l->items[l->count++];
  memset(e, 0, sizeof *e);
  return e;
}

/* ---- HTTP errors ------------------------------------------------------------- */

FmErr cloud_http_error(const char *service, const FmNetResp *r, FmErr e, char *err, size_t cap) {
  const char *svc = service && service[0] ? service : "The service";
  if (e == FM_ERR_CANCEL) {
    fm_strlcpy(err, "Cancelled", cap);
    return FM_ERR_CANCEL;
  }
  if (e != FM_OK || !r || r->status == 0) {
    const char *why = r && r->error[0] ? r->error : fm_err_str(e != FM_OK ? e : FM_ERR_IO);
    fm_snprintf(err, cap, "Network error: %s", why);
    return e != FM_OK ? e : FM_ERR_IO;
  }
  int st = r->status;
  if (st >= 200 && st < 400) {
    if (cap) err[0] = 0;
    return FM_OK;
  }
  switch (st) {
    case 401:
      fm_strlcpy(err, "Signed out: sign in again", cap);
      return FM_ERR_PASSWORD;
    case 403:
      fm_snprintf(err, cap, "%s refused the request (HTTP 403)", svc);
      return FM_ERR_ACCESS;
    case 404: case 410:
      fm_snprintf(err, cap, "%s: not found (HTTP %d)", svc, st);
      return FM_ERR_NOT_FOUND;
    case 409: case 412:
      fm_snprintf(err, cap, "%s: an item with this name already exists (HTTP %d)", svc, st);
      return FM_ERR_EXISTS;
    case 413: case 507:
      fm_snprintf(err, cap, "%s: the storage is full (HTTP %d)", svc, st);
      return FM_ERR_FULL;
    case 429:
      fm_snprintf(err, cap, "%s is busy: too many requests, try again later (HTTP 429)", svc);
      return FM_ERR_IO;
    default: break;
  }
  if (st >= 500) fm_snprintf(err, cap, "%s had a problem (HTTP %d). Try again later.", svc, st);
  else fm_snprintf(err, cap, "%s refused the request (HTTP %d)", svc, st);
  return FM_ERR_IO;
}

/* ---- times -------------------------------------------------------------------- */

/* Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant). */
static i64 days_from_civil(i64 y, int m, int d) {
  y -= m <= 2;
  i64 era = (y >= 0 ? y : y - 399) / 400;
  i64 yoe = y - era * 400;
  i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static int digits(const char **s, int n) {
  int v = 0;
  for (int i = 0; i < n; i++) {
    if (**s < '0' || **s > '9') return -1;
    v = v * 10 + (**s - '0');
    (*s)++;
  }
  return v;
}

static i64 make_time(int y, int mo, int d, int h, int mi, int se) {
  if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 24 || mi < 0 || mi > 59 || se < 0 ||
      se > 60)
    return 0;
  return days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
}

static i64 parse_iso(const char *s) {
  int y = digits(&s, 4);
  if (y < 0 || *s++ != '-') return 0;
  int mo = digits(&s, 2);
  if (mo < 0 || *s++ != '-') return 0;
  int d = digits(&s, 2);
  if (d < 0) return 0;
  int h = 0, mi = 0, se = 0;
  if (*s == 'T' || *s == 't' || *s == ' ') {
    s++;
    h = digits(&s, 2);
    if (h < 0 || *s++ != ':') return 0;
    mi = digits(&s, 2);
    if (mi < 0) return 0;
    if (*s == ':') {
      s++;
      se = digits(&s, 2);
      if (se < 0) return 0;
    }
    if (*s == '.' || *s == ',')
      for (s++; *s >= '0' && *s <= '9'; s++) {}
  } else if (*s) {
    return 0;
  }
  i64 t = make_time(y, mo, d, h, mi, se);
  if (!t && !(y == 1970 && mo == 1 && d == 1)) return 0;
  if (*s == 'Z' || *s == 'z') return t;
  if (*s == '+' || *s == '-') {
    int sign = *s++ == '-' ? -1 : 1;
    int oh = digits(&s, 2), om = 0;
    if (oh < 0) return 0;
    if (*s == ':') s++;
    if (*s) {
      om = digits(&s, 2);
      if (om < 0) return 0;
    }
    return t - sign * (oh * 3600 + om * 60);
  }
  return *s ? 0 : t;   /* no zone: taken as UTC */
}

static i64 parse_rfc1123(const char *s) {
  static const char *const kMon[] = { "jan", "feb", "mar", "apr", "may", "jun",
                                      "jul", "aug", "sep", "oct", "nov", "dec" };
  const char *c = strchr(s, ',');
  if (c) s = c + 1;
  while (*s == ' ') s++;
  int d = 0;
  while (*s >= '0' && *s <= '9') d = d * 10 + (*s++ - '0');
  if (*s == '-') s++;                       /* RFC 850: 09-Oct-26 */
  while (*s == ' ') s++;
  int mo = 0;
  for (int i = 0; i < 12; i++)
    if (!fm_strnicmp(s, kMon[i], 3)) mo = i + 1;
  if (!mo) return 0;
  s += 3;
  if (*s == '-') s++;
  while (*s == ' ') s++;
  int y = 0, nd = 0;
  while (*s >= '0' && *s <= '9') { y = y * 10 + (*s++ - '0'); nd++; }
  if (nd == 2) y += y < 70 ? 2000 : 1900;
  while (*s == ' ') s++;
  int h = digits(&s, 2);
  if (h < 0 || *s++ != ':') return 0;
  int mi = digits(&s, 2);
  if (mi < 0 || *s++ != ':') return 0;
  int se = digits(&s, 2);
  if (se < 0) return 0;
  while (*s == ' ') s++;
  i64 t = make_time(y, mo, d, h, mi, se);
  if (*s == '+' || *s == '-') {
    int sign = *s++ == '-' ? -1 : 1;
    int oh = digits(&s, 2), om = digits(&s, 2);
    if (oh >= 0 && om >= 0) t -= sign * (oh * 3600 + om * 60);
  }
  return t;
}

i64 cloud_parse_time(const char *s) {
  if (!s) return 0;
  while (*s == ' ' || *s == '\t' || *s == '"') s++;
  if (!*s) return 0;
  if (s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[2] >= '0' && s[3] >= '0' && s[4] == '-') return parse_iso(s);
  return parse_rfc1123(s);
}
