/* fwiki_int.h -- Wikipedia: the API calls (fwiki_api.c) shared with the
** view (fwiki.c) and the self test. Every wk_* call blocks (network) and
** runs on a worker thread; results are plain structs the caller frees with
** the matching wk_*_free.
*/
#ifndef FWIKI_INT_H
#define FWIKI_INT_H

#include "fwiki.h"

#define WK_LANG 16

/* A page in a list: search results, most read, related, links. */
typedef struct WkPage {
  char id[64];                 /* the source's own id (a book, a species), "" = the title is the id */
  char title[256];
  char desc[256];              /* the short description ("Province in Central Visayas") */
  char thumb[512];             /* thumbnail URL, "" = none */
} WkPage;

typedef struct WkList {
  WkPage *it;
  int n, cap;
  int next;                    /* search: the offset of the next page, -1 = end */
  char error[256];
} WkList;

/* Today's featured content (REST /feed/featured): not every language has it. */
typedef struct WkOtd { int year; char text[400]; WkPage page; } WkOtd;
typedef struct WkFeed {
  bool has_tfa;
  WkPage tfa;
  char tfa_extract[1200];
  WkList most;
  bool has_potd;
  char potd_title[256], potd_thumb[512], potd_full[1024], potd_desc[400];
  WkOtd otd[10];
  int notd;
  char error[256];
} WkFeed;

/* An article: blocks of text, headings among them. */
enum { WB_PARA, WB_H2, WB_H3, WB_H4 };
typedef struct WkBlock { int kind; char *s; } WkBlock;
typedef struct WkArticle {
  char lang[WK_LANG];
  char id[64];                 /* the source's id ("" for wikis: the title) */
  char wiki[256];              /* a Wikipedia article about the same thing ("" = none) */
  char title[256], desc[256], url[512], lead[1024];
  int lead_w, lead_h;
  char *text;                  /* every block's string lives in here */
  WkBlock *b;
  int nb;
  char error[256];
} WkArticle;

/* The article's media (REST /page/media-list). */
enum { WM_IMAGE, WM_VIDEO, WM_AUDIO };
typedef struct WkMedia {
  int type;
  char url[1024];              /* the picture itself when the source gives it ("" = ask the wiki) */
  char file[256];              /* "File:..." */
  char thumb[512];
  char caption[300];
} WkMedia;
typedef struct WkMediaList { WkMedia *it; int n; char error[256]; } WkMediaList;

/* A media file resolved for the viewers: a playable stream or a picture. */
typedef struct WkFile {
  char url[1024];              /* video: a WebM (VP9/Opus) derivative; audio: MP3 when there is one */
  char mime[80];
  int w, h;
  char error[256];
} WkFile;

typedef struct WkLang { char lang[WK_LANG]; char autonym[64]; char title[256]; } WkLang;
typedef struct WkLangs { WkLang *it; int n; char error[256]; } WkLangs;

/* The user's contact for Wikimedia (email or web page), "" = none: sent in
** an Api-User-Agent header with every request (thumbnails and downloads
** too, through wk_headers). Main thread; rarely changed. */
void wk_set_contact(const char *contact);
const char *wk_headers(void);

/* Calls. The first argument is the Wikimedia site ("en.wikipedia.org",
** "tl.wiktionary.org", "species.wikimedia.org"; wk_host_ok). */
bool  wk_host_ok(const char *host);
FmErr wk_feed(const char *lang, WkFeed *out, volatile int *cancel);
FmErr wk_search(const char *lang, const char *query, int offset, WkList *out, volatile int *cancel);
FmErr wk_article(const char *lang, const char *title, WkArticle *out, volatile int *cancel);
FmErr wk_media(const char *lang, const char *title, WkMediaList *out, volatile int *cancel);
FmErr wk_file(const char *lang, const char *file, int type, WkFile *out, volatile int *cancel);
FmErr wk_langs(const char *lang, const char *title, WkLangs *out, volatile int *cancel);
FmErr wk_related(const char *lang, const char *title, WkList *out, volatile int *cancel);
FmErr wk_links(const char *lang, const char *title, WkList *out, volatile int *cancel);
FmErr wk_random(const char *lang, char *title, size_t cap, volatile int *cancel);

void wk_list_free(WkList *l);
void wk_feed_free(WkFeed *f);
void wk_article_free(WkArticle *a);
void wk_media_free(WkMediaList *m);
void wk_langs_free(WkLangs *l);

/* Parsers, exposed for the self test. */
FmErr wk_parse_article(const char *json, size_t len, const char *lang, WkArticle *out);
FmErr wk_parse_search(const char *json, size_t len, WkList *out);
FmErr wk_parse_feed(const char *json, size_t len, WkFeed *out);
FmErr wk_parse_media(const char *json, size_t len, WkMediaList *out);
FmErr wk_parse_file(const char *json, size_t len, int type, WkFile *out);
bool  wk_lang_ok(const char *lang);

/* ---- the other sources (fwiki_more.c) ---------------------------------------------- */

/* Project Gutenberg through Gutendex: popular (query "") or matching books,
** page 1..; a book as text in chapters (its plain-text file). */
FmErr rd_guten_search(const char *lang, const char *query, int page, WkList *out, volatile int *cancel);
FmErr rd_guten_book(const char *id, WkArticle *out, volatile int *cancel);
void  rd_guten_text(char *text, WkArticle *out);     /* the parser, for the self test */
/* iNaturalist: species by name, or a group's most observed (group = a taxon
** id); a species as an article (summary, classification, facts) and its
** photos. locale: common names in that language. */
FmErr rd_inat_search(const char *locale, const char *query, WkList *out, volatile int *cancel);
FmErr rd_inat_group(const char *locale, const char *taxon, int page, WkList *out, volatile int *cancel);
FmErr rd_inat_taxon(const char *locale, const char *id, WkArticle *out, WkMediaList *photos, volatile int *cancel);
FmErr rd_inat_parse_taxon(const char *json, size_t len, WkArticle *out, WkMediaList *photos);
/* Fruityvice: every fruit (query filters by name); one fruit's nutrition. */
FmErr rd_fruit_list(const char *query, WkList *out, volatile int *cancel);
FmErr rd_fruit(const char *name, WkArticle *out, volatile int *cancel);
/* Free Dictionary (dictionaryapi.dev): an English word's meanings, examples,
** similar and opposite words; its recorded pronunciations as sounds. */
FmErr rd_dict(const char *word, WkArticle *out, WkMediaList *sounds, volatile int *cancel);
FmErr rd_dict_parse(const char *json, size_t len, WkArticle *out, WkMediaList *sounds);
/* Shared by the parsers: a block list over text built by the caller. */
void  wk_blocks_from_lines(WkArticle *a);

#endif
