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
int test_net(const char *tmp);        /* ftest_net.c: json, processes, https (MMCFM_NET_TEST=1) */
int test_viz(const char *tmp);        /* ftest_viz.c: visualizer bands, smoothing, presets, conf */
int test_library(const char *tmp);    /* ftest_lib.c: media library scan, favorites, persistence */
int test_vsrc(const char *tmp);       /* ftest_vsrc.c: online video sources (MMCFM_NET_TEST=1, MMCFM_YTDLP=path) */
int test_hls(const char *tmp);        /* ftest_hls.c: HLS playlists, AES-128, Dailymotion (MMCFM_HLS_TEST, MMCFM_DM_TEST) */
int test_generic(const char *tmp);    /* ftest_generic.c: native "Any site" page resolver (MMCFM_GENERIC_TEST=url) */
int test_online_ui(const char *tmp);  /* ftest_online.c: online videos view: formatting, recent searches */
int test_psrc(const char *tmp);       /* ftest_psrc.c: online photo sources (MMCFM_NET_TEST=1, MMCFM_*_KEY) */
int test_photo_ui(const char *tmp);   /* ftest_photo.c: online photos view: rows, albums file, recent searches */
int test_asrc(const char *tmp);       /* ftest_asrc.c: online audio sources, XML reader (MMCFM_NET_TEST=1, MMCFM_*_KEY) */
int test_aonline_ui(const char *tmp); /* ftest_aonline.c: online audio view: item lines, library file (MMCFM_AONLINE_PLAY) */
int test_soft(const char *tmp);       /* ftest_soft.c: built-in VP9 + Opus WebM backend (MMCFM_SOFT_BENCH) */
int test_cloud(const char *tmp);      /* ftest_cloud.c: cloud accounts, locations, jobs on the in-memory service */
int test_dav(const char *tmp);         /* ftest_dav.c: WebDAV + S3 adapters, SigV4 (MMCFM_DAV_TEST, MMCFM_S3_TEST) */
int test_mega(const char *tmp);        /* ftest_mega.c: MEGA crypto, sign-in, tree, CTR+MAC (MMCFM_MEGA_LINK, MMCFM_MEGA_LOGIN) */
int test_oauth(const char *tmp);      /* ftest_oauth.c: OAuth PKCE + loopback sign-in, Drive/Dropbox/OneDrive parsers */

#endif
