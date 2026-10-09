/* fcloud_mega.c -- MEGA (mega.nz) for the cloud panels (fcloud.h), no SDK.
**
** The public protocol, as the open-source MEGA SDK, megatools and rclone's
** go-mega speak it:
**   - API: POST https://g.api.mega.co.nz/cs?id=<seq>[&sid=<session>][&n=<folder link>]
**     with a JSON array of commands; the reply is an array of results, or a
**     bare negative number (an error for the whole request). -3 = try again.
**     HTTP 402 + "X-Hashcash: 1:<easiness>:<time>:<token>" asks for a
**     proof of work (SHA-256 over a 4-byte prefix + 262144 copies of the
**     token); the retry carries "X-Hashcash: 1:<token>:<prefix>".
**   - Sign-in: "us0" says the account version. v1 derives the password key
**     with 65536 AES rounds (prepare_key) and "uh" = stringhash(email); v2
**     uses PBKDF2-HMAC-SHA512(password, salt, 100000) -> 16 bytes key + 16
**     bytes "uh". "us" returns the master key (AES-ECB under the password
**     key), the RSA private key (p, q, d, u MPIs under the master key) and
**     "csid" (RSA-encrypted session id: its first 43 bytes, base64url).
**   - Files are AES-128-CTR (nonce || 64-bit big-endian block counter) with
**     a CBC-MAC per chunk (128 KB, 256 KB ... 1 MB, then 1 MB) folded into
**     a 64-bit "meta MAC" kept in the node key: key[0..3]^key[4..7] = AES
**     key, key[4..5] = nonce, key[6..7] = meta MAC (32-bit words).
**   - Attributes are "MEGA{json}" under AES-CBC with a zero IV and the node
**     key; "n" = name, "c" = fingerprint (4 CRC32 samples + mtime).
**
** Design decisions:
**   - The whole tree comes with one "f" call (that is how MEGA works), so it
**     is cached per signed-in session / link in a small table (8 entries)
**     for 30 s; our own changes patch the cached tree, so the panel sees
**     them at once. The table is the adapter's only mutable global; a
**     spinlock guards it and an entry in use is never evicted.
**   - Session text (acct->session): "sid=..\nmk=..\n" for accounts,
**     "link=folder|file\nid=..\nk=..\n" for public links. The password is
**     wiped after sign-in; fcloud.c encrypts the session at rest.
**   - Downloads decrypt and MAC as the bytes arrive (net_get_stream) into
**     path.part; a dropped connection resumes with a "/start-end" range; a
**     MAC mismatch deletes the file and fails. Uploads read 128 KB-1 MB
**     chunks, MAC + encrypt them and POST to <url>/<offset>. Neither ever
**     holds the whole file.
**   - Replacing a file uploads the new one as a version of the old ("ov"),
**     like MEGA's own apps. Remove moves to the Rubbish Bin.
**   - Public links: /file/<id>#<key>, /folder/<id>#<key>[/folder|file/<h>],
**     and the old #!id!key and #F!id!key forms.
**   - Not done: two-factor sign-in (the contract has no code field),
**     incoming shares (keys under RSA), CloudRAID downloads, password-
**     protected links (#P!).
*/
#include "fcloud_mega_int.h"
#include "fbignum.h"
#include "fplat.h"
#include "fsdl.h"

#define MG_API "https://g.api.mega.co.nz/cs"
#define MG_TREE_TTL_MS 30000
#define MG_TREE_MAX_REPLY ((size_t)256 << 20)

/* ---- base64url ---------------------------------------------------------------- */

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

size_t mega_b64_enc(const u8 *p, size_t n, char *out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    u32 v = (u32)p[i] << 16;
    if (i + 1 < n) v |= (u32)p[i + 1] << 8;
    if (i + 2 < n) v |= p[i + 2];
    int chars = (n - i >= 3) ? 4 : (int)(n - i) + 1;
    for (int k = 0; k < chars; k++) {
      if (o + 1 < cap) out[o] = kB64[(v >> (18 - 6 * k)) & 63];
      o++;
    }
  }
  if (cap) out[o < cap ? o : cap - 1] = 0;
  return o;
}

int mega_b64_dec(const char *s, u8 *out, size_t cap) {
  u32 acc = 0;
  int bits = 0;
  size_t o = 0;
  for (; *s && *s != '='; s++) {
    int v;
    char c = *s;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '-' || c == '+') v = 62;
    else if (c == '_' || c == '/') v = 63;
    else if (c == ',') break;               /* MEGA joins key lists with ',' */
    else return -1;
    acc = (acc << 6) | (u32)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= cap) return -1;
      out[o++] = (u8)(acc >> bits);
    }
  }
  return (int)o;
}

/* ---- small helpers -------------------------------------------------------------- */

static void ecb_dec(const FmAes *a, u8 *p, size_t n) {
  for (size_t i = 0; i + 16 <= n; i += 16) aes_decrypt_block(a, p + i, p + i);
}
static void ecb_enc(const FmAes *a, u8 *p, size_t n) {
  for (size_t i = 0; i + 16 <= n; i += 16) aes_encrypt_block(a, p + i, p + i);
}

/* JSON string body (no quotes) */
static void json_esc(const char *s, char *out, size_t cap) {
  size_t o = 0;
  for (; *s && o + 7 < cap; s++) {
    u8 c = (u8)*s;
    if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
    else if (c < 0x20) o += (size_t)fm_snprintf(out + o, cap - o, "\\u%04x", c);
    else out[o++] = (char)c;
  }
  out[o] = 0;
}

static bool sess_get(const char *sess, const char *key, char *out, size_t cap) {
  size_t kl = strlen(key);
  for (const char *p = sess; p && *p;) {
    const char *e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len > kl && !strncmp(p, key, kl) && p[kl] == '=') {
      size_t vl = len - kl - 1;
      if (vl >= cap) vl = cap - 1;
      memcpy(out, p + kl + 1, vl);
      out[vl] = 0;
      return true;
    }
    p = e ? e + 1 : NULL;
  }
  if (cap) *out = 0;
  return false;
}

static const char *jstr(const FmJsonNode *o, const char *k) { return json_str(json_get(o, k), ""); }

/* ---- key derivation --------------------------------------------------------------- */

void mega_prepare_key(const char *password, u8 out[16]) {
  static const u8 init[16] = { 0x93, 0xC4, 0x67, 0xE3, 0x7D, 0xB0, 0xC7, 0xA4,
                               0xD1, 0xBE, 0x3F, 0x81, 0x01, 0x52, 0xCB, 0x56 };
  size_t len = strlen(password);
  int nkeys = (int)((len + 15) / 16);
  FmAes *keys = nkeys ? (FmAes *)fm_alloc(sizeof(FmAes) * (size_t)nkeys) : NULL;
  for (int j = 0; j < nkeys; j++) {
    u8 k[16];
    memset(k, 0, 16);
    size_t take = FM_MIN((size_t)16, len - (size_t)j * 16);
    memcpy(k, password + j * 16, take);
    aes_init(&keys[j], k, 16);
    wipe(k, sizeof k);
  }
  u8 pk[16];
  memcpy(pk, init, 16);
  for (int r = 0; r < 65536; r++)
    for (int j = 0; j < nkeys; j++) aes_encrypt_block(&keys[j], pk, pk);
  memcpy(out, pk, 16);
  if (keys) { wipe(keys, sizeof(FmAes) * (size_t)nkeys); fm_free(keys); }
  wipe(pk, sizeof pk);
}

void mega_stringhash(const char *email, const u8 key[16], char out[16]) {
  u8 h[16];
  memset(h, 0, sizeof h);
  for (size_t i = 0; email[i]; i++) h[i & 15] ^= (u8)email[i];
  FmAes a;
  aes_init(&a, key, 16);
  for (int i = 0; i < 16384; i++) aes_encrypt_block(&a, h, h);
  u8 r[8];
  memcpy(r, h, 4);
  memcpy(r + 4, h + 8, 4);
  mega_b64_enc(r, 8, out, 16);
  wipe(&a, sizeof a);
}

void mega_v2_derive(const char *password, const u8 *salt, size_t salt_len, u8 pass_key[16], char uh[32]) {
  u8 d[32];
  pbkdf2_sha512((const u8 *)password, strlen(password), salt, salt_len, 100000, d, 32);
  memcpy(pass_key, d, 16);
  mega_b64_enc(d + 16, 16, uh, 32);
  wipe(d, sizeof d);
}

/* MPI: 2-byte big-endian bit count, then the bytes */
static const u8 *mpi_read(const u8 *p, const u8 *end, FmBn *out) {
  if (end - p < 2) return NULL;
  size_t bits = ((size_t)p[0] << 8) | p[1];
  size_t bytes = (bits + 7) / 8;
  p += 2;
  if ((size_t)(end - p) < bytes || !bn_from_bytes(out, p, bytes)) return NULL;
  return p + bytes;
}

