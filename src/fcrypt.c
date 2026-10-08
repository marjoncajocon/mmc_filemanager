/* fcrypt.c -- AES, SHA-1, SHA-256, HMAC, PBKDF2, ZipCrypto and CRC32.
**
** Design decisions:
**   - Portable C first: table AES (one encrypt and one decrypt table,
**     rotated for the other three columns), slicing-by-8 CRC32. The tables
**     are built once at first use (smaller source than literal tables);
**     the ready flag is published with release/acquire so a thread never
**     sees the flag before the tables.
**   - Hardware paths come from fasm.h (AES-NI, ARMv8 AES/CRC) and are
**     chosen in aes_init / per call. The key schedule is always computed in
**     C; for the hardware path it is only re-laid out as bytes.
**   - PBKDF2 keeps the HMAC state after the key and copies it per
**     iteration: two SHA-1 blocks per round instead of four.
**   - Secrets on the stack are wiped with wipe(), whose volatile stores
**     the compiler may not drop.
*/
#include "fcrypt.h"
#include "fasm.h"

/* ---- tables ------------------------------------------------------------- */

static u32 g_crc[8][256];
static u8 g_sbox[256], g_isbox[256];
static u32 g_te[256], g_td[256];
static volatile int g_tables_ready;

static u8 gmul(u8 a, u8 b) {
  u8 r = 0;
  while (b) {
    if (b & 1) r ^= a;
    a = (u8)((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
    b >>= 1;
  }
  return r;
}

static u8 rotl8(u8 x, int s) { return (u8)((x << s) | (x >> (8 - s))); }

static void tables_build(void) {
  for (u32 i = 0; i < 256; i++) {
    u32 c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    g_crc[0][i] = c;
  }
  for (int t = 1; t < 8; t++)
    for (int i = 0; i < 256; i++)
      g_crc[t][i] = (g_crc[t - 1][i] >> 8) ^ g_crc[0][g_crc[t - 1][i] & 0xFF];

  /* S-box: walk the multiplicative group with generator 3 and its inverse. */
  u8 p = 1, q = 1;
  do {
    p = (u8)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
    q = (u8)(q ^ (q << 1));
    q = (u8)(q ^ (q << 2));
    q = (u8)(q ^ (q << 4));
    if (q & 0x80) q ^= 0x09;
    u8 x = (u8)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4));
    g_sbox[p] = (u8)(x ^ 0x63);
  } while (p != 1);
  g_sbox[0] = 0x63;
  for (int i = 0; i < 256; i++) g_isbox[g_sbox[i]] = (u8)i;
  for (int i = 0; i < 256; i++) {
    u8 s = g_sbox[i], v = g_isbox[i];
    g_te[i] = ((u32)gmul(s, 2) << 24) | ((u32)s << 16) | ((u32)s << 8) | gmul(s, 3);
    g_td[i] = ((u32)gmul(v, 14) << 24) | ((u32)gmul(v, 9) << 16) | ((u32)gmul(v, 13) << 8) |
              gmul(v, 11);
  }
}

static void tables_init(void) {
#if defined(__GNUC__) || defined(__clang__)
  if (__atomic_load_n(&g_tables_ready, __ATOMIC_ACQUIRE)) return;
  tables_build();
  __atomic_store_n(&g_tables_ready, 1, __ATOMIC_RELEASE);
#else
  /* tcc builds are x86 only, where plain stores are not reordered. */
  if (g_tables_ready) return;
  tables_build();
  g_tables_ready = 1;
#endif
}

/* ---- CRC32 -------------------------------------------------------------- */

