/* fcrypt.h -- the cryptography the archive formats need, and CRC32.
**
** AES-128/192/256 (encrypt + decrypt, ECB block, CBC, WinZip CTR, big-endian
** CTR), SHA-1, SHA-256, SHA-512, HMAC-SHA1/256/512, PBKDF2-HMAC-SHA1/512,
** ZipCrypto and CRC32. Hardware
** paths (AES-NI, ARMv8 crypto, PCLMUL/ARM CRC) are chosen at run time
** through fasm.h when FM_ASM is on; the C versions are always present.
*/
#ifndef FCRYPT_H
#define FCRYPT_H

#include "fcore.h"

/* ---- CRC32 (zip/gzip polynomial) ---------------------------------------- */

u32 crc32_update(u32 crc, const void *data, size_t n);   /* start with 0 */

/* ---- AES ---------------------------------------------------------------- */

typedef struct FmAes {
  u32 ek[60];          /* encryption round keys */
  u32 dk[60];          /* decryption round keys */
  int rounds;          /* 10, 12, 14 */
  int hw;              /* hardware path in use */
} FmAes;

void aes_init(FmAes *a, const u8 *key, int key_bytes);   /* 16, 24, 32 */
void aes_encrypt_block(const FmAes *a, const u8 in[16], u8 out[16]);
void aes_decrypt_block(const FmAes *a, const u8 in[16], u8 out[16]);
/* In place, n multiple of 16; iv is updated (chaining continues). */
void aes_cbc_encrypt(const FmAes *a, u8 iv[16], u8 *buf, size_t n);
void aes_cbc_decrypt(const FmAes *a, u8 iv[16], u8 *buf, size_t n);
/* WinZip AE-x: CTR mode with a little-endian counter starting at 1; any n.
** ctr and pos keep the state between calls (init both to zero). */
typedef struct FmAesCtr { u8 ctr[16]; u8 ks[16]; int pos; } FmAesCtr;
void aes_ctr_le(const FmAes *a, FmAesCtr *c, u8 *buf, size_t n);
/* CTR with a 128-bit big-endian counter (NIST SP 800-38A, MEGA): the block
** holding byte `pos` uses counter iv + pos/16, so any slice can be processed
** on its own (resumed downloads, chunked uploads); any n. */
void aes_ctr_be(const FmAes *a, const u8 iv[16], u64 pos, u8 *buf, size_t n);
void wipe(void *p, size_t n);         /* zeroing the compiler cannot drop */

/* ---- hashes ------------------------------------------------------------- */

typedef struct FmSha1 { u32 h[5]; u64 len; u8 buf[64]; int n; } FmSha1;
void sha1_init(FmSha1 *s);
void sha1_update(FmSha1 *s, const void *data, size_t n);
void sha1_final(FmSha1 *s, u8 out[20]);

typedef struct FmSha256 { u32 h[8]; u64 len; u8 buf[64]; int n; } FmSha256;
void sha256_init(FmSha256 *s);
void sha256_update(FmSha256 *s, const void *data, size_t n);
void sha256_final(FmSha256 *s, u8 out[32]);

typedef struct FmSha512 { u64 h[8]; u64 len; u8 buf[128]; int n; } FmSha512;
void sha512_init(FmSha512 *s);
void sha512_update(FmSha512 *s, const void *data, size_t n);
void sha512_final(FmSha512 *s, u8 out[64]);

typedef struct FmHmacSha1 { FmSha1 inner, outer; } FmHmacSha1;
void hmac_sha1_init(FmHmacSha1 *h, const u8 *key, size_t key_len);
void hmac_sha1_update(FmHmacSha1 *h, const void *data, size_t n);
void hmac_sha1_final(FmHmacSha1 *h, u8 out[20]);

typedef struct FmHmacSha256 { FmSha256 inner, outer; } FmHmacSha256;
void hmac_sha256_init(FmHmacSha256 *h, const u8 *key, size_t key_len);
void hmac_sha256_update(FmHmacSha256 *h, const void *data, size_t n);
void hmac_sha256_final(FmHmacSha256 *h, u8 out[32]);
/* one call: out = HMAC-SHA256(key, data) */
void hmac_sha256(const u8 *key, size_t key_len, const void *data, size_t n, u8 out[32]);

typedef struct FmHmacSha512 { FmSha512 inner, outer; } FmHmacSha512;
void hmac_sha512_init(FmHmacSha512 *h, const u8 *key, size_t key_len);
void hmac_sha512_update(FmHmacSha512 *h, const void *data, size_t n);
void hmac_sha512_final(FmHmacSha512 *h, u8 out[64]);
void hmac_sha512(const u8 *key, size_t key_len, const void *data, size_t n, u8 out[64]);

void pbkdf2_sha1(const u8 *pw, size_t pw_len, const u8 *salt, size_t salt_len, u32 iters,
                 u8 *out, size_t out_len);
/* PBKDF2-HMAC-SHA512 (MEGA account v2: 100 000 rounds) */
void pbkdf2_sha512(const u8 *pw, size_t pw_len, const u8 *salt, size_t salt_len, u32 iters,
                   u8 *out, size_t out_len);

/* ---- ZipCrypto (traditional PKWARE) ------------------------------------- */

typedef struct FmZipCrypto { u32 k0, k1, k2; } FmZipCrypto;
void zipcrypto_init(FmZipCrypto *z, const char *password);
void zipcrypto_decrypt(FmZipCrypto *z, u8 *buf, size_t n);
void zipcrypto_encrypt(FmZipCrypto *z, u8 *buf, size_t n);

#endif