FmErr mega_login_reply(const FmJsonNode *r, const u8 pass_key[16], char *sid, size_t sidcap, u8 mk[16],
                       char *err, size_t errcap) {
  u8 k[32];
  FmAes a;
  if (mega_b64_dec(jstr(r, "k"), k, sizeof k) != 16) {
    fm_snprintf(err, errcap, "MEGA sent an unexpected sign-in reply");
    return FM_ERR_FORMAT;
  }
  aes_init(&a, pass_key, 16);
  aes_decrypt_block(&a, k, mk);
  aes_init(&a, mk, 16);
  const char *csid = jstr(r, "csid"), *tsid = jstr(r, "tsid");
  FmErr res = FM_ERR_FORMAT;
  if (*csid) {
    /* the RSA private key: p, q, d, u under the master key (ECB) */
    const char *pk64 = jstr(r, "privk");
    size_t cap = strlen(pk64) * 3 / 4 + 16;
    u8 *pk = (u8 *)fm_alloc(cap);
    u8 *ct = (u8 *)fm_alloc(strlen(csid) * 3 / 4 + 16);
    int pn = mega_b64_dec(pk64, pk, cap);
    int cn = mega_b64_dec(csid, ct, strlen(csid) * 3 / 4 + 16);
    FmBn *bn = (FmBn *)fm_alloc(sizeof(FmBn) * 6);   /* p q d n c m */
    if (pn >= 16 && cn > 2) {
      ecb_dec(&a, pk, (size_t)pn & ~(size_t)15);
      const u8 *q = mpi_read(pk, pk + pn, &bn[0]);
      if (q) q = mpi_read(q, pk + pn, &bn[1]);
      if (q) q = mpi_read(q, pk + pn, &bn[2]);
      if (q && mpi_read(ct, ct + cn, &bn[4]) && bn_mul(&bn[3], &bn[0], &bn[1]) &&
          bn_modexp(&bn[5], &bn[4], &bn[2], &bn[3])) {
        u8 m[BN_LIMBS * 4];
        size_t ml = bn_to_bytes(&bn[5], m, sizeof m);
        if (ml >= 43) {
          mega_b64_enc(m, 43, sid, sidcap);
          res = FM_OK;
        }
        wipe(m, sizeof m);
      }
    }
    wipe(pk, cap);
    wipe(bn, sizeof(FmBn) * 6);
    fm_free(pk);
    fm_free(ct);
    fm_free(bn);
  } else if (*tsid) {
    /* temporary session: its first 16 bytes under the master key are its last 16 */
    u8 t[48], c[16];
    if (mega_b64_dec(tsid, t, sizeof t) == 32) {
      aes_encrypt_block(&a, t, c);
      if (!memcmp(c, t + 16, 16)) {
        fm_strlcpy(sid, tsid, sidcap);
        res = FM_OK;
      }
    }
  }
  wipe(&a, sizeof a);
  wipe(k, sizeof k);
  if (res != FM_OK) {
    wipe(mk, 16);
    fm_snprintf(err, errcap, "Could not open the MEGA session keys (wrong password?)");
  }
  return res;
}

/* ---- attributes -------------------------------------------------------------------- */

bool mega_attr_decrypt(const u8 key[16], const char *b64, char *name, size_t ncap, char *fp, size_t fpcap) {
  size_t cap = strlen(b64) * 3 / 4 + 16;
  u8 *buf = (u8 *)fm_alloc(cap + 1);
  int n = mega_b64_dec(b64, buf, cap);
  bool ok = false;
  if (name && ncap) *name = 0;
  if (fp && fpcap) *fp = 0;
  if (n >= 16) {
    FmAes a;
    u8 iv[16];
    memset(iv, 0, 16);
    aes_init(&a, key, 16);
    n &= ~15;
    aes_cbc_decrypt(&a, iv, buf, (size_t)n);
    while (n > 0 && buf[n - 1] == 0) n--;
    buf[n] = 0;
    if (n > 4 && !memcmp(buf, "MEGA{", 5)) {
      FmJson j;
      if (json_parse(&j, (const char *)buf + 4, (size_t)n - 4) == FM_OK) {
        const FmJsonNode *root = json_root(&j);
        if (root && root->type == JSON_OBJ) {
          if (name) fm_strlcpy(name, jstr(root, "n"), ncap);
          if (fp) fm_strlcpy(fp, jstr(root, "c"), fpcap);
          ok = true;
        }
        json_free(&j);
      }
    }
    wipe(&a, sizeof a);
  }
  fm_free(buf);
  return ok;
}

bool mega_attr_encrypt(const u8 key[16], const char *name, const char *fp, char *out, size_t cap) {
  size_t nl = strlen(name) * 6 + (fp ? strlen(fp) : 0) + 64;
  char *js = (char *)fm_alloc(nl);
  char *esc = (char *)fm_alloc(nl);
  json_esc(name, esc, nl);
  int len = (fp && *fp) ? fm_snprintf(js, nl, "MEGA{\"n\":\"%s\",\"c\":\"%s\"}", esc, fp)
                        : fm_snprintf(js, nl, "MEGA{\"n\":\"%s\"}", esc);
  size_t padded = ((size_t)len + 15) & ~(size_t)15;
  u8 *buf = (u8 *)fm_calloc(1, padded);
  memcpy(buf, js, (size_t)len);
  FmAes a;
  u8 iv[16];
  memset(iv, 0, 16);
  aes_init(&a, key, 16);
  aes_cbc_encrypt(&a, iv, buf, padded);
  bool ok = mega_b64_enc(buf, padded, out, cap) < cap;
  fm_free(buf);
  fm_free(js);
  fm_free(esc);
  wipe(&a, sizeof a);
  return ok;
}

i64 mega_fp_mtime(const char *fp) {
  u8 b[32];
  int n = fp ? mega_b64_dec(fp, b, sizeof b) : -1;
  if (n < 17) return 0;
  int cnt = b[16];
  if (cnt > 8 || 17 + cnt > n) return 0;
  u64 v = 0;
  for (int i = cnt; i >= 1; i--) v = (v << 8) | b[16 + i];
  return (i64)v;
}

/* MEGA SDK FileFingerprint: <= 16 bytes verbatim; <= 8 KB four CRC32s of
** the quarters; else four CRC32s over 32 sparse 64-byte blocks each. CRCs
** stored big-endian, then the mtime as count + little-endian bytes. */
bool mega_fingerprint(const char *path, u64 size, i64 mtime, char *out, size_t cap) {
  u8 crc[16 + 9];
  memset(crc, 0, sizeof crc);
  FILE *f = fm_fopen(path, "rb");
  if (!f) return false;
  bool ok = true;
  if (size <= 16) {
    ok = fread(crc, 1, (size_t)size, f) == (size_t)size;
  } else if (size <= 8192) {
    u8 buf[8192];
    ok = fread(buf, 1, (size_t)size, f) == (size_t)size;
    for (int i = 0; ok && i < 4; i++) {
      size_t b = (size_t)(i * size / 4), e = (size_t)((i + 1) * size / 4);
      u32 c = crc32_update(0, buf + b, e - b);
      crc[4 * i] = (u8)(c >> 24); crc[4 * i + 1] = (u8)(c >> 16);
      crc[4 * i + 2] = (u8)(c >> 8); crc[4 * i + 3] = (u8)c;
    }
  } else {
    for (int i = 0; ok && i < 4; i++) {
      u32 c = 0;
      for (int j = 0; ok && j < 32; j++) {
        u64 off = (size - 64) * (u64)(i * 32 + j) / 127;
        u8 blk[64];
        ok = fm_fseek64(f, (i64)off, SEEK_SET) == 0 && fread(blk, 1, 64, f) == 64;
        c = crc32_update(c, blk, 64);
      }
      crc[4 * i] = (u8)(c >> 24); crc[4 * i + 1] = (u8)(c >> 16);
      crc[4 * i + 2] = (u8)(c >> 8); crc[4 * i + 3] = (u8)c;
    }
  }
  fclose(f);
  if (!ok) return false;
  u64 t = mtime > 0 ? (u64)mtime : 0;
  int cnt = 0;
  while (t) { crc[17 + cnt++] = (u8)t; t >>= 8; }
  crc[16] = (u8)cnt;
  return mega_b64_enc(crc, (size_t)(17 + cnt), out, cap) < cap;
}

/* ---- tree ---------------------------------------------------------------------------- */

static u32 h_hash(const char *h) {
  u32 v = 2166136261u;
  for (; *h; h++) v = (v ^ (u8)*h) * 16777619u;
  return v;
}

static void idx_insert(MgTree *t, int i) {
  u32 m = (u32)t->idx_cap - 1, k = h_hash(t->n[i].h) & m;
  while (t->idx[k] >= 0) k = (k + 1) & m;
  t->idx[k] = i;
}

MgNode *mega_tree_find(MgTree *t, const char *h) {
  if (!t->idx_cap || !h || !*h) return NULL;
  u32 m = (u32)t->idx_cap - 1, k = h_hash(h) & m;
  while (t->idx[k] >= 0) {
    if (!strcmp(t->n[t->idx[k]].h, h)) return &t->n[t->idx[k]];
    k = (k + 1) & m;
  }
  return NULL;
}

MgNode *mega_tree_add(MgTree *t, const char *h) {
  if (t->count == t->cap) {
    t->cap = t->cap ? t->cap * 2 : 256;
    t->n = (MgNode *)fm_realloc(t->n, sizeof(MgNode) * (size_t)t->cap);
  }
  MgNode *n = &t->n[t->count++];
  memset(n, 0, sizeof *n);
  fm_strlcpy(n->h, h, sizeof n->h);
  n->name = n->fp = "";
  if (t->count * 2 > t->idx_cap) {
    int cap = t->idx_cap ? t->idx_cap : 512;
    while (cap < t->count * 2) cap *= 2;
    fm_free(t->idx);
    t->idx = (int *)fm_alloc(sizeof(int) * (size_t)cap);
    t->idx_cap = cap;
    for (int i = 0; i < cap; i++) t->idx[i] = -1;
    for (int i = 0; i < t->count; i++) idx_insert(t, i);
  } else {
    idx_insert(t, t->count - 1);
  }
  return n;
}

void mega_tree_free(MgTree *t) {
  if (t->n) wipe(t->n, sizeof(MgNode) * (size_t)t->cap);
  fm_free(t->n);
  fm_free(t->idx);
  if (t->strings.block_size) arena_free(&t->strings);
  memset(t, 0, sizeof *t);
}

void mega_node_aes_key(const MgNode *n, u8 out[16]) {
  for (int i = 0; i < 16; i++) out[i] = n->klen == 32 ? (u8)(n->key[i] ^ n->key[16 + i]) : n->key[i];
}

typedef struct ShareKey { char h[12]; u8 k[16]; } ShareKey;

