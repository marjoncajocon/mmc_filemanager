/* fstream.h -- streaming (de)compressors with one interface.
**
** Used by tar (tar.gz, tar.xz ...), the single-file formats and zip.
** A reader pulls decompressed bytes from a FILE*; a writer pushes raw
** bytes and writes compressed ones to a FILE*. Buffers are fixed (64 KB
** in, 64 KB out), whatever the file size.
*/
#ifndef FSTREAM_H
#define FSTREAM_H

#include "fcore.h"

typedef enum FmCodec {
  CODEC_STORE = 0,   /* pass-through */
  CODEC_DEFLATE,     /* raw deflate (zip) */
  CODEC_GZIP,        /* gzip framing, multi-member */
  CODEC_BZIP2,
  CODEC_XZ,
  CODEC_LZMA,        /* .lzma ("LZMA alone") */
  CODEC_ZSTD,
} FmCodec;

typedef struct FmIn FmIn;
typedef struct FmOut FmOut;

/* limit: compressed bytes available from the current position (-1 = to EOF). */
FmIn *in_open(FILE *f, FmCodec c, i64 limit, FmErr *err);
/* Returns bytes read (0 at the end), -1 on error (see in_error). */
long  in_read(FmIn *s, void *buf, size_t n);
FmErr in_error(const FmIn *s);
u64   in_consumed(const FmIn *s);   /* compressed bytes consumed so far */
void  in_close(FmIn *s);

FmOut *out_open(FILE *f, FmCodec c, int level, FmErr *err);   /* level 1..9, -1 default */
FmErr  out_write(FmOut *s, const void *buf, size_t n);
FmErr  out_close(FmOut *s);     /* flushes the trailer; returns the final status */
u64    out_produced(const FmOut *s);

#endif
