/* ftest.h -- the built-in self test (`mmcfm --selftest`).
**
** Each area adds one function; ftest.c runs them all in a temporary folder
** and prints PASS/FAIL lines. Exit code 0 means everything passed.
*/
#ifndef FTEST_H
#define FTEST_H

#include "fcore.h"

/* Shared helpers for test files. */
extern int g_test_fail;
#define TEST_CHECK(cond) do { if (!(cond)) { \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_test_fail++; } } while (0)

int test_run_all(void);

/* Each returns the number of failures; `tmp` is an empty scratch folder. */
int test_core(const char *tmp);       /* ftest.c: strings, paths, utf-8 */
int test_crypto(const char *tmp);     /* ftest_arc.c: AES, SHA, PBKDF2, CRC vectors */
int test_archives(const char *tmp);   /* ftest_arc.c: zip, tar.*, single files */
int test_archives2(const char *tmp);  /* ftest_arc2.c: 7z and rar */
int test_media(const char *tmp);      /* ftest_media.c: decoders on generated data */
int test_fs(const char *tmp);         /* ftest_fs.c: copy/move/delete engine, vfs */
int test_themes(const char *tmp);     /* ftest_theme.c: every theme readable */

#endif
