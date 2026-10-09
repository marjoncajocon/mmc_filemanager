/* fcloud_mega_int.h -- MEGA internals shared with the self test (ftest_mega.c).
**
** Only fcloud_mega.c and ftest_mega.c include this. The pieces are split
** out of the network code so they can be checked offline: key derivation,
** the login reply, attributes, the node tree, and the CTR + MAC transfer
** state.
*/
#ifndef FCLOUD_MEGA_INT_H
#define FCLOUD_MEGA_INT_H

#include "fcloud.h"
#include "fcrypt.h"
#include "fjson.h"

/* ---- encoding --------------------------------------------------------------- */

/* base64url without padding (MEGA's form); returns the text length. */
size_t mega_b64_enc(const u8 *p, size_t n, char *out, size_t cap);
/* Accepts base64url or standard base64, padded or not; returns the byte
** count, -1 on a bad character or when it does not fit. */
int    mega_b64_dec(const char *s, u8 *out, size_t cap);

/* ---- key derivation and sign-in --------------------------------------------- */

void mega_prepare_key(const char *password, u8 out[16]);                /* account v1 */
void mega_stringhash(const char *email, const u8 key[16], char out[16]);  /* v1 "uh" */
/* v2: PBKDF2-HMAC-SHA512(password, salt, 100000) -> password key + "uh". */
void mega_v2_derive(const char *password, const u8 *salt, size_t salt_len, u8 pass_key[16], char uh[32]);
/* The "us" reply -> session id and master key (RSA csid or plain tsid). */
FmErr mega_login_reply(const FmJsonNode *r, const u8 pass_key[16], char *sid, size_t sidcap, u8 mk[16],
                       char *err, size_t errcap);

/* ---- attributes ------------------------------------------------------------- */

/* "MEGA{...}" under AES-CBC with a zero IV. name and fp ("c": fingerprint,
** may be NULL) come out; false when the key does not fit. */
bool mega_attr_decrypt(const u8 key[16], const char *b64, char *name, size_t ncap, char *fp, size_t fpcap);
bool mega_attr_encrypt(const u8 key[16], const char *name, const char *fp, char *out, size_t cap);
/* mtime from a fingerprint string, 0 when there is none */
i64  mega_fp_mtime(const char *fp);
/* The fingerprint of a local file (CRC samples + mtime), as MEGA's clients make it. */
bool mega_fingerprint(const char *path, u64 size, i64 mtime, char *out, size_t cap);

/* ---- the node tree ---------------------------------------------------------- */

enum { MG_FILE = 0, MG_FOLDER = 1, MG_ROOT = 2, MG_INBOX = 3, MG_RUBBISH = 4 };

typedef struct MgNode {
  char h[12], p[12];       /* handle, parent handle */
  u8 type;                 /* MG_* */
  u8 klen;                 /* 32 file, 16 folder, 0 none */
  u8 key[32];              /* the node key (files: key^iv words, iv, mac) */
  u64 size;
  i64 ts, mtime;           /* created on MEGA; file time from the fingerprint (0 none) */
  const char *name, *fp;   /* in the tree's arena */
} MgNode;

typedef struct MgTree {
  MgNode *n;
  int count, cap;
  int *idx;                /* open-addressing index of handles (-1 empty) */
  int idx_cap;             /* power of two, at least twice count */
  FmArena strings;
  char root[12];           /* the Cloud Drive, or the folder link's top folder */
  char rubbish[12];
} MgTree;

/* Builds the tree from an "f" reply object. Account trees decrypt with the
** master key (and share keys from "ok"/"sk"); link trees with the link key.
** Nodes whose key cannot be opened are left out. */
FmErr  mega_tree_build(MgTree *t, const FmJsonNode *f, const u8 key[16], bool link);
void   mega_tree_free(MgTree *t);
MgNode *mega_tree_find(MgTree *t, const char *h);
/* Appends a node with handle h (zeroed otherwise). Moves the array: earlier
** MgNode pointers are invalid afterwards. */
MgNode *mega_tree_add(MgTree *t, const char *h);
/* folded AES key of a node (files: words 0-3 xor 4-7) */
void   mega_node_aes_key(const MgNode *n, u8 out[16]);

/* ---- transfers: CTR + chunk MACs -------------------------------------------- */

/* End (exclusive) of the MEGA chunk that starts at `start`: 128 KB, 256 KB,
** ... 1 MB, then 1 MB steps. */
u64  mega_chunk_end(u64 start);

typedef struct MgMac {
  const FmAes *aes;
  u8 nonce[8];
  u8 cur[16], blk[16], file[16];
  int bn;                  /* bytes in blk */
  bool open;
  u64 pos, chunk_end;
} MgMac;

void mega_mac_init(MgMac *m, const FmAes *aes, const u8 nonce[8]);
void mega_mac_feed(MgMac *m, const u8 *p, size_t n);      /* plaintext, in file order */
void mega_mac_final(MgMac *m, u8 meta[8]);                /* condensed MAC */

/* HTTP 402 proof of work: "1:<easiness>:<time>:<token>" -> the request line
** "X-Hashcash: 1:<token>:<prefix>" request line (with CRLF) into out. */
bool mega_hashcash(const char *hdr, char *out, size_t cap, volatile int *cancel);

/* ---- links ------------------------------------------------------------------ */

typedef struct MgLink {
  bool folder;
  char id[16];             /* public handle */
  u8 key[32];
  int klen;
  char sub[16];            /* folder link: a sub folder/file handle, "" = top */
} MgLink;

bool mega_parse_link(const char *link, MgLink *out);

#endif
