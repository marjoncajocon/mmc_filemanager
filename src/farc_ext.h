/* farc_ext.h -- archive-internal helpers beyond the contract headers.
**
** fstream with caller-supplied I/O: fstream.h reads and writes a FILE*,
** but zip entries need a step in between (ZipCrypto / WinZip AES sit
** between the file and the codec), so the same codecs are offered over
** read/write callbacks; in_open and out_open are thin wrappers over them.
** Plus the name decoding shared by zip and tar.
**
** Design decisions:
**   - A callback, not a FILE*-like object: no fopencookie/funopen, which
**     Windows lacks.
**   - CODEC_ZIP_LZMA is zip method 14 (a 4-byte zip header, 5 property
**     bytes, then raw LZMA). It ends quietly at the end of input because
**     zip often stores no end marker; the zip reader checks the size.
*/
#ifndef FARC_EXT_H
#define FARC_EXT_H

#include "fstream.h"

/* Returns bytes read (> 0), 0 at the end, -1 on error. */
typedef long (*FmReadFn)(void *ud, void *buf, size_t n);
/* Writes all n bytes or returns an error. */
typedef FmErr (*FmWriteFn)(void *ud, const void *buf, size_t n);

#define CODEC_ZIP_LZMA ((FmCodec)100)

FmIn  *in_open_cb(FmCodec c, FmReadFn rd, void *ud, FmErr *err);
FmOut *out_open_cb(FmCodec c, int level, FmWriteFn wr, void *ud, FmErr *err);

/* .lzma ("LZMA alone") of `size` bytes pulled from `in`, header included.
** progress(ud, bytes_read) returning false cancels (FM_ERR_CANCEL). */
FmErr lzma_alone_encode(FILE *in, u64 size, int level, FmWriteFn wr, void *wud,
                        bool (*progress)(void *ud, u64 done), void *pud);

/* CRC tables of the LZMA SDK (xz checks, 7z); safe to call from any thread. */
void stream_sdk_init(void);

/* ---- names (farc.c) ----------------------------------------------------- */

/* Strict UTF-8 check: no overlongs, surrogates or code points past U+10FFFF. */
bool arc_valid_utf8(const u8 *s, size_t n);
/* Raw entry name to UTF-8 in out (room for 3 * n + 1 bytes). Names are kept
** as they are when is_utf8 is set or they are valid UTF-8 anyway; other
** bytes are read as CP437, the zip default and what Windows tools use. */
void arc_name_utf8(const u8 *raw, size_t n, bool is_utf8, char *out);

#endif