FmErr mega_tree_build(MgTree *t, const FmJsonNode *f, const u8 key[16], bool link) {
  memset(t, 0, sizeof *t);
  arena_init(&t->strings, 64 * 1024);
  const FmJsonNode *nodes = json_get(f, "f");
  if (!nodes || nodes->type != JSON_ARR) return FM_ERR_FORMAT;
  FmAes mka;
  aes_init(&mka, key, 16);
  /* share keys: "ok" entries and our outgoing shares' "sk", under the master key */
  ShareKey *sk = NULL;
  int nsk = 0, csk = 0;
  if (!link) {
    for (int pass = 0; pass < 2; pass++) {
      const FmJsonNode *arr = pass ? nodes : json_get(f, "ok");
      for (const FmJsonNode *o = json_first(arr); o; o = json_next(o)) {
        const char *h = jstr(o, "h"), *k = jstr(o, pass ? "sk" : "k");
        u8 kb[32];
        if (!*h || strlen(h) >= 12 || mega_b64_dec(k, kb, sizeof kb) != 16) continue;
        if (nsk == csk) {
          csk = csk ? csk * 2 : 16;
          sk = (ShareKey *)fm_realloc(sk, sizeof(ShareKey) * (size_t)csk);
        }
        fm_strlcpy(sk[nsk].h, h, sizeof sk[nsk].h);
        aes_decrypt_block(&mka, kb, sk[nsk].k);
        nsk++;
      }
    }
  }
  char name[1024], fp[128];
  for (const FmJsonNode *o = json_first(nodes); o; o = json_next(o)) {
    const char *h = jstr(o, "h");
    int type = (int)json_num(json_get(o, "t"), -1);
    if (!*h || strlen(h) >= 12 || type < 0 || type > 4) continue;
    u8 nk[32];
    int klen = 0;
    if (type == MG_FILE || type == MG_FOLDER) {
      /* "k": "<handle>:<key>/<handle>:<key>..." -- user handles (11 chars)
      ** take the master key, node handles (8) a share key; links: the link key */
      const char *kp = jstr(o, "k");
      while (*kp && !klen) {
        const char *colon = strchr(kp, ':');
        if (!colon) break;
        const char *end = strchr(colon, '/');
        size_t hl = (size_t)(colon - kp), kl = end ? (size_t)(end - colon - 1) : strlen(colon + 1);
        char ph[16], kb64[64];
        if (hl < sizeof ph && kl < sizeof kb64) {
          memcpy(ph, kp, hl); ph[hl] = 0;
          memcpy(kb64, colon + 1, kl); kb64[kl] = 0;
          const u8 *dk = NULL;
          if (link) dk = key;
          else if (hl == 11) dk = key;
          else for (int i = 0; i < nsk; i++) if (!strcmp(sk[i].h, ph)) { dk = sk[i].k; break; }
          int n = dk ? mega_b64_dec(kb64, nk, sizeof nk) : -1;
          if (dk && (n == 16 || n == 32) && n == (type == MG_FILE ? 32 : 16)) {
            FmAes da;
            aes_init(&da, dk, 16);
            ecb_dec(&da, nk, (size_t)n);
            wipe(&da, sizeof da);
            klen = n;
          }
        }
        kp = end ? end + 1 : "";
      }
      if (!klen) continue;                  /* not ours to open (incoming share under RSA) */
    }
    MgNode tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.type = (u8)type;
    tmp.klen = (u8)klen;
    memcpy(tmp.key, nk, sizeof nk);
    name[0] = fp[0] = 0;
    if (klen) {
      u8 ak[16];
      mega_node_aes_key(&tmp, ak);
      bool ok = mega_attr_decrypt(ak, jstr(o, "a"), name, sizeof name, fp, sizeof fp);
      wipe(ak, sizeof ak);
      if (!ok || !name[0]) continue;        /* wrong key: leave it out */
    } else {
      fm_strlcpy(name, type == MG_ROOT ? "Cloud Drive" : type == MG_INBOX ? "Inbox" : "Rubbish Bin", sizeof name);
    }
    if (mega_tree_find(t, h)) continue;     /* duplicate */
    MgNode *n = mega_tree_add(t, h);
    tmp.size = (u64)json_num(json_get(o, "s"), 0);
    tmp.ts = (i64)json_num(json_get(o, "ts"), 0);
    tmp.mtime = mega_fp_mtime(fp);
    memcpy(tmp.h, n->h, sizeof tmp.h);
    fm_strlcpy(tmp.p, jstr(o, "p"), sizeof tmp.p);
    tmp.name = arena_strdup(&t->strings, name);
    tmp.fp = fp[0] ? arena_strdup(&t->strings, fp) : "";
    *n = tmp;
    if (type == MG_ROOT && !t->root[0]) fm_strlcpy(t->root, h, sizeof t->root);
    if (type == MG_RUBBISH) fm_strlcpy(t->rubbish, h, sizeof t->rubbish);
    wipe(&tmp, sizeof tmp);
  }
  if (link) {
    /* the shared folder: the folder whose parent is not in the tree */
    for (int i = 0; i < t->count && !t->root[0]; i++)
      if (t->n[i].type == MG_FOLDER && !mega_tree_find(t, t->n[i].p)) fm_strlcpy(t->root, t->n[i].h, sizeof t->root);
  }
  if (sk) { wipe(sk, sizeof(ShareKey) * (size_t)csk); fm_free(sk); }
  wipe(&mka, sizeof mka);
  return t->root[0] ? FM_OK : FM_ERR_FORMAT;
}

/* ---- MAC -------------------------------------------------------------------------------- */

u64 mega_chunk_end(u64 start) {
  u64 p = 0;
  for (int i = 1; i <= 8; i++) {
    u64 e = p + (u64)i * 131072;
    if (start < e) return e;
    p = e;
  }
  return start + 1048576 - (start - p) % 1048576;
}

void mega_mac_init(MgMac *m, const FmAes *aes, const u8 nonce[8]) {
  memset(m, 0, sizeof *m);
  m->aes = aes;
  memcpy(m->nonce, nonce, 8);
}

static void mac_close(MgMac *m) {
  if (m->bn) {
    memset(m->blk + m->bn, 0, (size_t)(16 - m->bn));
    for (int i = 0; i < 16; i++) m->cur[i] ^= m->blk[i];
    aes_encrypt_block(m->aes, m->cur, m->cur);
    m->bn = 0;
  }
  for (int i = 0; i < 16; i++) m->file[i] ^= m->cur[i];
  aes_encrypt_block(m->aes, m->file, m->file);
  m->open = false;
}

void mega_mac_feed(MgMac *m, const u8 *p, size_t n) {
  while (n) {
    if (m->open && m->pos == m->chunk_end) mac_close(m);
    if (!m->open) {
      memcpy(m->cur, m->nonce, 8);
      memcpy(m->cur + 8, m->nonce, 8);
      m->chunk_end = mega_chunk_end(m->pos);
      m->open = true;
    }
    size_t take = (size_t)FM_MIN((u64)n, m->chunk_end - m->pos);
    size_t i = 0;
    while (i < take) {
      if (m->bn == 0 && take - i >= 16) {
        for (int k = 0; k < 16; k++) m->cur[k] ^= p[i + k];
        aes_encrypt_block(m->aes, m->cur, m->cur);
        i += 16;
      } else {
        m->blk[m->bn++] = p[i++];
        if (m->bn == 16) {
          for (int k = 0; k < 16; k++) m->cur[k] ^= m->blk[k];
          aes_encrypt_block(m->aes, m->cur, m->cur);
          m->bn = 0;
        }
      }
    }
    m->pos += take;
    p += take;
    n -= take;
  }
}

void mega_mac_final(MgMac *m, u8 meta[8]) {
  if (m->open) mac_close(m);
  for (int i = 0; i < 4; i++) {
    meta[i] = (u8)(m->file[i] ^ m->file[4 + i]);
    meta[4 + i] = (u8)(m->file[8 + i] ^ m->file[12 + i]);
  }
}

/* ---- links ----------------------------------------------------------------------------- */

static bool take_tok(const char **pp, char stop1, char stop2, char *out, size_t cap) {
  const char *p = *pp;
  size_t n = 0;
  while (*p && *p != stop1 && *p != stop2 && !strchr("?& \t\r\n", *p)) {
    if (n + 1 < cap) out[n++] = *p;
    p++;
  }
  out[n] = 0;
  *pp = p;
  return n > 0 && n + 1 < cap;
}

bool mega_parse_link(const char *link, MgLink *out) {
  memset(out, 0, sizeof *out);
  const char *p = fm_stristr(link, "mega.nz/");
  if (!p) p = fm_stristr(link, "mega.co.nz/");
  if (!p) p = fm_stristr(link, "mega.io/");
  if (!p) return false;
  p = strchr(p, '/') + 1;
  char key[64];
  if (!fm_strnicmp(p, "file/", 5) || !fm_strnicmp(p, "folder/", 7) || !fm_strnicmp(p, "embed/", 6)) {
    out->folder = !fm_strnicmp(p, "folder/", 7);
    p = strchr(p, '/') + 1;
    if (!take_tok(&p, '#', '/', out->id, sizeof out->id) || *p != '#') return false;
    p++;
    if (!take_tok(&p, '/', '!', key, sizeof key)) return false;
    if (out->folder && (!fm_strnicmp(p, "/folder/", 8) || !fm_strnicmp(p, "/file/", 6))) {
      p = strchr(p + 1, '/') + 1;
      take_tok(&p, '/', '?', out->sub, sizeof out->sub);
    }
  } else if (p[0] == '#' && (p[1] == '!' || (p[1] == 'F' && p[2] == '!'))) {
    out->folder = p[1] == 'F';
    p += out->folder ? 3 : 2;
    if (!take_tok(&p, '!', '!', out->id, sizeof out->id) || *p != '!') return false;
    p++;
    if (!take_tok(&p, '!', '!', key, sizeof key)) return false;
    if (out->folder && *p == '!') { p++; take_tok(&p, '!', '!', out->sub, sizeof out->sub); }
  } else {
    return false;
  }
  out->klen = mega_b64_dec(key, out->key, sizeof out->key);
  return out->klen == (out->folder ? 16 : 32);
}

/* ---- API calls ------------------------------------------------------------------------- */

