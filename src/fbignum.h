/* fbignum.h -- just enough big-integer arithmetic for RSA decryption.
**
** MEGA hands out the session id encrypted to the account's RSA key; this is
** the modular exponentiation that opens it. Fixed-size numbers (up to 4224
** bits) on the stack, no allocation.
**
** Design decisions:
**   - 32-bit limbs with 64-bit products: the same code on tcc's 32-bit
**     build, zig and the NDK, no compiler intrinsics.
**   - Montgomery multiplication (CIOS) for the exponentiation; the slow
**     bit-by-bit reduction is only used for setup (R^2 mod n, base mod n).
**   - Not constant time: it runs on the user's own machine on their own
**     key, once per sign-in, so a timing side channel has no observer.
*/
#ifndef FBIGNUM_H
#define FBIGNUM_H

#include "fcore.h"

#define BN_LIMBS 132                  /* 4224 bits */

typedef struct FmBn {
  int n;                              /* limbs in use (no leading zero limbs) */
  u32 d[BN_LIMBS];                    /* little-endian limbs */
} FmBn;

/* Big-endian bytes -> number; false when it does not fit. */
bool   bn_from_bytes(FmBn *r, const u8 *p, size_t len);
/* Number -> minimal big-endian bytes (0 -> no bytes); returns the length,
** or 0 when cap is too small for a non-zero value. */
size_t bn_to_bytes(const FmBn *a, u8 *out, size_t cap);
int    bn_cmp(const FmBn *a, const FmBn *b);
int    bn_bits(const FmBn *a);
/* r = a * b; false on overflow. r must not alias a or b. */
bool   bn_mul(FmBn *r, const FmBn *a, const FmBn *b);
/* r = a mod m (m > 0). */
void   bn_mod(FmBn *r, const FmBn *a, const FmBn *m);
/* r = b^e mod m; m must be odd. false when m is even or zero. */
bool   bn_modexp(FmBn *r, const FmBn *b, const FmBn *e, const FmBn *m);

#endif