static u32 ld32le(const u8 *p) {
  return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

u32 crc32_update(u32 crc, const void *data, size_t n) {
  const u8 *p = (const u8 *)data;
#if defined(FM_ASM) && defined(FASM_HAVE_CRC)
  if (fasm_cpu() & FASM_CRC) return fasm_crc32(crc, p, n);
#endif
  tables_init();
  crc = ~crc;
  while (n && ((uintptr_t)p & 7)) {
    crc = (crc >> 8) ^ g_crc[0][(crc ^ *p++) & 0xFF];
    n--;
  }
  while (n >= 8) {
    u32 a = ld32le(p) ^ crc, b = ld32le(p + 4);
    crc = g_crc[7][a & 0xFF] ^ g_crc[6][(a >> 8) & 0xFF] ^ g_crc[5][(a >> 16) & 0xFF] ^
          g_crc[4][a >> 24] ^ g_crc[3][b & 0xFF] ^ g_crc[2][(b >> 8) & 0xFF] ^
          g_crc[1][(b >> 16) & 0xFF] ^ g_crc[0][b >> 24];
    p += 8;
    n -= 8;
  }
  while (n--) crc = (crc >> 8) ^ g_crc[0][(crc ^ *p++) & 0xFF];
  return ~crc;
}

/* ---- AES ---------------------------------------------------------------- */

static u32 ld32be(const u8 *p) {
  return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
static void st32be(u8 *p, u32 v) {
  p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}
static u32 ror32(u32 v, int s) { return (v >> s) | (v << (32 - s)); }

#define TE0(x) g_te[x]
#define TE1(x) ror32(g_te[x], 8)
#define TE2(x) ror32(g_te[x], 16)
#define TE3(x) ror32(g_te[x], 24)
#define TD0(x) g_td[x]
#define TD1(x) ror32(g_td[x], 8)
#define TD2(x) ror32(g_td[x], 16)
#define TD3(x) ror32(g_td[x], 24)

static u32 sub_word(u32 w) {
  return ((u32)g_sbox[w >> 24] << 24) | ((u32)g_sbox[(w >> 16) & 0xFF] << 16) |
         ((u32)g_sbox[(w >> 8) & 0xFF] << 8) | g_sbox[w & 0xFF];
}

void aes_init_soft(FmAes *a, const u8 *key, int key_bytes) {
  static const u8 rcon[10] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36 };
  tables_init();
  int nk = key_bytes / 4;
  if (nk != 4 && nk != 6 && nk != 8) nk = 4;
  a->rounds = nk + 6;
  a->hw = 0;
  int total = 4 * (a->rounds + 1);
  u32 *rk = a->ek;
  for (int i = 0; i < nk; i++) rk[i] = ld32be(key + 4 * i);
  for (int i = nk; i < total; i++) {
    u32 t = rk[i - 1];
    if (i % nk == 0) t = sub_word((t << 8) | (t >> 24)) ^ ((u32)rcon[i / nk - 1] << 24);
    else if (nk == 8 && i % nk == 4) t = sub_word(t);
    rk[i] = rk[i - nk] ^ t;
  }
  /* Equivalent inverse cipher: reversed round keys, InvMixColumns on the
  ** middle ones (Td applied to S-box output undoes the S-box). */
  for (int r = 0; r <= a->rounds; r++) {
    for (int j = 0; j < 4; j++) {
      u32 w = rk[4 * (a->rounds - r) + j];
      if (r > 0 && r < a->rounds)
        w = TD0(g_sbox[w >> 24]) ^ TD1(g_sbox[(w >> 16) & 0xFF]) ^
            TD2(g_sbox[(w >> 8) & 0xFF]) ^ TD3(g_sbox[w & 0xFF]);
      a->dk[4 * r + j] = w;
    }
  }
}

void aes_init(FmAes *a, const u8 *key, int key_bytes) {
  aes_init_soft(a, key, key_bytes);
#ifdef FM_ASM
  if (fasm_cpu() & FASM_AES) {
    int total = 4 * (a->rounds + 1);
    for (int i = 0; i < total; i++) {
      u8 b[4];
      st32be(b, a->ek[i]);
      memcpy(&a->ek[i], b, 4);
      st32be(b, a->dk[i]);
      memcpy(&a->dk[i], b, 4);
    }
    a->hw = 1;
  }
#endif
}

void aes_encrypt_block(const FmAes *a, const u8 in[16], u8 out[16]) {
#ifdef FM_ASM
  if (a->hw) { fasm_aes_enc((const u8 *)a->ek, a->rounds, in, out); return; }
#endif
  const u32 *rk = a->ek;
  u32 s0 = ld32be(in) ^ rk[0], s1 = ld32be(in + 4) ^ rk[1];
  u32 s2 = ld32be(in + 8) ^ rk[2], s3 = ld32be(in + 12) ^ rk[3];
  for (int r = 1; r < a->rounds; r++) {
    rk += 4;
    u32 t0 = TE0(s0 >> 24) ^ TE1((s1 >> 16) & 0xFF) ^ TE2((s2 >> 8) & 0xFF) ^ TE3(s3 & 0xFF) ^ rk[0];
    u32 t1 = TE0(s1 >> 24) ^ TE1((s2 >> 16) & 0xFF) ^ TE2((s3 >> 8) & 0xFF) ^ TE3(s0 & 0xFF) ^ rk[1];
    u32 t2 = TE0(s2 >> 24) ^ TE1((s3 >> 16) & 0xFF) ^ TE2((s0 >> 8) & 0xFF) ^ TE3(s1 & 0xFF) ^ rk[2];
    u32 t3 = TE0(s3 >> 24) ^ TE1((s0 >> 16) & 0xFF) ^ TE2((s1 >> 8) & 0xFF) ^ TE3(s2 & 0xFF) ^ rk[3];
    s0 = t0; s1 = t1; s2 = t2; s3 = t3;
  }
  rk += 4;
  const u8 *S = g_sbox;
  st32be(out, ((u32)S[s0 >> 24] << 24 | (u32)S[(s1 >> 16) & 0xFF] << 16 |
               (u32)S[(s2 >> 8) & 0xFF] << 8 | S[s3 & 0xFF]) ^ rk[0]);
  st32be(out + 4, ((u32)S[s1 >> 24] << 24 | (u32)S[(s2 >> 16) & 0xFF] << 16 |
                   (u32)S[(s3 >> 8) & 0xFF] << 8 | S[s0 & 0xFF]) ^ rk[1]);
  st32be(out + 8, ((u32)S[s2 >> 24] << 24 | (u32)S[(s3 >> 16) & 0xFF] << 16 |
                   (u32)S[(s0 >> 8) & 0xFF] << 8 | S[s1 & 0xFF]) ^ rk[2]);
  st32be(out + 12, ((u32)S[s3 >> 24] << 24 | (u32)S[(s0 >> 16) & 0xFF] << 16 |
                    (u32)S[(s1 >> 8) & 0xFF] << 8 | S[s2 & 0xFF]) ^ rk[3]);
}

void aes_decrypt_block(const FmAes *a, const u8 in[16], u8 out[16]) {
#ifdef FM_ASM
  if (a->hw) { fasm_aes_dec((const u8 *)a->dk, a->rounds, in, out); return; }
#endif
  const u32 *rk = a->dk;
  u32 s0 = ld32be(in) ^ rk[0], s1 = ld32be(in + 4) ^ rk[1];
  u32 s2 = ld32be(in + 8) ^ rk[2], s3 = ld32be(in + 12) ^ rk[3];
  for (int r = 1; r < a->rounds; r++) {
    rk += 4;
    u32 t0 = TD0(s0 >> 24) ^ TD1((s3 >> 16) & 0xFF) ^ TD2((s2 >> 8) & 0xFF) ^ TD3(s1 & 0xFF) ^ rk[0];
    u32 t1 = TD0(s1 >> 24) ^ TD1((s0 >> 16) & 0xFF) ^ TD2((s3 >> 8) & 0xFF) ^ TD3(s2 & 0xFF) ^ rk[1];
    u32 t2 = TD0(s2 >> 24) ^ TD1((s1 >> 16) & 0xFF) ^ TD2((s0 >> 8) & 0xFF) ^ TD3(s3 & 0xFF) ^ rk[2];
    u32 t3 = TD0(s3 >> 24) ^ TD1((s2 >> 16) & 0xFF) ^ TD2((s1 >> 8) & 0xFF) ^ TD3(s0 & 0xFF) ^ rk[3];
    s0 = t0; s1 = t1; s2 = t2; s3 = t3;
  }
  rk += 4;
  const u8 *S = g_isbox;
  st32be(out, ((u32)S[s0 >> 24] << 24 | (u32)S[(s3 >> 16) & 0xFF] << 16 |
               (u32)S[(s2 >> 8) & 0xFF] << 8 | S[s1 & 0xFF]) ^ rk[0]);
  st32be(out + 4, ((u32)S[s1 >> 24] << 24 | (u32)S[(s0 >> 16) & 0xFF] << 16 |
                   (u32)S[(s3 >> 8) & 0xFF] << 8 | S[s2 & 0xFF]) ^ rk[1]);
  st32be(out + 8, ((u32)S[s2 >> 24] << 24 | (u32)S[(s1 >> 16) & 0xFF] << 16 |
                   (u32)S[(s0 >> 8) & 0xFF] << 8 | S[s3 & 0xFF]) ^ rk[2]);
  st32be(out + 12, ((u32)S[s3 >> 24] << 24 | (u32)S[(s2 >> 16) & 0xFF] << 16 |
                    (u32)S[(s1 >> 8) & 0xFF] << 8 | S[s0 & 0xFF]) ^ rk[3]);
}

void aes_cbc_encrypt(const FmAes *a, u8 iv[16], u8 *buf, size_t n) {
  for (size_t off = 0; off + 16 <= n; off += 16) {
    for (int i = 0; i < 16; i++) buf[off + i] ^= iv[i];
    aes_encrypt_block(a, buf + off, buf + off);
    memcpy(iv, buf + off, 16);
  }
}

void aes_cbc_decrypt(const FmAes *a, u8 iv[16], u8 *buf, size_t n) {
  u8 c[16], p[16];
  for (size_t off = 0; off + 16 <= n; off += 16) {
    memcpy(c, buf + off, 16);
    aes_decrypt_block(a, c, p);
    for (int i = 0; i < 16; i++) buf[off + i] = p[i] ^ iv[i];
    memcpy(iv, c, 16);
  }
}

static void ctr_next(const FmAes *a, FmAesCtr *c) {
  for (int i = 0; i < 16; i++)
    if (++c->ctr[i]) break;
  aes_encrypt_block(a, c->ctr, c->ks);
}

void aes_ctr_le(const FmAes *a, FmAesCtr *c, u8 *buf, size_t n) {
  size_t i = 0;
  while (i < n && c->pos != 0) {          /* finish the current key block */
    buf[i++] ^= c->ks[c->pos];
    c->pos = (c->pos + 1) & 15;
  }
  while (n - i >= 16) {
    ctr_next(a, c);
    for (int k = 0; k < 16; k++) buf[i + k] ^= c->ks[k];
    i += 16;
  }
  if (i < n) {
    ctr_next(a, c);
    while (i < n) buf[i++] ^= c->ks[c->pos++];
  }
}

void wipe(void *p, size_t n) {
  volatile u8 *v = (volatile u8 *)p;
  while (n--) *v++ = 0;
}

/* ---- SHA-1 -------------------------------------------------------------- */

static u32 rol32(u32 v, int s) { return (v << s) | (v >> (32 - s)); }

static void sha1_block(u32 h[5], const u8 *p) {
  u32 w[80];
  for (int i = 0; i < 16; i++) w[i] = ld32be(p + 4 * i);
  for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], t;
  /* four loops, not one with a branch per round: about twice as fast */
#define SHA1_ROUND(f, k)                                             t = rol32(a, 5) + (f) + e + (k) + w[i];                          e = d; d = c; c = rol32(b, 30); b = a; a = t
  int i = 0;
  for (; i < 20; i++) { SHA1_ROUND(d ^ (b & (c ^ d)), 0x5A827999u); }
  for (; i < 40; i++) { SHA1_ROUND(b ^ c ^ d, 0x6ED9EBA1u); }
  for (; i < 60; i++) { SHA1_ROUND((b & c) | (d & (b | c)), 0x8F1BBCDCu); }
  for (; i < 80; i++) { SHA1_ROUND(b ^ c ^ d, 0xCA62C1D6u); }
#undef SHA1_ROUND
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1_init(FmSha1 *s) {
  static const u32 iv[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
  memcpy(s->h, iv, sizeof iv);
  s->len = 0;
  s->n = 0;
}

void sha1_update(FmSha1 *s, const void *data, size_t n) {
  const u8 *p = (const u8 *)data;
  s->len += n;
  if (s->n) {
    size_t take = FM_MIN(n, (size_t)(64 - s->n));
    memcpy(s->buf + s->n, p, take);
    s->n += (int)take; p += take; n -= take;
    if (s->n < 64) return;
    sha1_block(s->h, s->buf);
    s->n = 0;
  }
  for (; n >= 64; p += 64, n -= 64) sha1_block(s->h, p);
  memcpy(s->buf, p, n);
  s->n = (int)n;
}

void sha1_final(FmSha1 *s, u8 out[20]) {
  u64 bits = s->len * 8;
  u8 pad[72];
  size_t padn = (s->n < 56) ? (size_t)(56 - s->n) : (size_t)(120 - s->n);
  memset(pad, 0, sizeof pad);
  pad[0] = 0x80;
  for (int i = 0; i < 8; i++) pad[padn + i] = (u8)(bits >> (56 - 8 * i));
  sha1_update(s, pad, padn + 8);
  for (int i = 0; i < 5; i++) st32be(out + 4 * i, s->h[i]);
  wipe(s, sizeof *s);
}

/* ---- SHA-256 ------------------------------------------------------------ */

static const u32 kSha256K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_block(u32 h[8], const u8 *p) {
  u32 w[64];
  for (int i = 0; i < 16; i++) w[i] = ld32be(p + 4 * i);
  for (int i = 16; i < 64; i++) {
    u32 s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    u32 s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    u32 S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
    u32 ch = (e & f) ^ (~e & g);
    u32 t1 = hh + S1 + ch + kSha256K[i] + w[i];
    u32 S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
    u32 mj = (a & b) ^ (a & c) ^ (b & c);
    u32 t2 = S0 + mj;
    hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha256_init(FmSha256 *s) {
  static const u32 iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                             0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
  memcpy(s->h, iv, sizeof iv);
  s->len = 0;
  s->n = 0;
}

void sha256_update(FmSha256 *s, const void *data, size_t n) {
  const u8 *p = (const u8 *)data;
  s->len += n;
  if (s->n) {
    size_t take = FM_MIN(n, (size_t)(64 - s->n));
    memcpy(s->buf + s->n, p, take);
    s->n += (int)take; p += take; n -= take;
    if (s->n < 64) return;
    sha256_block(s->h, s->buf);
    s->n = 0;
  }
  for (; n >= 64; p += 64, n -= 64) sha256_block(s->h, p);
  memcpy(s->buf, p, n);
  s->n = (int)n;
}

void sha256_final(FmSha256 *s, u8 out[32]) {
  u64 bits = s->len * 8;
  u8 pad[72];
  size_t padn = (s->n < 56) ? (size_t)(56 - s->n) : (size_t)(120 - s->n);
  memset(pad, 0, sizeof pad);
  pad[0] = 0x80;
  for (int i = 0; i < 8; i++) pad[padn + i] = (u8)(bits >> (56 - 8 * i));
  sha256_update(s, pad, padn + 8);
  for (int i = 0; i < 8; i++) st32be(out + 4 * i, s->h[i]);
  wipe(s, sizeof *s);
}

/* ---- HMAC-SHA1, PBKDF2 -------------------------------------------------- */

void hmac_sha1_init(FmHmacSha1 *h, const u8 *key, size_t key_len) {
  u8 k[64], pad[64];
  memset(k, 0, sizeof k);
  if (key_len > 64) {
    FmSha1 s;
    sha1_init(&s);
    sha1_update(&s, key, key_len);
    sha1_final(&s, k);
  } else if (key_len) {
    memcpy(k, key, key_len);
  }
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
  sha1_init(&h->inner);
  sha1_update(&h->inner, pad, 64);
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5C;
  sha1_init(&h->outer);
  sha1_update(&h->outer, pad, 64);
  wipe(k, sizeof k);
  wipe(pad, sizeof pad);
}

void hmac_sha1_update(FmHmacSha1 *h, const void *data, size_t n) {
  sha1_update(&h->inner, data, n);
}

void hmac_sha1_final(FmHmacSha1 *h, u8 out[20]) {
  u8 d[20];
  sha1_final(&h->inner, d);
  sha1_update(&h->outer, d, 20);
  sha1_final(&h->outer, out);
  wipe(d, sizeof d);
}

void pbkdf2_sha1(const u8 *pw, size_t pw_len, const u8 *salt, size_t salt_len, u32 iters,
                 u8 *out, size_t out_len) {
  FmHmacSha1 base, h;
  u8 u[20], t[20], cnt[4];
  hmac_sha1_init(&base, pw, pw_len);
  for (u32 block = 1; out_len; block++) {
    st32be(cnt, block);
    h = base;
    hmac_sha1_update(&h, salt, salt_len);
    hmac_sha1_update(&h, cnt, 4);
    hmac_sha1_final(&h, u);
    memcpy(t, u, 20);
    for (u32 i = 1; i < iters; i++) {
      h = base;
      hmac_sha1_update(&h, u, 20);
      hmac_sha1_final(&h, u);
      for (int k = 0; k < 20; k++) t[k] ^= u[k];
    }
    size_t take = FM_MIN(out_len, (size_t)20);
    memcpy(out, t, take);
    out += take;
    out_len -= take;
  }
  wipe(&base, sizeof base);
  wipe(&h, sizeof h);
  wipe(u, sizeof u);
  wipe(t, sizeof t);
}

/* ---- ZipCrypto ---------------------------------------------------------- */

static void zc_update(FmZipCrypto *z, u8 c) {
  z->k0 = (z->k0 >> 8) ^ g_crc[0][(z->k0 ^ c) & 0xFF];
  z->k1 = (z->k1 + (z->k0 & 0xFF)) * 134775813u + 1;
  z->k2 = (z->k2 >> 8) ^ g_crc[0][(z->k2 ^ (z->k1 >> 24)) & 0xFF];
}

static u8 zc_byte(const FmZipCrypto *z) {
  u32 t = (z->k2 | 2) & 0xFFFF;
  return (u8)((t * (t ^ 1)) >> 8);
}

void zipcrypto_init(FmZipCrypto *z, const char *password) {
  tables_init();
  z->k0 = 0x12345678;
  z->k1 = 0x23456789;
  z->k2 = 0x34567890;
  for (const u8 *p = (const u8 *)password; *p; p++) zc_update(z, *p);
}

void zipcrypto_decrypt(FmZipCrypto *z, u8 *buf, size_t n) {
  for (size_t i = 0; i < n; i++) {
    u8 c = (u8)(buf[i] ^ zc_byte(z));
    zc_update(z, c);
    buf[i] = c;
  }
}

void zipcrypto_encrypt(FmZipCrypto *z, u8 *buf, size_t n) {
  for (size_t i = 0; i < n; i++) {
    u8 k = zc_byte(z);
    zc_update(z, buf[i]);
    buf[i] ^= k;
  }
}