typedef struct MgApi {
  char sid[160];           /* signed in, or "" */
  char folder[16];         /* folder link: &n= */
  volatile int *cancel;
  char *err;
  size_t errcap;
} MgApi;

static FmErr mg_code(int code, const char *what, char *err, size_t cap) {
  switch (code) {
  case -9:  fm_snprintf(err, cap, "%s: not found on MEGA", what); return FM_ERR_NOT_FOUND;
  case -11: fm_snprintf(err, cap, "MEGA refused access (%s)", what); return FM_ERR_ACCESS;
  case -12: fm_snprintf(err, cap, "%s: it already exists", what); return FM_ERR_EXISTS;
  case -14: fm_snprintf(err, cap, "MEGA: the decryption key is wrong"); return FM_ERR_PASSWORD;
  case -15: fm_snprintf(err, cap, "Signed out: sign in again"); return FM_ERR_PASSWORD;
  case -16: fm_snprintf(err, cap, "MEGA has blocked this account or link"); return FM_ERR_ACCESS;
  case -17: fm_snprintf(err, cap, "MEGA: over the storage or transfer quota"); return FM_ERR_FULL;
  case -26: fm_snprintf(err, cap, "This MEGA account uses two-factor sign-in, which is not supported yet");
            return FM_ERR_UNSUPPORTED;
  case -4: case -6: case -18: case -19:
    fm_snprintf(err, cap, "MEGA is busy (error %d); try again later", code); return FM_ERR_IO;
  default:  fm_snprintf(err, cap, "MEGA error %d (%s)", code, what); return FM_ERR_IO;
  }
}

static void mg_sleep(int ms, volatile int *cancel) {
  for (int t = 0; t < ms && !(cancel && *cancel); t += 50) SDL_Delay(50);
}

/* Proof of work for HTTP 402: find a 4-byte prefix (a little-endian
** counter, as MEGA's clients count) with SHA-256(prefix + 262144 x the
** 48-byte token)[0..4] <= threshold. Each try hashes 12 MB, so workers on
** half the cores take interleaved counters; the 12 MB is never built, a
** 12 KB slab of token copies is hashed 1024 times instead. */
typedef struct HcJob {
  const u8 *slab;
  u32 threshold, start, step;
  volatile int *cancel;
  volatile int *done;
  u32 *result;
  SDL_SpinLock *lock;
} HcJob;

enum { HC_REPS = 256 };

static int SDLCALL hc_worker(void *arg) {
  HcJob *j = (HcJob *)arg;
  for (u32 pre = j->start;; pre += j->step) {
    if (*j->done || (j->cancel && *j->cancel)) break;
    u8 pb[4] = { (u8)pre, (u8)(pre >> 8), (u8)(pre >> 16), (u8)(pre >> 24) };
    FmSha256 s;
    u8 d[32];
    sha256_init(&s);
    sha256_update(&s, pb, 4);
    for (int i = 0; i < 262144 / HC_REPS; i++) sha256_update(&s, j->slab, 48 * HC_REPS);
    sha256_final(&s, d);
    u32 v = ((u32)d[0] << 24) | ((u32)d[1] << 16) | ((u32)d[2] << 8) | d[3];
    if (v <= j->threshold) {
      SDL_AtomicLock(j->lock);
      if (!*j->done) { *j->result = pre; *j->done = 1; }
      SDL_AtomicUnlock(j->lock);
      break;
    }
    if (pre > 0xFFFFFFFFu - j->step) break;
  }
  return 0;
}

bool mega_hashcash(const char *hdr, char *out, size_t cap, volatile int *cancel) {
  char tok[96];
  const char *p = hdr;
  int ver = atoi(p);
  if (!(p = strchr(p, ':'))) return false;
  int easy = atoi(++p);
  if (!(p = strchr(p, ':')) || !(p = strchr(p + 1, ':'))) return false;
  fm_strlcpy(tok, p + 1, sizeof tok);
  for (char *e = tok; *e; e++) if (*e == '\r' || *e == '\n' || *e == ' ') { *e = 0; break; }
  if (ver != 1 || easy < 0 || easy > 255) return false;
  u8 tb[48];
  memset(tb, 0, sizeof tb);
  if (mega_b64_dec(tok, tb, sizeof tb) <= 0) return false;
  u8 *slab = (u8 *)fm_alloc(48 * HC_REPS);
  for (int i = 0; i < HC_REPS; i++) memcpy(slab + 48 * i, tb, 48);
  int nw = SDL_GetCPUCount() / 2;
  nw = FM_CLAMP(nw, 1, 8);
  HcJob jobs[8];
  SDL_Thread *th[8];
  volatile int done = 0;
  u32 result = 0;
  SDL_SpinLock lock = 0;
  for (int i = 0; i < nw; i++) {
    HcJob *j = &jobs[i];
    j->slab = slab;
    j->threshold = (u32)((((easy & 63) << 1) + 1) << ((easy >> 6) * 7 + 3));
    j->start = (u32)(i + 1);
    j->step = (u32)nw;
    j->cancel = cancel;
    j->done = &done;
    j->result = &result;
    j->lock = &lock;
    th[i] = i ? fm_thread_create(hc_worker, "mega-hashcash", j) : NULL;
  }
  hc_worker(&jobs[0]);                     /* this thread is worker 0 */
  for (int i = 1; i < nw; i++)
    if (th[i]) SDL_WaitThread(th[i], NULL);
    else hc_worker(&jobs[i]);              /* no thread: run its share here */
  fm_free(slab);
  if (!done) return false;
  u8 pb[4] = { (u8)result, (u8)(result >> 8), (u8)(result >> 16), (u8)(result >> 24) };
  char cash[16];
  mega_b64_enc(pb, 4, cash, sizeof cash);
  fm_snprintf(out, cap, "X-Hashcash: 1:%s:%s\r\n", tok, cash);
  return true;
}

/* One command; *res is the command's reply inside j (an object, array or
** string). Retries -3, 5xx and dropped connections with backoff. */
static FmErr mg_call(MgApi *api, const char *cmd, FmJson *j, const FmJsonNode **res, size_t max_reply) {
  memset(j, 0, sizeof *j);
  *res = NULL;
  size_t bl = strlen(cmd) + 3;
  char *body = (char *)fm_alloc(bl);
  fm_snprintf(body, bl, "[%s]", cmd);
  char hc[256] = "";
  int wait = 250;
  FmErr e = FM_ERR_IO;
  for (int attempt = 0; attempt < 7; attempt++) {
    if (api->cancel && *api->cancel) { e = FM_ERR_CANCEL; break; }
    u32 seq;
    plat_random(&seq, sizeof seq);
    char url[384], hdr[384];
    int ul = fm_snprintf(url, sizeof url, MG_API "?id=%u", (unsigned)seq);
    if (api->sid[0]) ul += fm_snprintf(url + ul, sizeof url - (size_t)ul, "&sid=%s", api->sid);
    if (api->folder[0]) fm_snprintf(url + ul, sizeof url - (size_t)ul, "&n=%s", api->folder);
    fm_snprintf(hdr, sizeof hdr, "Content-Type: application/json\r\n%s", hc);
    FmNetReq rq;
    memset(&rq, 0, sizeof rq);
    rq.method = "POST";
    rq.headers = hdr;
    rq.body = body;
    rq.body_len = strlen(body);
    rq.max_reply = max_reply ? max_reply : ((size_t)16 << 20);
    FmNetResp r;
    FmErr ne = net_request(url, &rq, &r, api->cancel);
    if (ne == FM_ERR_CANCEL || (api->cancel && *api->cancel)) { net_resp_free(&r); e = FM_ERR_CANCEL; break; }
    if (ne != FM_OK) {
      fm_snprintf(api->err, api->errcap, "Network error: %s", r.error[0] ? r.error : fm_err_str(ne));
      net_resp_free(&r);
      e = ne == FM_ERR_UNSUPPORTED ? ne : FM_ERR_IO;
      if (ne == FM_ERR_UNSUPPORTED) break;
      mg_sleep(wait, api->cancel);
      wait *= 2;
      continue;
    }
    if (r.status == 402) {
      char ch[256];
      hc[0] = 0;
      if (net_resp_header(&r, "X-Hashcash", ch, sizeof ch)) mega_hashcash(ch, hc, sizeof hc, api->cancel);
      net_resp_free(&r);
      if (!hc[0]) { fm_snprintf(api->err, api->errcap, "MEGA asked for a proof of work this app could not give"); e = FM_ERR_ACCESS; break; }
      continue;
    }
    if (r.status >= 500 || r.status == 0) {
      fm_snprintf(api->err, api->errcap, "MEGA is not answering (HTTP %d)", r.status);
      net_resp_free(&r);
      e = FM_ERR_IO;
      mg_sleep(wait, api->cancel);
      wait *= 2;
      continue;
    }
    if (r.status != 200) {
      fm_snprintf(api->err, api->errcap, "MEGA refused the request (HTTP %d)", r.status);
      net_resp_free(&r);
      e = FM_ERR_ACCESS;
      break;
    }
    FmErr pe = json_parse(j, (const char *)r.data, r.len);
    net_resp_free(&r);
    if (pe != FM_OK) {
      json_free(j);
      memset(j, 0, sizeof *j);
      fm_snprintf(api->err, api->errcap, "MEGA sent a reply this app cannot read");
      e = FM_ERR_FORMAT;
      break;
    }
    const FmJsonNode *root = json_root(j), *first = root;
    if (root && root->type == JSON_ARR) first = json_first(root);
    if (first && first->type == JSON_NUM && first->n < 0) {
      int code = (int)first->n;
      json_free(j);
      memset(j, 0, sizeof *j);
      if (code == -3) { e = FM_ERR_IO; fm_snprintf(api->err, api->errcap, "MEGA is busy; try again"); mg_sleep(wait, api->cancel); wait *= 2; continue; }
      e = mg_code(code, "request", api->err, api->errcap);
      break;
    }
    if (!first) {
      json_free(j);
      memset(j, 0, sizeof *j);
      fm_snprintf(api->err, api->errcap, "MEGA sent an empty reply");
      e = FM_ERR_FORMAT;
      break;
    }
    *res = first;
    e = FM_OK;
    break;
  }
  fm_free(body);
  return e;
}

