/* fasm.h -- inline-assembly fast paths for fcrypt.c (AES, CRC32).
**
** Only compiled under FM_ASM (gcc/clang on x86-64 or arm64, see fcore.h);
** everything here has a C twin in fcrypt.c, and the asm is chosen at run
** time from what the CPU reports.
**
** Design decisions:
**   - One header, all static inline, so no extra object file and nothing to
**     link on targets without asm (tcc, emcc, --no-asm).
**   - x86-64: cpuid for AES-NI; the AES rounds use aesenc/aesdec with "x"
**     (xmm) operands. CRC32 stays slicing-by-8 in C: SSE4.2's crc32 is the
**     Castagnoli polynomial, not the zip one, and the C loop already runs
**     at several GB/s.
**   - AArch64: the crc32b/crc32x instructions (the zip polynomial) and
**     aese/aesd when HWCAP says so (getauxval on Linux/Android,
**     IsProcessorFeaturePresent on Windows, always on Apple). The
**     `.arch_extension` directive lets the assembler accept them without
**     building the whole file for armv8-a+crypto.
**   - Round keys for the hardware paths are kept as plain bytes in
**     FmAes.ek/dk (the C path keeps big-endian words); FmAes.hw says which.
*/
#ifndef FASM_H
#define FASM_H

#include "fcore.h"
#include "fcrypt.h"

/* The C key schedule, whatever the CPU: lets the self test compare the
** hardware AES against the portable one (fcrypt.c). */
void aes_init_soft(FmAes *a, const u8 *key, int key_bytes);

enum { FASM_AES = 1, FASM_CRC = 2 };

#ifdef FM_ASM

#if defined(FM_ARM64) && defined(FM_WIN)
#  include "fwin.h"
#  ifndef PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE
#    define PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE 30
#  endif
#  ifndef PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE
#    define PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE 31
#  endif
#elif defined(FM_ARM64) && (defined(FM_LINUX) || defined(FM_ANDROID))
#  include <sys/auxv.h>
#endif

/* ---- cpu features ------------------------------------------------------- */

static inline int fasm_detect(void) {
  int f = 0;
#if defined(FM_X64)
  u32 a, b, c, d;
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0u), "c"(0u));
  if (a >= 1) {
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0u));
    if (c & (1u << 25)) f |= FASM_AES;
  }
#elif defined(FM_ARM64)
#  if defined(__APPLE__)
  f = FASM_AES | FASM_CRC;
#  elif defined(FM_WIN)
  if (IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE)) f |= FASM_AES;
  if (IsProcessorFeaturePresent(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE)) f |= FASM_CRC;
#  elif defined(FM_LINUX) || defined(FM_ANDROID)
  unsigned long hw = getauxval(AT_HWCAP);
  if (hw & (1ul << 3)) f |= FASM_AES;    /* HWCAP_AES */
  if (hw & (1ul << 7)) f |= FASM_CRC;    /* HWCAP_CRC32 */
#  endif
#endif
  return f;
}

/* Detected once; a race only makes two threads store the same value. */
static inline int fasm_cpu(void) {
  static volatile int cached = -1;
  int f = cached;
  if (f < 0) cached = f = fasm_detect();
  return f;
}

/* ---- AES ---------------------------------------------------------------- */

#if defined(FM_X64)

typedef long long FasmV __attribute__((vector_size(16)));

static inline FasmV fasm_ld(const u8 *p) { FasmV v; memcpy(&v, p, 16); return v; }
static inline void fasm_st(u8 *p, FasmV v) { memcpy(p, &v, 16); }

static inline void fasm_aes_enc(const u8 *rk, int rounds, const u8 *in, u8 *out) {
  FasmV b = fasm_ld(in) ^ fasm_ld(rk);
  for (int i = 1; i < rounds; i++) {
    FasmV k = fasm_ld(rk + 16 * i);
    __asm__("aesenc %1, %0" : "+x"(b) : "x"(k));
  }
  FasmV k = fasm_ld(rk + 16 * rounds);
  __asm__("aesenclast %1, %0" : "+x"(b) : "x"(k));
  fasm_st(out, b);
}

static inline void fasm_aes_dec(const u8 *rk, int rounds, const u8 *in, u8 *out) {
  FasmV b = fasm_ld(in) ^ fasm_ld(rk);
  for (int i = 1; i < rounds; i++) {
    FasmV k = fasm_ld(rk + 16 * i);
    __asm__("aesdec %1, %0" : "+x"(b) : "x"(k));
  }
  FasmV k = fasm_ld(rk + 16 * rounds);
  __asm__("aesdeclast %1, %0" : "+x"(b) : "x"(k));
  fasm_st(out, b);
}


#elif defined(FM_ARM64)

typedef unsigned char FasmV __attribute__((vector_size(16)));

static inline FasmV fasm_ld(const u8 *p) { FasmV v; memcpy(&v, p, 16); return v; }
static inline void fasm_st(u8 *p, FasmV v) { memcpy(p, &v, 16); }

static inline void fasm_aes_enc(const u8 *rk, int rounds, const u8 *in, u8 *out) {
  FasmV b = fasm_ld(in);
  for (int i = 0; i < rounds - 1; i++) {
    FasmV k = fasm_ld(rk + 16 * i);
    __asm__(".arch_extension aes\n\taese %0.16b, %1.16b\n\taesmc %0.16b, %0.16b"
            : "+w"(b) : "w"(k));
  }
  FasmV k = fasm_ld(rk + 16 * (rounds - 1));
  __asm__(".arch_extension aes\n\taese %0.16b, %1.16b" : "+w"(b) : "w"(k));
  fasm_st(out, b ^ fasm_ld(rk + 16 * rounds));
}

static inline void fasm_aes_dec(const u8 *rk, int rounds, const u8 *in, u8 *out) {
  FasmV b = fasm_ld(in);
  for (int i = 0; i < rounds - 1; i++) {
    FasmV k = fasm_ld(rk + 16 * i);
    __asm__(".arch_extension aes\n\taesd %0.16b, %1.16b\n\taesimc %0.16b, %0.16b"
            : "+w"(b) : "w"(k));
  }
  FasmV k = fasm_ld(rk + 16 * (rounds - 1));
  __asm__(".arch_extension aes\n\taesd %0.16b, %1.16b" : "+w"(b) : "w"(k));
  fasm_st(out, b ^ fasm_ld(rk + 16 * rounds));
}


/* ---- CRC32 (AArch64) ---------------------------------------------------- */

#define FASM_HAVE_CRC 1

static inline u32 fasm_crc32(u32 crc, const u8 *p, size_t n) {
  crc = ~crc;
  while (n && ((uintptr_t)p & 7)) {
    u32 b = *p++;
    __asm__(".arch_extension crc\n\tcrc32b %w0, %w0, %w1" : "+r"(crc) : "r"(b));
    n--;
  }
  while (n >= 8) {
    u64 v;
    memcpy(&v, p, 8);
    __asm__(".arch_extension crc\n\tcrc32x %w0, %w0, %x1" : "+r"(crc) : "r"(v));
    p += 8;
    n -= 8;
  }
  while (n--) {
    u32 b = *p++;
    __asm__(".arch_extension crc\n\tcrc32b %w0, %w0, %w1" : "+r"(crc) : "r"(b));
  }
  return ~crc;
}

#endif /* FM_ARM64 */

#endif /* FM_ASM */

#endif
