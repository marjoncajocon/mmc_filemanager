/* fpsrc.h -- online photo sources: one adapter struct per website.
**
** The photo twin of fvsrc.h (video sources): each site is a table of
** functions; the gallery (masonry grid), the viewer hookup (the built-in
** image viewer on a cached file) and downloads are shared.
**
**   const FmPsrc g_psrc_mysite = {
**     "mysite", "My Site", IC_IMAGE, PSRC_SEARCH, mysite_search, NULL, "Free, no key needed"
**   };
**
** Threading and memory rules are the same as fvsrc.h: every function blocks,
** runs on a worker thread, is reentrant, polls `cancel`, and fills plain
** structs with fixed-size strings (one free per page).
**
** Sources checked from this PC (2026-10-08): Openverse, Wikimedia Commons,
** NASA Images and the Art Institute of Chicago answer without a key;
** Pexels, Unsplash and Pixabay need a free key (401 without). Google Photos
** since 2025 only exposes photos the user picks in Google's picker after an
** OAuth sign-in (Picker API), so it is a "picked photos" source, not a full
** library browser.
*/
#ifndef FPSRC_H
#define FPSRC_H

#include "fcore.h"
#include "ficon.h"

enum {
  PSRC_SEARCH  = 1 << 0,   /* has search() */
  PSRC_NEEDKEY = 1 << 1,   /* needs an API key in the settings */
  PSRC_BROWSE  = 1 << 2,   /* empty query lists something useful (curated / recent / picked) */
  PSRC_SIGNIN  = 1 << 3,   /* needs an account sign-in (OAuth) */
};

typedef struct FmPsrcItem {
  char id[96];
  char title[256];
  char author[128];
  char license[64];        /* "CC BY-SA 4.0", "Pexels License", "Public domain" ... */
  char thumb[512];         /* small image for the grid (~400 px) */
  char full[1024];         /* large image for the viewer / download */
  char page[512];          /* the photo's web page (attribution, "Open in browser") */
  int width, height;       /* of `full`; 0 = unknown (the grid then assumes 4:3) */
  u32 color;               /* dominant colour 0xRRGGBB for placeholders, 0 = none */
  /* added by the adapters (2026-10-08): */
  char source[16];         /* FmPsrc key that made the item ("openverse"...) */
  char original[1024];     /* best file for the Download button (full size,
                           ** maybe TIFF / 50 MB); "" = same as `full` */
} FmPsrcItem;

typedef struct FmPsrcPage {
  FmPsrcItem *items;
  int count, cap;
  char next[256];          /* continuation token, "" = end */
  char error[256];
} FmPsrcPage;

typedef struct FmPsrcConf {
  char key_pexels[96], key_unsplash[96], key_pixabay[96];
  char cache_dir[FM_PATH_MAX];
  char download_dir[FM_PATH_MAX];
  bool safe_search;
} FmPsrcConf;

typedef struct FmPsrc {
  const char *key;          /* "openverse", "wikimedia", ... (stable, saved in conf) */
  const char *name;
  FmIcon icon;
  int flags;                /* PSRC_* */
  FmErr (*search)(const FmPsrcConf *c, const char *query, const char *page_token, FmPsrcPage *out,
                  volatile int *cancel);
  /* optional: fill `full`/size/licence when search returned a thin item */
  FmErr (*details)(const FmPsrcConf *c, FmPsrcItem *item, volatile int *cancel);
  const char *about;        /* "Creative Commons images, no key needed" */
  /* added by the adapters (2026-10-08): */
  /* extra request headers ("Key: value\r\n...") for this site's images,
  ** thumbnails included, or NULL. The Art Institute's IIIF server answers
  ** 403 (a Cloudflare page) without "AIC-User-Agent". Use psrc_item_headers. */
  const char *img_headers;
  /* optional: called by psrc_fetch after a photo was saved to a download
  ** folder (Unsplash asks apps to report downloads); errors are ignored */
  void (*saved)(const FmPsrcConf *c, const FmPsrcItem *item, volatile int *cancel);
} FmPsrc;

/* ---- registry and shared helpers (fpsrc.c) ------------------------------------ */

int psrc_count(void);
const FmPsrc *psrc_at(int i);
const FmPsrc *psrc_find(const char *key);

void psrc_page_free(FmPsrcPage *p);
FmPsrcItem *psrc_page_add(FmPsrcPage *p);
void psrc_conf_snapshot(FmPsrcConf *c);

/* Headers to send with item->thumb / item->full requests (net_get's
** `headers`), NULL when the item's source needs none. */
const char *psrc_item_headers(const FmPsrcItem *item);

/* Downloads item->full into the cache (or dir when given) for the viewer;
** out gets the local path. Reuses a complete cached file.
** With dir: saves item->original (else full) as "<title> - <author>.<ext>"
** plus "<title> - <author>.txt" holding title, author, licence and page,
** copying from the cache when the same file is already there. The
** extension comes from the file's first bytes; a reply that is not an
** image is an error, never a saved file. */
FmErr psrc_fetch(const FmPsrcConf *c, const FmPsrcItem *item, const char *dir, char *out, size_t cap,
                 bool (*progress)(void *user, u64 done, u64 total), void *user, char *err, size_t errcap,
                 volatile int *cancel);

#endif