/* ---- per-account state --------------------------------------------------------------- */

typedef struct MgCtx {
  MgApi api;
  u8 mk[16];               /* master key (accounts) */
  MgLink link;             /* public links */
  bool is_link;
} MgCtx;

static FmErr ctx_open(FmCloudAcct *a, MgCtx *c, char *err, size_t errcap, volatile int *cancel) {
  memset(c, 0, sizeof *c);
  c->api.cancel = cancel;
  c->api.err = err;
  c->api.errcap = errcap;
  char v[128];
  if (sess_get(a->session, "link", v, sizeof v)) {
    char id[32], k[96];
    sess_get(a->session, "id", id, sizeof id);
    sess_get(a->session, "k", k, sizeof k);
    c->is_link = true;
    c->link.folder = !strcmp(v, "folder");
    fm_strlcpy(c->link.id, id, sizeof c->link.id);
    c->link.klen = mega_b64_dec(k, c->link.key, sizeof c->link.key);
    if (c->link.klen != (c->link.folder ? 16 : 32)) {
      fm_snprintf(err, errcap, "This MEGA link is incomplete (the key is missing)");
      return FM_ERR_PASSWORD;
    }
    if (c->link.folder) fm_strlcpy(c->api.folder, id, sizeof c->api.folder);
    return FM_OK;
  }
  char mk[64];
  if (!sess_get(a->session, "sid", c->api.sid, sizeof c->api.sid) || !sess_get(a->session, "mk", mk, sizeof mk) ||
      mega_b64_dec(mk, c->mk, sizeof c->mk) != 16) {
    wipe(mk, sizeof mk);
    fm_snprintf(err, errcap, "Signed out: sign in again");
    return FM_ERR_PASSWORD;
  }
  wipe(mk, sizeof mk);
  return FM_OK;
}

static void ctx_close(MgCtx *c) { wipe(c, sizeof *c); }

/* ---- tree cache ------------------------------------------------------------------------ */

typedef struct MgCache {
  char key[200];
  u64 stamp, used;
  bool busy;
  MgTree tree;
} MgCache;

static MgCache g_cache[8];
static SDL_SpinLock g_cache_lock;

static void cache_key(const MgCtx *c, char *out, size_t cap) {
  if (c->is_link) fm_snprintf(out, cap, "link:%s", c->link.id);
  else fm_snprintf(out, cap, "sid:%s", c->api.sid);
}

/* Takes the account's tree (fetching it when missing, stale or `fresh`);
** give it back with tree_put. */
static FmErr tree_get(MgCtx *c, bool fresh, MgCache **out) {
  char key[200];
  cache_key(c, key, sizeof key);
  u64 now = plat_now_ms();
  MgCache *e = NULL;
  SDL_AtomicLock(&g_cache_lock);
  for (int i = 0; i < FM_COUNT(g_cache); i++)
    if (g_cache[i].key[0] && !strcmp(g_cache[i].key, key) && !g_cache[i].busy) { e = &g_cache[i]; break; }
  if (!e) {
    for (int i = 0; i < FM_COUNT(g_cache); i++) {
      MgCache *x = &g_cache[i];
      if (x->busy) continue;
      if (!e || !x->key[0] || (e->key[0] && x->used < e->used)) e = x;
      if (!x->key[0]) break;
    }
    if (e) {
      mega_tree_free(&e->tree);
      fm_strlcpy(e->key, key, sizeof e->key);
      e->stamp = 0;
    }
  }
  if (e) { e->busy = true; e->used = now; }
  SDL_AtomicUnlock(&g_cache_lock);
  if (!e) { fm_snprintf(c->api.err, c->api.errcap, "Too many MEGA transfers at once"); return FM_ERR_IO; }
  if (fresh || !e->stamp || now - e->stamp > MG_TREE_TTL_MS) {
    FmJson j;
    const FmJsonNode *r;
    FmErr err = mg_call(&c->api, c->is_link ? "{\"a\":\"f\",\"c\":1,\"ca\":1,\"r\":1}" : "{\"a\":\"f\",\"c\":1}", &j, &r,
                        MG_TREE_MAX_REPLY);
    if (err == FM_OK) {
      MgTree t;
      err = mega_tree_build(&t, r, c->is_link ? c->link.key : c->mk, c->is_link);
      if (err == FM_OK) {
        mega_tree_free(&e->tree);
        e->tree = t;
        e->stamp = plat_now_ms();
      } else {
        mega_tree_free(&t);
        fm_snprintf(c->api.err, c->api.errcap,
                    c->is_link ? "Could not open this MEGA folder (wrong key in the link?)" : "MEGA sent a file list this app cannot read");
      }
    }
    json_free(&j);
    if (err != FM_OK) {
      SDL_AtomicLock(&g_cache_lock);
      mega_tree_free(&e->tree);
      e->key[0] = 0;
      e->busy = false;
      SDL_AtomicUnlock(&g_cache_lock);
      return err;
    }
  }
  *out = e;
  return FM_OK;
}

static void tree_put(MgCache *e) {
  SDL_AtomicLock(&g_cache_lock);
  e->busy = false;
  SDL_AtomicUnlock(&g_cache_lock);
}

/* the folder an id names: "" = the drive / the link's top folder */
static MgNode *tree_dir(MgTree *t, const MgLink *link, const char *id) {
  if (!id || !*id) id = link && link->sub[0] ? link->sub : t->root;
  size_t l = strlen(id);
  char h[16];
  fm_strlcpy(h, id, sizeof h);
  if (l && l < sizeof h && h[l - 1] == '/') h[l - 1] = 0;
  MgNode *n = mega_tree_find(t, h);
  return n && n->type != MG_FILE ? n : NULL;
}

static void entry_from(const MgNode *n, FmCloudEntry *e) {
  memset(e, 0, sizeof *e);
  fm_strlcpy(e->id, n->h, sizeof e->id);
  fm_strlcpy(e->name, n->name, sizeof e->name);
  e->dir = n->type != MG_FILE;
  e->size = e->dir ? 0 : n->size;
  e->mtime = n->mtime ? n->mtime : n->ts;
}

/* ---- login ----------------------------------------------------------------------------- */

