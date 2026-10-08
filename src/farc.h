/* farc.h -- archives: detect, list, extract, create.
**
** Formats: zip (store/deflate, ZipCrypto and WinZip AES-256), 7z (LZMA,
** LZMA2, PPMd, BCJ, AES-256), rar 4/5 (extract only, no encryption), tar
** (ustar/pax/gnu) plain or compressed with gz/bz2/xz/zst, and single files
** compressed with gz/bz2/xz/zst/lzma.
**
** All functions may run on a worker thread. Long operations report through
** FmArcCb: progress (return false to cancel) and password (asks the user;
** called from the worker, the callback must block until answered).
**
** Design decisions:
**   - Entries are listed once at open into an arena; extraction streams
**     data through 64 KB buffers and never holds a whole file in memory.
**   - Names inside archives use '/'; anything absolute or containing ".."
**     is refused on extraction (zip slip).
*/
#ifndef FARC_H
#define FARC_H

#include "fcore.h"

typedef enum FmArcFmt {
  ARC_NONE = 0,
  ARC_ZIP, ARC_7Z, ARC_RAR,
  ARC_TAR, ARC_TAR_GZ, ARC_TAR_BZ2, ARC_TAR_XZ, ARC_TAR_ZST,
  ARC_GZ, ARC_BZ2, ARC_XZ, ARC_ZST, ARC_LZMA,
  ARC_COUNT
} FmArcFmt;

typedef struct FmArcEntry {
  char *path;          /* "dir/sub/name", '/'-separated, no leading '/' */
  u64 size;            /* uncompressed */
  u64 packed;          /* compressed (0 when unknown, e.g. solid 7z) */
  i64 mtime;           /* unix seconds, 0 unknown */
  u32 crc;             /* CRC32 when the format stores one */
  u32 mode;            /* POSIX mode bits when known */
  u8 is_dir;
  u8 encrypted;
  u8 is_link;          /* symlink: data is the target path */
  u8 method;           /* format-specific, for the info dialog */
} FmArcEntry;

typedef struct FmArcCb {
  void *ud;
  /* bytes of output done / total; return false to cancel */
  bool (*progress)(void *ud, u64 done, u64 total);
  /* fills buf with a password; retry=true after a wrong one; false = cancel */
  bool (*password)(void *ud, char *buf, int cap, bool retry);
  /* current entry name (for the progress line), may be NULL */
  void (*entry)(void *ud, const char *name);
} FmArcCb;

typedef struct FmArc FmArc;

FmArcFmt    arc_detect(const char *path);         /* magic bytes, then extension */
const char *arc_fmt_name(FmArcFmt f);             /* "ZIP", "TAR.GZ" ... */
const char *arc_fmt_ext(FmArcFmt f);              /* ".zip", ".tar.gz" ... */
bool        arc_fmt_can_create(FmArcFmt f);
bool        arc_fmt_can_encrypt(FmArcFmt f);      /* zip, 7z */
bool        arc_fmt_multi(FmArcFmt f);            /* holds many files (not .gz alone) */

/* Opens and lists. cb may be NULL (no password prompt: encrypted 7z headers
** then fail with FM_ERR_PASSWORD). */
FmErr arc_open(const char *path, const FmArcCb *cb, FmArc **out);
void  arc_close(FmArc *a);
FmArcFmt arc_format(const FmArc *a);
int   arc_count(const FmArc *a);
const FmArcEntry *arc_entry(const FmArc *a, int i);
bool  arc_has_encrypted(const FmArc *a);
u64   arc_total_size(const FmArc *a);

/* Extracts selected entries (sel[i] != 0; NULL = all) into dst_dir.
** strip_prefix ("" or "dir/sub/") is removed from each name, so extracting
** a folder seen inside the archive lands without its parents. Folders in
** the selection extract with everything below them. */
FmErr arc_extract(FmArc *a, const u8 *sel, const char *dst_dir, const char *strip_prefix,
                  const FmArcCb *cb);
/* Extracts one entry to an exact file path (viewers, open-with). */
FmErr arc_extract_one(FmArc *a, int index, const char *dst_file, const FmArcCb *cb);

typedef struct FmArcOpts {
  FmArcFmt fmt;
  int level;              /* 0 = store, 1 fastest .. 9 best; -1 default */
  const char *password;   /* NULL/"" = none (zip, 7z only) */
  bool zip_aes;           /* zip: AES-256 (true) or ZipCrypto (false) */
  bool encrypt_names;     /* 7z: encrypt the file list too */
  bool solid;             /* 7z */
} FmArcOpts;

/* Creates out_path from files/folders (absolute paths). Names are stored
** relative to base_dir. Folders are added recursively. */
FmErr arc_create(const char *out_path, const char *const *srcs, int n, const char *base_dir,
                 const FmArcOpts *o, const FmArcCb *cb);

#endif
