/* fbignum.c -- fixed-size big integers for RSA (see fbignum.h). */
#include "fbignum.h"

static void bn_trim(FmBn *a) {
  while (a->n > 0 && a->d[a->n - 1] == 0) a->n--;
}

bool bn_from_bytes(FmBn *r, const u8 *p, size_t len) {
  while (len && *p == 0) { p++; len--; }
  memset(r, 0, sizeof *r);
  if (len > BN_LIMBS * 4) return false;
  for (size_t i = 0; i < len; i++) {
    size_t bit = (len - 1 - i) * 8;
    r->d[bit / 32] |= (u32)p[i] << (bit % 32);
  }
  r->n = (int)((len + 3) / 4);
  bn_trim(r);
  return true;
}

int bn_bits(const FmBn *a) {
  if (!a->n) return 0;
  u32 top = a->d[a->n - 1];
  int b = 0;
  while (top) { b++; top >>= 1; }
  return (a->n - 1) * 32 + b;
}

size_t bn_to_bytes(const FmBn *a, u8 *out, size_t cap) {
  size_t len = (size_t)(bn_bits(a) + 7) / 8;
  if (len > cap) return 0;
  for (size_t i = 0; i < len; i++) {
    size_t bit = (len - 1 - i) * 8;
    out[i] = (u8)(a->d[bit / 32] >> (bit % 32));
  }
  return len;
}

int bn_cmp(const FmBn *a, const FmBn *b) {
  if (a->n != b->n) return a->n < b->n ? -1 : 1;
  for (int i = a->n - 1; i >= 0; i--)
    if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
  return 0;
}

/* a -= b, a >= b */
static void bn_sub_in(FmBn *a, const FmBn *b) {
  u64 borrow = 0;
  for (int i = 0; i < a->n; i++) {
    u64 s = (u64)a->d[i] - (i < b->n ? b->d[i] : 0) - borrow;
    a->d[i] = (u32)s;
    borrow = (s >> 63) & 1;
  }
  bn_trim(a);
}

/* a = 2a + bit; false on overflow */
static bool bn_shl1(FmBn *a, u32 bit) {
  u32 carry = bit;
  for (int i = 0; i < a->n; i++) {
    u32 v = a->d[i];
    a->d[i] = (v << 1) | carry;
    carry = v >> 31;
  }
  if (carry) {
    if (a->n >= BN_LIMBS) return false;
    a->d[a->n++] = carry;
  }
  return true;
}

bool bn_mul(FmBn *r, const FmBn *a, const FmBn *b) {
  memset(r, 0, sizeof *r);
  if (!a->n || !b->n) return true;
  if (a->n + b->n > BN_LIMBS) return false;
  for (int i = 0; i < a->n; i++) {
    u64 c = 0;
    for (int j = 0; j < b->n; j++) {
      u64 t = (u64)a->d[i] * b->d[j] + r->d[i + j] + c;
      r->d[i + j] = (u32)t;
      c = t >> 32;
    }
    r->d[i + b->n] = (u32)c;
  }
  r->n = a->n + b->n;
  bn_trim(r);
  return true;
}

void bn_mod(FmBn *r, const FmBn *a, const FmBn *m) {
  FmBn t;
  memset(&t, 0, sizeof t);
  for (int bit = bn_bits(a) - 1; bit >= 0; bit--) {
    /* t < m, so 2t + 1 < 2m fits whenever m fits with one spare bit */
    bn_shl1(&t, (a->d[bit / 32] >> (bit % 32)) & 1);
    if (bn_cmp(&t, m) >= 0) bn_sub_in(&t, m);
  }
  *r = t;
}

/* Montgomery product t = a*b*R^-1 mod m, k limbs, a and b < m. */
static void mont_mul(u32 *out, const u32 *a, const u32 *b, const u32 *m, int k, u32 mp) {
  u32 t[BN_LIMBS + 2];
  memset(t, 0, sizeof(u32) * (size_t)(k + 2));
  for (int i = 0; i < k; i++) {
    u64 c = 0;
    for (int j = 0; j < k; j++) {
      u64 s = (u64)a[j] * b[i] + t[j] + c;
      t[j] = (u32)s;
      c = s >> 32;
    }
    u64 s = (u64)t[k] + c;
    t[k] = (u32)s;
    t[k + 1] = (u32)(s >> 32);
    u32 q = t[0] * mp;
    c = ((u64)q * m[0] + t[0]) >> 32;
    for (int j = 1; j < k; j++) {
      s = (u64)q * m[j] + t[j] + c;
      t[j - 1] = (u32)s;
      c = s >> 32;
    }
    s = (u64)t[k] + c;
    t[k - 1] = (u32)s;
    t[k] = t[k + 1] + (u32)(s >> 32);
  }
  /* t < 2m: one conditional subtraction */
  bool ge = t[k] != 0;
  if (!ge) {
    ge = true;
    for (int i = k - 1; i >= 0; i--)
      if (t[i] != m[i]) { ge = t[i] > m[i]; break; }
  }
  if (ge) {
    u64 borrow = 0;
    for (int i = 0; i < k; i++) {
      u64 s = (u64)t[i] - m[i] - borrow;
      t[i] = (u32)s;
      borrow = (s >> 63) & 1;
    }
  }
  memcpy(out, t, sizeof(u32) * (size_t)k);
}

bool bn_modexp(FmBn *r, const FmBn *b, const FmBn *e, const FmBn *m) {
  if (!m->n || !(m->d[0] & 1) || m->n > BN_LIMBS - 1) return false;
  int k = m->n;
  /* mp = -m^-1 mod 2^32 (Newton: each step doubles the correct bits) */
  u32 inv = 1;
  for (int i = 0; i < 5; i++) inv *= 2 - m->d[0] * inv;
  u32 mp = (u32)0 - inv;
  /* R^2 mod m, R = 2^(32k) */
  FmBn rr;
  memset(&rr, 0, sizeof rr);
  rr.n = 1;
  rr.d[0] = 1;
  for (int i = 0; i < 64 * k; i++) {
    bn_shl1(&rr, 0);
    if (bn_cmp(&rr, m) >= 0) bn_sub_in(&rr, m);
  }
  FmBn base;
  bn_mod(&base, b, m);
  u32 x[BN_LIMBS], acc[BN_LIMBS], one[BN_LIMBS], r2[BN_LIMBS], bb[BN_LIMBS];
  memset(r2, 0, sizeof r2);
  memcpy(r2, rr.d, sizeof(u32) * (size_t)rr.n);
  memset(bb, 0, sizeof bb);
  memcpy(bb, base.d, sizeof(u32) * (size_t)base.n);
  memset(one, 0, sizeof one);
  one[0] = 1;
  mont_mul(x, bb, r2, m->d, k, mp);        /* base in Montgomery form */
  mont_mul(acc, one, r2, m->d, k, mp);     /* 1 in Montgomery form */
  for (int bit = bn_bits(e) - 1; bit >= 0; bit--) {
    mont_mul(acc, acc, acc, m->d, k, mp);
    if ((e->d[bit / 32] >> (bit % 32)) & 1) mont_mul(acc, acc, x, m->d, k, mp);
  }
  mont_mul(acc, acc, one, m->d, k, mp);
  memset(r, 0, sizeof *r);
  memcpy(r->d, acc, sizeof(u32) * (size_t)k);
  r->n = k;
  bn_trim(r);
  return true;
}