static FmErr mg_login(FmCloudAcct *a, char *err, size_t errcap, volatile int *cancel) {
  MgApi api;
  memset(&api, 0, sizeof api);
  api.cancel = cancel;
  api.err = err;
  api.errcap = errcap;
  char email[256], esc[600], cmd[900];
  fm_strlcpy(email, a->user, sizeof email);
  for (char *p = email; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
  /* trim spaces around the address */
  char *s = email;
  while (*s == ' ') s++;
  memmove(email, s, strlen(s) + 1);
  for (size_t l = strlen(email); l && email[l - 1] == ' ';) email[--l] = 0;
  if (!email[0] || !a->secret[0]) {
    fm_snprintf(err, errcap, "Enter your MEGA email and password");
    return FM_ERR_PASSWORD;
  }
  json_esc(email, esc, sizeof esc);
  fm_snprintf(cmd, sizeof cmd, "{\"a\":\"us0\",\"user\":\"%s\"}", esc);
  FmJson j;
  const FmJsonNode *r;
  FmErr e = mg_call(&api, cmd, &j, &r, 0);
  if (e != FM_OK) return e;
  int ver = (int)json_num(json_get(r, "v"), 0);
  u8 salt[64];
  int sl = mega_b64_dec(jstr(r, "s"), salt, sizeof salt);
  json_free(&j);
  u8 pk[16];
  char uh[40];
  if (ver == 1) {
    mega_prepare_key(a->secret, pk);
    mega_stringhash(email, pk, uh);
  } else if (ver == 2 && sl > 0) {
    mega_v2_derive(a->secret, salt, (size_t)sl, pk, uh);
  } else {
    fm_snprintf(err, errcap, "This MEGA account type (version %d) is not supported", ver);
    return FM_ERR_UNSUPPORTED;
  }
  if (cancel && *cancel) { wipe(pk, sizeof pk); return FM_ERR_CANCEL; }
  fm_snprintf(cmd, sizeof cmd, "{\"a\":\"us\",\"user\":\"%s\",\"uh\":\"%s\"}", esc, uh);
  e = mg_call(&api, cmd, &j, &r, 0);
  if (e != FM_OK) {
    wipe(pk, sizeof pk);
    if (e == FM_ERR_NOT_FOUND) {
      fm_snprintf(err, errcap, "Wrong MEGA email or password");
      return FM_ERR_PASSWORD;
    }
    return e;
  }
  char sid[160];
  u8 mk[16];
  e = mega_login_reply(r, pk, sid, sizeof sid, mk, err, errcap);
  json_free(&j);
  wipe(pk, sizeof pk);
  if (e != FM_OK) return e;
  char mk64[32];
  mega_b64_enc(mk, 16, mk64, sizeof mk64);
  fm_snprintf(a->session, sizeof a->session, "sid=%s\nmk=%s\n", sid, mk64);
  wipe(mk, sizeof mk);
  wipe(mk64, sizeof mk64);
  wipe(sid, sizeof sid);
  wipe(a->secret, sizeof a->secret);
  a->session_changed = true;
  return FM_OK;
}

/* ---- list ------------------------------------------------------------------------------- */

/* file link: the one file, from "g" with p= */
static FmErr link_file_info(MgCtx *c, FmCloudEntry *e, char *url, size_t urlcap) {
  char cmd[128];
  fm_snprintf(cmd, sizeof cmd, "{\"a\":\"g\",\"p\":\"%s\"%s}", c->link.id, url ? ",\"g\":1,\"ssl\":2" : "");
  FmJson j;
  const FmJsonNode *r;
  FmErr err = mg_call(&c->api, cmd, &j, &r, 0);
  if (err != FM_OK) return err;
  MgNode n;
  memset(&n, 0, sizeof n);
  n.klen = 32;
  memcpy(n.key, c->link.key, 32);
  u8 ak[16];
  char name[256];
  mega_node_aes_key(&n, ak);
  bool ok = mega_attr_decrypt(ak, jstr(r, "at"), name, sizeof name, NULL, 0);
  wipe(ak, sizeof ak);
  wipe(&n, sizeof n);
  int ecode = (int)json_num(json_get(r, "e"), 0);
  if (ecode < 0) err = mg_code(ecode, "file", c->api.err, c->api.errcap);
  else if (!ok) {
    fm_snprintf(c->api.err, c->api.errcap, "Could not open this MEGA file (wrong key in the link?)");
    err = FM_ERR_PASSWORD;
  } else {
    memset(e, 0, sizeof *e);
    fm_strlcpy(e->id, c->link.id, sizeof e->id);
    fm_strlcpy(e->name, name, sizeof e->name);
    e->size = (u64)json_num(json_get(r, "s"), 0);
    if (url) {
      const FmJsonNode *g = json_get(r, "g");
      if (!g || g->type != JSON_STR) {
        fm_snprintf(c->api.err, c->api.errcap, "MEGA gave no download address for this file");
        err = FM_ERR_UNSUPPORTED;
      } else {
        fm_strlcpy(url, g->s, urlcap);
      }
    }
  }
  json_free(&j);
  return err;
}

static FmErr mg_list(FmCloudAcct *a, const char *dir_id, FmCloudList *out, volatile int *cancel) {
  MgCtx c;
  FmErr err = ctx_open(a, &c, out->error, sizeof out->error, cancel);
  if (err != FM_OK) return err;
  if (c.is_link && !c.link.folder) {
    FmCloudEntry e;
    err = link_file_info(&c, &e, NULL, 0);
    if (err == FM_OK) *cloud_list_add(out) = e;
    ctx_close(&c);
    return err;
  }
  /* from the cached tree (refetched when older than MG_TREE_TTL_MS) */
  MgCache *ce;
  err = tree_get(&c, false, &ce);
  if (err == FM_OK) {
    MgTree *t = &ce->tree;
    MgNode *d = tree_dir(t, c.is_link ? &c.link : NULL, dir_id);
    if (!d) {
      fm_snprintf(out->error, sizeof out->error, "This folder is no longer on MEGA");
      err = FM_ERR_NOT_FOUND;
    } else {
      char dh[12];
      fm_strlcpy(dh, d->h, sizeof dh);
      for (int i = 0; i < t->count; i++) {
        const MgNode *n = &t->n[i];
        if ((n->type == MG_FILE || n->type == MG_FOLDER) && !strcmp(n->p, dh)) entry_from(n, cloud_list_add(out));
      }
    }
    tree_put(ce);
  }
  ctx_close(&c);
  return err;
}

/* ---- download --------------------------------------------------------------------------- */

typedef struct MgDl {
  FmAes aes;
  u8 iv[16];
  MgMac mac;
  FILE *f;
  u64 pos, size;
  u8 *buf;
  bool bad_status, io_err;
  int status;
  FmNetProgress cb;
  void *user;
  volatile int *cancel;
} MgDl;

static void dl_head(void *u, const FmNetResp *r) {
  MgDl *d = (MgDl *)u;
  d->status = r->status;
  d->bad_status = r->status < 200 || r->status >= 300;
}

static bool dl_data(void *u, const u8 *p, size_t n) {
  MgDl *d = (MgDl *)u;
  if (d->bad_status || (d->cancel && *d->cancel)) return false;
  while (n && d->pos < d->size) {
    size_t take = (size_t)FM_MIN((u64)FM_MIN(n, (size_t)65536), d->size - d->pos);
    memcpy(d->buf, p, take);
    aes_ctr_be(&d->aes, d->iv, d->pos, d->buf, take);
    mega_mac_feed(&d->mac, d->buf, take);
    if (fwrite(d->buf, 1, take, d->f) != take) { d->io_err = true; return false; }
    d->pos += take;
    p += take;
    n -= take;
  }
  if (d->cb && !d->cb(d->user, d->pos, d->size)) return false;
  return true;
}

/* Streams url (the encrypted body) into path, decrypting and checking the MAC. */
static FmErr mg_fetch(const char *url, const u8 key32[32], u64 size, const char *path, FmNetProgress cb, void *user,
                      char *err, size_t errcap, volatile int *cancel) {
  char part[FM_PATH_MAX];
  fm_snprintf(part, sizeof part, "%s.part", path);
  MgDl *d = (MgDl *)fm_calloc(1, sizeof *d);
  u8 k[16];
  for (int i = 0; i < 16; i++) k[i] = (u8)(key32[i] ^ key32[16 + i]);
  aes_init(&d->aes, k, 16);
  wipe(k, sizeof k);
  memcpy(d->iv, key32 + 16, 8);
  mega_mac_init(&d->mac, &d->aes, key32 + 16);
  d->size = size;
  d->cb = cb;
  d->user = user;
  d->cancel = cancel;
  d->buf = (u8 *)fm_alloc(65536);
  d->f = fm_fopen(part, "wb");
  FmErr e = FM_OK;
  if (!d->f) {
    fm_snprintf(err, errcap, "Cannot create %s", part);
    e = FM_ERR_IO;
  }
  for (int attempt = 0; e == FM_OK && d->pos < size && attempt < 5; attempt++) {
    char u[1024];
    if (d->pos) fm_snprintf(u, sizeof u, "%s/%llu-%llu", url, (unsigned long long)d->pos, (unsigned long long)(size - 1));
    else fm_strlcpy(u, url, sizeof u);
    u64 before = d->pos;
    FmNetResp r;
    memset(&r, 0, sizeof r);
    FmErr ne = net_get_stream(u, NULL, dl_head, dl_data, d, &r, cancel);
    if (cancel && *cancel) e = FM_ERR_CANCEL;
    else if (d->io_err) { fm_snprintf(err, errcap, "Cannot write %s", part); e = FM_ERR_IO; }
    else if (d->bad_status) {
      if (d->status == 509) fm_snprintf(err, errcap, "MEGA's transfer quota is used up; try again later");
      else fm_snprintf(err, errcap, "MEGA refused the download (HTTP %d)", d->status);
      e = d->status == 509 ? FM_ERR_FULL : FM_ERR_IO;
    } else if (ne == FM_ERR_CANCEL && d->pos < size) {
      e = FM_ERR_CANCEL;                      /* the progress callback said stop */
    } else if (d->pos < size && d->pos == before && attempt >= 2) {
      fm_snprintf(err, errcap, "Network error: %s", r.error[0] ? r.error : "the download stopped");
      e = FM_ERR_IO;
    }
    net_resp_free(&r);
    if (e == FM_OK && d->pos < size) mg_sleep(500 * (attempt + 1), cancel);
  }
  if (e == FM_OK && d->pos < size) {
    fm_snprintf(err, errcap, "The download from MEGA stopped early");
    e = FM_ERR_IO;
  }
  if (d->f && fclose(d->f) != 0 && e == FM_OK) { fm_snprintf(err, errcap, "Cannot write %s", part); e = FM_ERR_IO; }
  if (e == FM_OK && size) {
    u8 meta[8];
    mega_mac_final(&d->mac, meta);
    if (memcmp(meta, key32 + 24, 8) != 0) {
      fm_snprintf(err, errcap, "The file from MEGA failed its integrity check (MAC mismatch)");
      e = FM_ERR_CRC;
    }
  }
  if (e == FM_OK) {
    plat_remove_file(path);
    e = plat_rename(part, path);
    if (e != FM_OK) fm_snprintf(err, errcap, "Cannot rename %s", part);
  }
  if (e != FM_OK) plat_remove_file(part);
  fm_free(d->buf);
  wipe(d, sizeof *d);
  fm_free(d);
  return e;
}

static FmErr mg_download(FmCloudAcct *a, const FmCloudEntry *ent, const char *local_path, FmNetProgress cb, void *user,
                         char *err, size_t errcap, volatile int *cancel) {
  MgCtx c;
  FmErr e = ctx_open(a, &c, err, errcap, cancel);
  if (e != FM_OK) return e;
  char url[1024];
  u8 key[32];
  u64 size = 0;
  i64 mtime = ent->mtime;
  if (c.is_link && !c.link.folder) {
    FmCloudEntry fe;
    e = link_file_info(&c, &fe, url, sizeof url);
    memcpy(key, c.link.key, 32);
    size = fe.size;
  } else {
    MgCache *ce;
    e = tree_get(&c, false, &ce);
    if (e == FM_OK) {
      MgNode *n = mega_tree_find(&ce->tree, ent->id);
      if (!n || n->type != MG_FILE || n->klen != 32) {
        fm_snprintf(err, errcap, "This file is no longer on MEGA");
        e = FM_ERR_NOT_FOUND;
      } else {
        memcpy(key, n->key, 32);
        size = n->size;
      }
      tree_put(ce);
    }
    if (e == FM_OK) {
      char cmd[128];
      FmJson j;
      const FmJsonNode *r;
      fm_snprintf(cmd, sizeof cmd, "{\"a\":\"g\",\"g\":1,\"ssl\":2,\"n\":\"%s\"}", ent->id);
      e = mg_call(&c.api, cmd, &j, &r, 0);
      if (e == FM_OK) {
        const FmJsonNode *g = json_get(r, "g");
        int ecode = (int)json_num(json_get(r, "e"), 0);
        if (ecode < 0) e = mg_code(ecode, "download", err, errcap);
        else if (!g || g->type != JSON_STR) {
          fm_snprintf(err, errcap, "MEGA gave no download address for this file");
          e = FM_ERR_UNSUPPORTED;
        } else {
          fm_strlcpy(url, g->s, sizeof url);
          size = (u64)json_num(json_get(r, "s"), (double)size);
        }
        json_free(&j);
      }
    }
  }
  if (e == FM_OK) {
    if (size == 0) {
      /* nothing to fetch: an empty file */
      FILE *f = fm_fopen(local_path, "wb");
      if (!f || fclose(f) != 0) { fm_snprintf(err, errcap, "Cannot create %s", local_path); e = FM_ERR_IO; }
    } else {
      e = mg_fetch(url, key, size, local_path, cb, user, err, errcap, cancel);
    }
    if (e == FM_OK && mtime > 0) plat_set_mtime(local_path, mtime);
  }
  wipe(key, sizeof key);
  ctx_close(&c);
  return e;
}

/* ---- tree edits ---------------------------------------------------------------------------- */

/* adds the node(s) of a "p" reply ({"f":[...]}) to the cached tree; *first gets the first */
static void tree_merge(MgCache *ce, const MgCtx *c, const FmJsonNode *r, FmCloudEntry *first) {
  MgTree add;
  /* reuse the builder on just these nodes; the parent is in our tree, so
  ** "root" detection fails harmlessly (FM_ERR_FORMAT) for accounts */
  mega_tree_build(&add, r, c->mk, false);
  for (int i = 0; i < add.count; i++) {
    const MgNode *s = &add.n[i];
    if (mega_tree_find(&ce->tree, s->h)) continue;
    MgNode tmp = *s;
    MgNode *n = mega_tree_add(&ce->tree, s->h);
    tmp.name = arena_strdup(&ce->tree.strings, s->name);
    tmp.fp = arena_strdup(&ce->tree.strings, s->fp);
    *n = tmp;
    if (first && i == 0) entry_from(n, first);
  }
  mega_tree_free(&add);
}

static FmErr edit_begin(FmCloudAcct *a, MgCtx *c, MgCache **ce, char *err, size_t errcap, volatile int *cancel) {
  FmErr e = ctx_open(a, c, err, errcap, cancel);
  if (e != FM_OK) return e;
  if (c->is_link) {
    fm_snprintf(err, errcap, "A public MEGA link is read-only");
    ctx_close(c);
    return FM_ERR_ACCESS;
  }
  e = tree_get(c, false, ce);
  if (e != FM_OK) ctx_close(c);
  return e;
}

static void edit_end(MgCtx *c, MgCache *ce) {
  tree_put(ce);
  ctx_close(c);
}

/* a "p" (put nodes) call: one node under parent */
static FmErr put_node(MgCtx *c, MgCache *ce, const char *parent, const char *h, int type, const char *attr,
                      const u8 *key, int klen, const char *ov, FmCloudEntry *out) {
  u8 ek[32];
  char k64[64], cmd[2048 + 512];
  memcpy(ek, key, (size_t)klen);
  FmAes ma;
  aes_init(&ma, c->mk, 16);
  ecb_enc(&ma, ek, (size_t)klen);
  wipe(&ma, sizeof ma);
  mega_b64_enc(ek, (size_t)klen, k64, sizeof k64);
  size_t need = strlen(attr) + 256;
  char *buf = need > sizeof cmd ? (char *)fm_alloc(need) : cmd;
  size_t cap = need > sizeof cmd ? need : sizeof cmd;
  char ovs[40] = "";
  if (ov && *ov) fm_snprintf(ovs, sizeof ovs, ",\"ov\":\"%s\"", ov);
  fm_snprintf(buf, cap, "{\"a\":\"p\",\"t\":\"%s\",\"n\":[{\"h\":\"%s\",\"t\":%d,\"a\":\"%s\",\"k\":\"%s\"%s}]}", parent, h,
              type, attr, k64, ovs);
  FmJson j;
  const FmJsonNode *r;
  FmErr e = mg_call(&c->api, buf, &j, &r, 0);
  if (buf != cmd) fm_free(buf);
  if (e == FM_OK) {
    FmCloudEntry first;
    memset(&first, 0, sizeof first);
    tree_merge(ce, c, r, &first);
    if (ov && *ov) {
      MgNode *old = mega_tree_find(&ce->tree, ov);
      if (old) fm_strlcpy(old->p, first.id, sizeof old->p);    /* versions hang off the new node */
    }
    if (out) *out = first;
    json_free(&j);
  }
  wipe(ek, sizeof ek);
  return e;
}

static MgNode *child_named(MgTree *t, const char *dir, const char *name, int type) {
  for (int i = 0; i < t->count; i++)
    if (t->n[i].type == type && !strcmp(t->n[i].p, dir) && !strcmp(t->n[i].name, name)) return &t->n[i];
  return NULL;
}

static FmErr mg_mkdir(FmCloudAcct *a, const char *parent_id, const char *name, FmCloudEntry *out, char *err, size_t errcap,
                      volatile int *cancel) {
  MgCtx c;
  MgCache *ce;
  FmErr e = edit_begin(a, &c, &ce, err, errcap, cancel);
  if (e != FM_OK) return e;
  MgNode *d = tree_dir(&ce->tree, NULL, parent_id);
  if (!d) {
    fm_snprintf(err, errcap, "The folder is no longer on MEGA");
    e = FM_ERR_NOT_FOUND;
  } else {
    char dh[12];
    fm_strlcpy(dh, d->h, sizeof dh);
    u8 key[16];
    plat_random(key, sizeof key);
    size_t acap = strlen(name) * 8 + 128;
    char *attr = (char *)fm_alloc(acap);
    mega_attr_encrypt(key, name, NULL, attr, acap);
    e = put_node(&c, ce, dh, "xxxxxxxx", MG_FOLDER, attr, key, 16, NULL, out);
    fm_free(attr);
    wipe(key, sizeof key);
  }
  edit_end(&c, ce);
  return e;
}

static FmErr simple_cmd(MgCtx *c, const char *cmd, const char *what) {
  FmJson j;
  const FmJsonNode *r;
  FmErr e = mg_call(&c->api, cmd, &j, &r, 0);
  if (e == FM_OK) {
    if (r->type == JSON_NUM && r->n < 0) e = mg_code((int)r->n, what, c->api.err, c->api.errcap);
    json_free(&j);
  }
  return e;
}

static FmErr mg_move(FmCloudAcct *a, const FmCloudEntry *ent, const char *new_parent_id, char *err, size_t errcap,
                     volatile int *cancel) {
  MgCtx c;
  MgCache *ce;
  FmErr e = edit_begin(a, &c, &ce, err, errcap, cancel);
  if (e != FM_OK) return e;
  MgNode *d = tree_dir(&ce->tree, NULL, new_parent_id);
  MgNode *n = mega_tree_find(&ce->tree, ent->id);
  if (!d || !n) {
    fm_snprintf(err, errcap, "The item is no longer on MEGA");
    e = FM_ERR_NOT_FOUND;
  } else {
    char cmd[128];
    fm_snprintf(cmd, sizeof cmd, "{\"a\":\"m\",\"n\":\"%s\",\"t\":\"%s\"}", n->h, d->h);
    e = simple_cmd(&c, cmd, "move");
    if (e == FM_OK) fm_strlcpy(n->p, d->h, sizeof n->p);
  }
  edit_end(&c, ce);
  return e;
}

static FmErr mg_remove(FmCloudAcct *a, const FmCloudEntry *ent, char *err, size_t errcap, volatile int *cancel) {
  MgCtx c;
  MgCache *ce;
  FmErr e = edit_begin(a, &c, &ce, err, errcap, cancel);
  if (e != FM_OK) return e;
  MgNode *n = mega_tree_find(&ce->tree, ent->id);
  if (!n) {
    fm_snprintf(err, errcap, "The item is no longer on MEGA");
    e = FM_ERR_NOT_FOUND;
  } else {
    char cmd[128];
    if (ce->tree.rubbish[0]) fm_snprintf(cmd, sizeof cmd, "{\"a\":\"m\",\"n\":\"%s\",\"t\":\"%s\"}", n->h, ce->tree.rubbish);
    else fm_snprintf(cmd, sizeof cmd, "{\"a\":\"d\",\"n\":\"%s\"}", n->h);
    e = simple_cmd(&c, cmd, "remove");
    if (e == FM_OK) fm_strlcpy(n->p, ce->tree.rubbish[0] ? ce->tree.rubbish : "-", sizeof n->p);
  }
  edit_end(&c, ce);
  return e;
}

static FmErr mg_rename(FmCloudAcct *a, const FmCloudEntry *ent, const char *new_name, char *err, size_t errcap,
                       volatile int *cancel) {
  MgCtx c;
  MgCache *ce;
  FmErr e = edit_begin(a, &c, &ce, err, errcap, cancel);
  if (e != FM_OK) return e;
  MgNode *n = mega_tree_find(&ce->tree, ent->id);
  if (!n || !n->klen) {
    fm_snprintf(err, errcap, "The item is no longer on MEGA");
    e = FM_ERR_NOT_FOUND;
  } else {
    u8 k[16];
    mega_node_aes_key(n, k);
    size_t acap = strlen(new_name) * 8 + strlen(n->fp) + 128;
    char *attr = (char *)fm_alloc(acap), *cmd = (char *)fm_alloc(acap + 128);
    mega_attr_encrypt(k, new_name, n->fp, attr, acap);
    wipe(k, sizeof k);
    fm_snprintf(cmd, acap + 128, "{\"a\":\"a\",\"n\":\"%s\",\"at\":\"%s\"}", n->h, attr);
    e = simple_cmd(&c, cmd, "rename");
    if (e == FM_OK) n->name = arena_strdup(&ce->tree.strings, new_name);
    fm_free(attr);
    fm_free(cmd);
  }
  edit_end(&c, ce);
  return e;
}

static FmErr mg_quota(FmCloudAcct *a, u64 *used, u64 *total, char *err, size_t errcap, volatile int *cancel) {
  MgCtx c;
  FmErr e = ctx_open(a, &c, err, errcap, cancel);
  if (e != FM_OK) return e;
  *used = *total = 0;
  if (c.is_link) { ctx_close(&c); return FM_OK; }
  FmJson j;
  const FmJsonNode *r;
  e = mg_call(&c.api, "{\"a\":\"uq\",\"strg\":1,\"xfer\":0}", &j, &r, 0);
  if (e == FM_OK) {
    *used = (u64)json_num(json_get(r, "cstrg"), 0);
    *total = (u64)json_num(json_get(r, "mstrg"), 0);
    json_free(&j);
  }
  ctx_close(&c);
  return e;
}

/* ---- upload ------------------------------------------------------------------------------ */

typedef struct UpProg { FmNetProgress cb; void *user; u64 base, total, len; } UpProg;

static bool up_progress(void *u, u64 done, u64 total) {
  UpProg *p = (UpProg *)u;
  if (total != p->len) return true;         /* the reply, not our chunk */
  if (p->cb) return p->cb(p->user, p->base + done, p->total);
  return true;
}

static FmErr mg_upload(FmCloudAcct *a, const char *dir_id, const char *local_path, const char *name, FmCloudEntry *out,
                       FmNetProgress cb, void *user, char *err, size_t errcap, volatile int *cancel) {
  FmStat st;
  if (!plat_stat(local_path, &st) || (st.flags & FM_ST_DIR)) {
    fm_snprintf(err, errcap, "Cannot read %s", local_path);
    return FM_ERR_IO;
  }
  MgCtx c;
  MgCache *ce;
  FmErr e = edit_begin(a, &c, &ce, err, errcap, cancel);
  if (e != FM_OK) return e;
  char dh[12] = "", ov[12] = "";
  MgNode *d = tree_dir(&ce->tree, NULL, dir_id);
  if (d) {
    fm_strlcpy(dh, d->h, sizeof dh);
    MgNode *old = child_named(&ce->tree, dh, name, MG_FILE);
    if (old) fm_strlcpy(ov, old->h, sizeof ov);
  }
  /* the tree is not needed while the bytes go up: let others use the cache */
  tree_put(ce);
  if (!dh[0]) {
    fm_snprintf(err, errcap, "The folder is no longer on MEGA");
    ctx_close(&c);
    return FM_ERR_NOT_FOUND;
  }
  char fp[64] = "";
  mega_fingerprint(local_path, st.size, st.mtime, fp, sizeof fp);
  char cmd[160], url[1024] = "";
  FmJson j;
  const FmJsonNode *r;
  fm_snprintf(cmd, sizeof cmd, "{\"a\":\"u\",\"ssl\":2,\"s\":%llu}", (unsigned long long)st.size);
  e = mg_call(&c.api, cmd, &j, &r, 0);
  if (e == FM_OK) {
    fm_strlcpy(url, jstr(r, "p"), sizeof url);
    json_free(&j);
    if (!url[0]) { fm_snprintf(err, errcap, "MEGA gave no upload address"); e = FM_ERR_FORMAT; }
  }
  u8 rk[24];                               /* AES key (16) + nonce (8) */
  plat_random(rk, sizeof rk);
  FmAes aes;
  aes_init(&aes, rk, 16);
  u8 iv[16];
  memset(iv, 0, sizeof iv);
  memcpy(iv, rk + 16, 8);
  MgMac mac;
  mega_mac_init(&mac, &aes, rk + 16);
  char handle[64] = "";
  FILE *f = e == FM_OK ? fm_fopen(local_path, "rb") : NULL;
  if (e == FM_OK && !f) { fm_snprintf(err, errcap, "Cannot read %s", local_path); e = FM_ERR_IO; }
  u8 *buf = (u8 *)fm_alloc(1048576);
  u64 pos = 0;
  UpProg up = { cb, user, 0, st.size, 0 };
  while (e == FM_OK) {
    u64 end = st.size ? mega_chunk_end(pos) : 0;
    if (end > st.size) end = st.size;
    size_t n = (size_t)(end - pos);
    if (n && fread(buf, 1, n, f) != n) { fm_snprintf(err, errcap, "Cannot read %s", local_path); e = FM_ERR_IO; break; }
    mega_mac_feed(&mac, buf, n);
    aes_ctr_be(&aes, iv, pos, buf, n);
    char cu[1100];
    fm_snprintf(cu, sizeof cu, "%s/%llu", url, (unsigned long long)pos);
    FmNetResp rs;
    FmErr ne = FM_ERR_IO;
    for (int attempt = 0; attempt < 4; attempt++) {
      FmNetReq rq;
      memset(&rq, 0, sizeof rq);
      rq.method = "POST";
      rq.headers = "Content-Type: application/octet-stream\r\n";
      rq.body = buf;
      rq.body_len = n;
      rq.max_reply = 4096;
      up.base = pos;
      up.len = n;
      rq.progress = up_progress;
      rq.user = &up;
      ne = net_request(cu, &rq, &rs, cancel);
      if (ne == FM_OK && rs.status >= 200 && rs.status < 300) break;
      if (ne == FM_ERR_CANCEL || (cancel && *cancel)) { ne = FM_ERR_CANCEL; break; }
      if (ne == FM_OK) {
        fm_snprintf(err, errcap, "MEGA refused the upload (HTTP %d)", rs.status);
        ne = FM_ERR_IO;
        if (rs.status < 500) break;
      } else {
        fm_snprintf(err, errcap, "Network error: %s", rs.error[0] ? rs.error : fm_err_str(ne));
      }
      net_resp_free(&rs);
      memset(&rs, 0, sizeof rs);
      mg_sleep(1000 * (attempt + 1), cancel);
    }
    if (ne != FM_OK) { net_resp_free(&rs); e = ne; break; }
    const char *body = rs.data ? (const char *)rs.data : "";
    if (body[0] == '-') e = mg_code(atoi(body), "upload", err, errcap);
    else if (body[0]) fm_strlcpy(handle, body, sizeof handle);
    net_resp_free(&rs);
    pos = end;
    if (cb && !cb(user, pos, st.size)) e = FM_ERR_CANCEL;
    if (pos >= st.size) break;
  }
  if (f) fclose(f);
  fm_free(buf);
  for (char *p = handle; *p; p++) if (*p == '\r' || *p == '\n' || *p == ' ') { *p = 0; break; }
  if (e == FM_OK && !handle[0]) { fm_snprintf(err, errcap, "MEGA did not confirm the upload"); e = FM_ERR_IO; }
  if (e == FM_OK) {
    u8 meta[8], nk[32];
    mega_mac_final(&mac, meta);
    for (int i = 0; i < 16; i++) nk[i] = rk[i];
    for (int i = 0; i < 8; i++) { nk[i] ^= rk[16 + i]; nk[8 + i] ^= meta[i]; }
    memcpy(nk + 16, rk + 16, 8);
    memcpy(nk + 24, meta, 8);
    size_t acap = strlen(name) * 8 + 256;
    char *attr = (char *)fm_alloc(acap);
    mega_attr_encrypt(rk, name, fp, attr, acap);
    /* back to the tree for the put (the cache entry may have been refetched meanwhile) */
    e = tree_get(&c, false, &ce);
    if (e == FM_OK) {
      e = put_node(&c, ce, dh, handle, MG_FILE, attr, nk, 32, ov[0] ? ov : NULL, out);
      tree_put(ce);
    }
    fm_free(attr);
    wipe(nk, sizeof nk);
  }
  wipe(rk, sizeof rk);
  wipe(&aes, sizeof aes);
  wipe(&mac, sizeof mac);
  ctx_close(&c);
  return e;
}

/* ---- public links ------------------------------------------------------------------------ */

static FmErr mg_open_link(const char *link, FmCloudAcct *a, FmCloudEntry *root, char *err, size_t errcap,
                          volatile int *cancel) {
  MgLink l;
  if (!mega_parse_link(link, &l)) {
    fm_snprintf(err, errcap, "This is not a MEGA link with its key (mega.nz/file/...#key or mega.nz/folder/...#key)");
    return FM_ERR_FORMAT;
  }
  memset(a, 0, sizeof *a);
  fm_strlcpy(a->provider, "mega", sizeof a->provider);
  char k64[64];
  mega_b64_enc(l.key, (size_t)l.klen, k64, sizeof k64);
  fm_snprintf(a->session, sizeof a->session, "link=%s\nid=%s\nk=%s\n", l.folder ? "folder" : "file", l.id, k64);
  memset(root, 0, sizeof *root);
  root->dir = true;
  MgCtx c;
  FmErr e = ctx_open(a, &c, err, errcap, cancel);
  if (e != FM_OK) return e;
  if (!l.folder) {
    FmCloudEntry fe;
    e = link_file_info(&c, &fe, NULL, 0);
    if (e == FM_OK) {
      fm_strlcpy(root->name, fe.name, sizeof root->name);
      fm_strlcpy(a->label, fe.name, sizeof a->label);
    }
  } else {
    fm_strlcpy(c.link.sub, l.sub, sizeof c.link.sub);
    MgCache *ce;
    e = tree_get(&c, true, &ce);
    if (e == FM_OK) {
      MgNode *n = mega_tree_find(&ce->tree, l.sub[0] ? l.sub : ce->tree.root);
      if (!n) {
        fm_snprintf(err, errcap, "This folder is not in the MEGA link");
        e = FM_ERR_NOT_FOUND;
      } else {
        if (n->type != MG_FILE) fm_strlcpy(root->id, l.sub, sizeof root->id);
        fm_strlcpy(root->name, n->name, sizeof root->name);
        MgNode *top = mega_tree_find(&ce->tree, ce->tree.root);
        fm_strlcpy(a->label, top ? top->name : n->name, sizeof a->label);
      }
      tree_put(ce);
    }
  }
  ctx_close(&c);
  return e;
}

const FmCloud g_cloud_mega = {
  "mega", "MEGA", IC_CLOUD, CLOUD_PASSWORD | CLOUD_UPLOAD | CLOUD_LINKS,
  mg_login, mg_list, mg_download, mg_upload, mg_mkdir, mg_remove, mg_rename, mg_move, mg_quota, NULL, mg_open_link,
  "Sign in with your MEGA email and password, or open a public mega.nz link",
};
