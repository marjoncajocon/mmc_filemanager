/* fwiki.c -- Online reading: the view (see fwiki.h). The sources are the
** Wikimedia wikis (fwiki_api.c) and Gutenberg, iNaturalist, Fruityvice and
** the Free Dictionary (fwiki_more.c).
**
** Design decisions:
**   - One full-window view, drawn like the other online views, with pages:
**     home (the source's own start: Wikipedia's featured content, popular
**     books, a group of species, every fruit...), results, an article, its
**     media, its languages, related articles and links, contents, saved
**     articles and history. Back walks the pages, then the articles read.
**   - A source is a row of kSrc: what it can do (languages, media, related,
**     random) decides which buttons show. Everything a source returns fills
**     the same structs, so one reader draws a Wikipedia article, a book in
**     chapters, a species and a word's meanings.
**   - Every request runs on its own thread (WkTask), polled each frame; one
**     that is no longer wanted is cancelled and joined later (reap), as the
**     photo and video views do.
**   - Articles are drawn from the page's HTML when the source has it (the
**     wikis, Gutenberg's HTML editions; fdoc.c: headings, lists, code,
**     pictures, tables, bold/italic, links), else as wrapped plain text.
**     Either way the layout is measured once per width and text size and only
**     what is on screen is drawn (a whole book is a few thousand blocks).
**   - A link inside a page opens that page in the same source (a wiki
**     article, a #section of the book); a picture opens in the image viewer
**     at full size; anything else goes to the browser.
**   - Pictures open in the app's image viewer, videos in the video player,
**     sounds in the music player; all are downloaded whole into the cache
**     first (upload.wikimedia.org answers 429 to streaming's many range
**     requests, checked 2026-10-10).
**   - Source, language, text size, the Wikimedia contact, saved articles and
**     history live in PLACE_CONFIG/wiki.txt; no settings keys.
*/
#include "fwiki_int.h"
#include "fapp.h"
#include "fconf.h"
#include "ffont.h"
#include "fgfx.h"
#include "ficon.h"
#include "fnet.h"
#include "fplat.h"
#include "ftheme.h"
#include "ftitle.h"
#include "fview.h"
#include "fview_int.h"
#include "fonline_int.h"

/* ---- sources -------------------------------------------------------------------- */

enum { SK_WIKI, SK_GUTEN, SK_INAT, SK_FRUIT, SK_DICT, SK_DEVTO, SK_SO };

typedef struct Src {
  const char *key, *name, *about;
  FmIcon icon;
  int kind;
  const char *host;            /* wikis: "%s.wikipedia.org" (%s = language) or a fixed host */
  bool langs, media, related, random, feed;
} Src;

static const Src kSrc[] = {
  { "wikipedia", "Wikipedia", "The free encyclopedia", IC_TEXT, SK_WIKI, "%s.wikipedia.org", 1, 1, 1, 1, 1 },
  { "dictionary", "Dictionary", "English words: meanings, examples, pronunciation", IC_TEXT, SK_DICT, NULL, 0, 1, 0, 0, 0 },
  { "wiktionary", "Wiktionary", "Words in every language: meanings, translations", IC_TEXT, SK_WIKI, "%s.wiktionary.org", 1, 1, 0, 1, 0 },
  { "gutenberg", "Free books", "Project Gutenberg: 70,000+ public-domain books", IC_LIBRARY, SK_GUTEN, NULL, 0, 0, 0, 0, 0 },
  { "inaturalist", "Plants & animals", "iNaturalist: every species, photos, local names", IC_PICTURES, SK_INAT, NULL, 0, 1, 0, 0, 0 },
  { "fruits", "Fruits", "Fruityvice: nutrition of 49 fruits", IC_HEART, SK_FRUIT, NULL, 0, 0, 0, 0, 0 },
  { "wikivoyage", "Wikivoyage", "Travel guides", IC_LANDMARK, SK_WIKI, "%s.wikivoyage.org", 1, 1, 1, 1, 0 },
  { "wikibooks", "Wikibooks", "Free textbooks, cookbooks and how-tos", IC_LIBRARY, SK_WIKI, "%s.wikibooks.org", 1, 0, 1, 1, 0 },
  { "wikisource", "Wikisource", "Free texts: books, poems, speeches, laws", IC_DOC, SK_WIKI, "%s.wikisource.org", 1, 1, 1, 1, 0 },
  { "wikispecies", "Wikispecies", "The classification of living things", IC_TEXT, SK_WIKI, "species.wikimedia.org", 0, 0, 1, 1, 0 },
  { "devto", "DEV articles", "DEV Community: programming tutorials and articles", IC_RSS, SK_DEVTO, NULL, 0, 0, 0, 0, 0 },
  { "stackoverflow", "Stack Overflow", "Programming questions and their answers", IC_CODE, SK_SO, NULL, 0, 0, 0, 0, 0 },
};
#define NSRC ((int)FM_COUNT(kSrc))

static int src_find(const char *key) {
  for (int i = 0; i < NSRC; i++)
    if (!strcmp(kSrc[i].key, key)) return i;
  return -1;
}

static void host_of(int src, const char *lang, char *out, size_t cap) {
  const Src *s = &kSrc[src];
  if (s->kind != SK_WIKI) { out[0] = 0; return; }
  if (strstr(s->host, "%s")) fm_snprintf(out, cap, s->host, lang);
  else fm_strlcpy(out, s->host, cap);
}

/* The home's chips: iNaturalist's groups (iconic taxa), DEV's and Stack
** Overflow's topics (tags). */
typedef struct Group { const char *name, *arg; } Group;
static const Group kInatGroups[] = {
  { "Plants", "47126" }, { "Birds", "3" }, { "Mammals", "40151" }, { "Insects", "47158" }, { "Fish", "47178" },
  { "Reptiles", "26036" }, { "Amphibians", "20978" }, { "Fungi", "47170" }, { "Mollusks", "47115" },
  { "Spiders", "47118" },
};
static const Group kDevTags[] = {
  { "This week", "" }, { "Tutorials", "tutorial" }, { "Beginners", "beginners" }, { "C", "c" }, { "C++", "cpp" },
  { "Python", "python" }, { "JavaScript", "javascript" }, { "Java", "java" }, { "Rust", "rust" }, { "Go", "go" },
  { "Web", "webdev" }, { "Algorithms", "algorithms" }, { "Data structures", "datastructures" },
};
static const Group kSoTags[] = {
  { "Top", "" }, { "C", "c" }, { "C++", "c++" }, { "Python", "python" }, { "JavaScript", "javascript" },
  { "Java", "java" }, { "C#", "c#" }, { "SQL", "sql" }, { "Git", "git" }, { "Algorithms", "algorithm" },
  { "Data structures", "data-structures" }, { "Linked lists", "linked-list" },
};

static const Group *groups_of(int kind, int *n) {
  switch (kind) {
    case SK_INAT: *n = (int)FM_COUNT(kInatGroups); return kInatGroups;
    case SK_DEVTO: *n = (int)FM_COUNT(kDevTags); return kDevTags;
    case SK_SO: *n = (int)FM_COUNT(kSoTags); return kSoTags;
    default: *n = 0; return NULL;
  }
}

/* Sources whose home is a list of their own (books, species, fruits, articles, questions). */
static bool has_browse(int kind) {
  return kind == SK_GUTEN || kind == SK_INAT || kind == SK_FRUIT || kind == SK_DEVTO || kind == SK_SO;
}

/* ---- requests ------------------------------------------------------------------- */

enum { WT_FEED, WT_SEARCH, WT_ARTICLE, WT_MEDIA, WT_PLAY, WT_IMAGE, WT_LANGS, WT_RELATED, WT_LINKS, WT_RANDOM, WT_BROWSE };

typedef struct WkTask {
  int kind;
  SDL_Thread *thr;
  SDL_atomic_t done;
  volatile int cancel;
  int src;
  char lang[WK_LANG];
  char host[64];
  char arg[512];              /* query, title, file or URL */
  char id[64];                /* the source's id */
  int num;                    /* search page or offset, media type */
  FmErr err;
  char error[256];
  WkFeed feed;
  WkList list;
  WkArticle art;
  WkMediaList media;
  WkFile file;
  WkLangs langs;
  char title[256];            /* random */
  char path[FM_PATH_MAX];     /* image, video, sound: the downloaded file */
  SDL_atomic_t permille;      /* download progress, 0..1000 */
} WkTask;

static WkTask *g_dropped[16];

static bool cache_path(const char *url, char *out, size_t cap) {
  char dir[FM_PATH_MAX], name[64];
  if (!plat_place(PLACE_CACHE, dir, sizeof dir)) return false;
  char sub[FM_PATH_MAX];
  if (!fm_path_join(sub, sizeof sub, dir, "wiki-media")) return false;
  plat_mkdirs(sub);
  u64 h = 1469598103934665603ull;                    /* FNV-1a of the URL */
  for (const char *s = url; *s; s++) h = (h ^ (u8)*s) * 1099511628211ull;
  char clean[1024];
  fm_strlcpy(clean, url, sizeof clean);
  clean[strcspn(clean, "?#")] = 0;
  const char *dot = strrchr(clean, '.');
  char ext[8] = "jpg";
  if (dot && strlen(dot) <= 5 && !strchr(dot, '/')) fm_strlcpy(ext, dot + 1, sizeof ext);
  fm_snprintf(name, sizeof name, "%016llx.%s", (unsigned long long)h, ext);
  return fm_path_join(out, cap, sub, name);
}

static bool dl_progress(void *u, u64 done, u64 total) {
  WkTask *t = (WkTask *)u;
  int pm = total ? (int)(done * 1000 / total) : 0;
  if (pm != SDL_AtomicGet(&t->permille)) {
    SDL_AtomicSet(&t->permille, pm);
    app_wake();
  }
  return !t->cancel;
}

static void search_task(WkTask *t) {
  switch (kSrc[t->src].kind) {
    case SK_WIKI: t->err = wk_search(t->host, t->arg, t->num, &t->list, &t->cancel); break;
    case SK_GUTEN: t->err = rd_guten_search(t->lang, t->arg, t->num, &t->list, &t->cancel); break;
    case SK_INAT: t->err = rd_inat_search(t->lang, t->arg, &t->list, &t->cancel); break;
    case SK_FRUIT: t->err = rd_fruit_list(t->arg, &t->list, &t->cancel); break;
    case SK_DEVTO: t->err = rd_devto_list(t->arg, t->num, &t->list, &t->cancel); break;   /* a topic tag */
    case SK_SO: t->err = rd_so_search(t->arg, t->num, &t->list, &t->cancel); break;
    default: t->err = FM_ERR_UNSUPPORTED; break;
  }
  fm_strlcpy(t->error, t->list.error, sizeof t->error);
}

static void article_task(WkTask *t) {
  switch (kSrc[t->src].kind) {
    case SK_WIKI: t->err = wk_article(t->host, t->arg, &t->art, &t->cancel); break;
    case SK_GUTEN: t->err = rd_guten_book(t->id, &t->art, &t->cancel); break;
    case SK_INAT: t->err = rd_inat_taxon(t->lang, t->id, &t->art, &t->media, &t->cancel); break;
    case SK_FRUIT: t->err = rd_fruit(t->id[0] ? t->id : t->arg, &t->art, &t->cancel); break;
    case SK_DICT: t->err = rd_dict(t->arg, &t->art, &t->media, &t->cancel); break;
    case SK_DEVTO: t->err = rd_devto_article(t->id[0] ? t->id : t->arg, &t->art, &t->cancel); break;
    case SK_SO: t->err = rd_so_question(t->id[0] ? t->id : t->arg, &t->art, &t->cancel); break;
    default: t->err = FM_ERR_UNSUPPORTED; break;
  }
  fm_strlcpy(t->error, t->art.error, sizeof t->error);
  fm_strlcpy(t->art.lang, t->lang, sizeof t->art.lang);
}

static int task_main(void *u) {
  WkTask *t = (WkTask *)u;
  switch (t->kind) {
    case WT_FEED: t->err = wk_feed(t->host, &t->feed, &t->cancel); fm_strlcpy(t->error, t->feed.error, sizeof t->error); break;
    case WT_SEARCH: search_task(t); break;
    case WT_BROWSE:
      if (kSrc[t->src].kind == SK_GUTEN) t->err = rd_guten_search(t->lang, "", t->num, &t->list, &t->cancel);
      else if (kSrc[t->src].kind == SK_INAT) t->err = rd_inat_group(t->lang, t->arg, t->num, &t->list, &t->cancel);
      else if (kSrc[t->src].kind == SK_FRUIT) t->err = rd_fruit_list("", &t->list, &t->cancel);
      else if (kSrc[t->src].kind == SK_DEVTO) t->err = rd_devto_list(t->arg, t->num, &t->list, &t->cancel);
      else if (kSrc[t->src].kind == SK_SO) t->err = rd_so_list(t->arg, t->num, &t->list, &t->cancel);
      fm_strlcpy(t->error, t->list.error, sizeof t->error);
      break;
    case WT_ARTICLE: article_task(t); break;
    case WT_MEDIA: t->err = wk_media(t->host, t->arg, &t->media, &t->cancel); fm_strlcpy(t->error, t->media.error, sizeof t->error); break;
    case WT_LANGS: t->err = wk_langs(t->host, t->arg, &t->langs, &t->cancel); fm_strlcpy(t->error, t->langs.error, sizeof t->error); break;
    case WT_RELATED: t->err = wk_related(t->host, t->arg, &t->list, &t->cancel); fm_strlcpy(t->error, t->list.error, sizeof t->error); break;
    case WT_LINKS: t->err = wk_links(t->host, t->arg, &t->list, &t->cancel); fm_strlcpy(t->error, t->list.error, sizeof t->error); break;
    case WT_RANDOM: t->err = wk_random(t->host, t->title, sizeof t->title, &t->cancel); break;
    case WT_PLAY:
    case WT_IMAGE: {
      /* the file: a URL the source gave (id), else the wiki finds it */
      const char *u;
      if (t->id[0] == 'h') {
        fm_strlcpy(t->file.url, t->id, sizeof t->file.url);
        t->err = FM_OK;
      } else {
        t->err = wk_file(t->host, t->arg, t->num, &t->file, &t->cancel);
        fm_strlcpy(t->error, t->file.error, sizeof t->error);
      }
      u = t->file.url;
      if (t->err == FM_OK) {                         /* picture, video, sound: into the cache */
        if (!cache_path(u, t->path, sizeof t->path)) { t->err = FM_ERR_IO; break; }
        if (!plat_exists(t->path)) {
          FmNetResp r;
          memset(&r, 0, sizeof r);
          bool wm = strstr(u, "wikimedia.org") != NULL;
          t->err = net_download(u, wm ? wk_headers() : NULL, t->path, dl_progress, t, &r, &t->cancel);
          if (t->err == FM_OK && r.status != 200) {
            t->err = FM_ERR_IO;
            fm_snprintf(t->error, sizeof t->error, "The server answered %d", r.status);
            plat_remove_file(t->path);
          }
          net_resp_free(&r);
        }
      }
      break;
    }
    default: break;
  }
  if (t->err != FM_OK && !t->error[0]) fm_strlcpy(t->error, fm_err_str(t->err), sizeof t->error);
  SDL_AtomicSet(&t->done, 1);
  app_wake();
  return 0;
}

static WkTask *task_start(int kind, int src, const char *lang, const char *arg, const char *id, int num) {
  WkTask *t = (WkTask *)fm_calloc(1, sizeof *t);
  t->kind = kind;
  t->src = FM_CLAMP(src, 0, NSRC - 1);
  fm_strlcpy(t->lang, lang ? lang : "en", sizeof t->lang);
  host_of(t->src, t->lang, t->host, sizeof t->host);
  fm_strlcpy(t->arg, arg ? arg : "", sizeof t->arg);
  fm_strlcpy(t->id, id ? id : "", sizeof t->id);
  t->num = num;
  t->thr = fm_thread_create(task_main, "reading", t);
  if (!t->thr) { fm_free(t); return NULL; }
  return t;
}

static bool task_done(WkTask *t) { return t && SDL_AtomicGet(&t->done); }

static void task_free(WkTask *t) {
  if (!t) return;
  if (t->thr) SDL_WaitThread(t->thr, NULL);
  wk_feed_free(&t->feed);
  wk_list_free(&t->list);
  wk_article_free(&t->art);
  wk_media_free(&t->media);
  wk_langs_free(&t->langs);
  fm_free(t);
}

/* Not wanted any more: cancelled, joined once it returns. */
static void task_drop(WkTask **pt) {
  WkTask *t = *pt;
  *pt = NULL;
  if (!t) return;
  t->cancel = 1;
  for (int i = 0; i < FM_COUNT(g_dropped); i++)
    if (!g_dropped[i]) { g_dropped[i] = t; return; }
  task_free(t);                                       /* the list is full: wait now */
}

static void reap(void) {
  for (int i = 0; i < FM_COUNT(g_dropped); i++)
    if (g_dropped[i] && task_done(g_dropped[i])) { task_free(g_dropped[i]); g_dropped[i] = NULL; }
}

/* ---- state ----------------------------------------------------------------------- */

enum { WP_HOME, WP_RESULTS, WP_ARTICLE, WP_MEDIA, WP_LANGS, WP_RELATED, WP_TOC, WP_SAVED, WP_HISTORY, WP_PICKLANG,
       WP_CONTACT, WP_NPAGES };

typedef struct WkRef {
  char src[16], lang[WK_LANG], id[64];
  char title[256], desc[256], thumb[512];
} WkRef;
#define WK_SAVED_MAX 500
#define WK_HIST_MAX  80
#define WK_STACK     32

static struct {
  bool open, loaded;
  int page;
  int src;                     /* the source on screen (kSrc) */
  char lang[WK_LANG];
  float zoom;                  /* text size, 0.8 .. 1.6 */
  char field[256];
  bool focus_search;
  FmScroll scroll[WP_NPAGES];
  /* home: Wikipedia's feed, or the source's list (books, a group, fruits) */
  WkTask *t_feed;
  WkFeed feed;
  bool feed_ok;
  char feed_lang[WK_LANG];
  char feed_error[256];
  WkTask *t_browse;
  WkList browse;
  char browse_key[80];         /* what browse holds: "src|lang|group" */
  char browse_error[256];
  int group[NSRC];             /* the chip on each source's home (groups_of) */
  /* results */
  WkTask *t_search;
  WkList res;
  int res_src;
  char res_query[256];
  char res_error[256];
  /* the article */
  WkTask *t_art;
  WkArticle art;
  int art_src;
  bool art_ok;
  char art_error[256];
  char want_title[256];
  float *bh;                   /* block heights at lay_w / lay_zoom */
  float lay_w, lay_zoom, lay_total, lay_top;
  int lay_gen;                 /* font_generation() of bh */
  float restore_y;             /* back to an article: where it was read */
  int demo_page;               /* --demo-wiki-page: shown once the article is in, -1 = none */
  /* where the article leads */
  WkTask *t_media, *t_langs, *t_rel, *t_links;
  WkMediaList media;
  WkLangs langs;
  WkList rel, links;
  char media_error[256], langs_error[256], rel_error[256];
  bool media_for, langs_for, rel_for;     /* loaded for the current article */
  /* one-shot requests */
  WkTask *t_play, *t_image, *t_random;
  /* the articles read before (Back) */
  struct { int src; char lang[WK_LANG], id[64], title[256]; float y; } stack[WK_STACK];
  int nstack;
  int back_page;               /* a sub-page returns here */
  /* find in page */
  bool find_open, find_focus;
  char find_q[128], find_ran[128];
  int find_n, find_cur;
  int *find_blk;               /* plain text pages: the block of each match */
  float art_view_h;
  char contact[300];           /* for Wikimedia's rate limits (Api-User-Agent) */
  char contact_field[300];
  /* lists */
  WkRef *saved, *hist;
  int nsaved, nhist;
} S;

static void save_file(void);
static void contact_card(float x, float *y, float col, bool page);
static void go_back(void);

static const Src *cur(void) { return &kSrc[FM_CLAMP(S.src, 0, NSRC - 1)]; }

/* ---- wiki.txt ------------------------------------------------------------------------- */

static bool conf_path(char *out, size_t cap) {
  char dir[FM_PATH_MAX];
  return plat_place(PLACE_CONFIG, dir, sizeof dir) && fm_path_join(out, cap, dir, "wiki.txt");
}

static void clean_field(char *s) {
  for (; *s; s++)
    if (*s == '\t' || *s == '\n' || *s == '\r' || *s == '|') *s = ' ';
}

static void ref_set(WkRef *r, const char *src, const char *lang, const char *id, const char *title, const char *desc,
                    const char *thumb) {
  memset(r, 0, sizeof *r);
  fm_strlcpy(r->src, src, sizeof r->src);
  fm_strlcpy(r->lang, lang, sizeof r->lang);
  fm_strlcpy(r->id, id ? id : "", sizeof r->id);
  fm_strlcpy(r->title, title, sizeof r->title);
  fm_strlcpy(r->desc, desc ? desc : "", sizeof r->desc);
  fm_strlcpy(r->thumb, thumb ? thumb : "", sizeof r->thumb);
  clean_field(r->id);
  clean_field(r->title);
  clean_field(r->desc);
  clean_field(r->thumb);
}

static void load_file(void) {
  if (S.loaded) return;
  S.loaded = true;
  fm_strlcpy(S.lang, "en", sizeof S.lang);
  S.zoom = 1.0f;
  S.demo_page = -1;
  S.saved = (WkRef *)fm_calloc(WK_SAVED_MAX, sizeof *S.saved);
  S.hist = (WkRef *)fm_calloc(WK_HIST_MAX, sizeof *S.hist);
  char path[FM_PATH_MAX];
  FILE *f = conf_path(path, sizeof path) ? fm_fopen(path, "rb") : NULL;
  if (f) {
    char line[2000];
    while (fgets(line, sizeof line, f)) {
      line[strcspn(line, "\r\n")] = 0;
      char *fld[8] = { 0 };
      int n = 0;
      for (char *p = line; p && n < 8;) {
        fld[n++] = p;
        char *tab = strchr(p, '\t');
        if (tab) *tab = 0;
        p = tab ? tab + 1 : NULL;
      }
      if (n >= 2 && !strcmp(fld[0], "L") && wk_lang_ok(fld[1])) fm_strlcpy(S.lang, fld[1], sizeof S.lang);
      else if (n >= 2 && !strcmp(fld[0], "Z")) S.zoom = FM_CLAMP((float)atof(fld[1]), 0.8f, 1.6f);
      else if (n >= 2 && !strcmp(fld[0], "C")) fm_strlcpy(S.contact, fld[1], sizeof S.contact);
      else if (n >= 2 && !strcmp(fld[0], "R") && src_find(fld[1]) >= 0) S.src = src_find(fld[1]);
      else if (n >= 5 && (!strcmp(fld[0], "s") || !strcmp(fld[0], "h")) && src_find(fld[1]) >= 0 && fld[4][0]) {
        /* s|h  source  lang  id  title  desc  thumb */
        bool sv = fld[0][0] == 's';
        WkRef *v = sv ? S.saved : S.hist;
        int *cnt = sv ? &S.nsaved : &S.nhist;
        if (*cnt < (sv ? WK_SAVED_MAX : WK_HIST_MAX))
          ref_set(&v[(*cnt)++], fld[1], fld[2], fld[3], fld[4], n > 5 ? fld[5] : "", n > 6 ? fld[6] : "");
      } else if (n >= 3 && (!strcmp(fld[0], "S") || !strcmp(fld[0], "H")) && wk_lang_ok(fld[1]) && fld[2][0]) {
        /* the first version: Wikipedia only */
        bool sv = fld[0][0] == 'S';
        WkRef *v = sv ? S.saved : S.hist;
        int *cnt = sv ? &S.nsaved : &S.nhist;
        if (*cnt < (sv ? WK_SAVED_MAX : WK_HIST_MAX))
          ref_set(&v[(*cnt)++], "wikipedia", fld[1], "", fld[2], n > 3 ? fld[3] : "", n > 4 ? fld[4] : "");
      }
    }
    fclose(f);
  }
  wk_set_contact(S.contact);
  fm_strlcpy(S.contact_field, S.contact, sizeof S.contact_field);
}

static void save_file(void) {
  char path[FM_PATH_MAX], tmp[FM_PATH_MAX];
  if (!conf_path(path, sizeof path)) return;
  fm_snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *f = fm_fopen(tmp, "wb");
  if (!f) return;
  fprintf(f, "# mmcfm Online reading: L language, Z text size, C Wikimedia contact, R source, s saved, h history\n");
  fprintf(f, "L\t%s\nZ\t%.2f\nR\t%s\n", S.lang, S.zoom, cur()->key);
  if (S.contact[0]) fprintf(f, "C\t%s\n", S.contact);
  for (int i = 0; i < S.nsaved; i++)
    fprintf(f, "s\t%s\t%s\t%s\t%s\t%s\t%s\n", S.saved[i].src, S.saved[i].lang, S.saved[i].id, S.saved[i].title,
            S.saved[i].desc, S.saved[i].thumb);
  for (int i = 0; i < S.nhist; i++)
    fprintf(f, "h\t%s\t%s\t%s\t%s\t%s\t%s\n", S.hist[i].src, S.hist[i].lang, S.hist[i].id, S.hist[i].title, S.hist[i].desc,
            S.hist[i].thumb);
  if (fclose(f) == 0) {
    plat_remove_file(path);
    plat_rename(tmp, path);
  }
}

static int ref_find(const WkRef *v, int n, const char *src, const char *lang, const char *id, const char *title) {
  for (int i = 0; i < n; i++) {
    if (strcmp(v[i].src, src) || strcmp(v[i].lang, lang)) continue;
    if (id && id[0] ? !strcmp(v[i].id, id) : !strcmp(v[i].title, title)) return i;
  }
  return -1;
}

static void ref_front(WkRef *v, int *n, int max, const char *src, const char *lang, const char *id, const char *title,
                      const char *desc, const char *thumb) {
  int i = ref_find(v, *n, src, lang, id, title);
  if (i < 0) i = *n < max ? (*n)++ : max - 1;
  memmove(&v[1], &v[0], (size_t)i * sizeof *v);
  ref_set(&v[0], src, lang, id, title, desc, thumb);
}

static void ref_remove(WkRef *v, int *n, int i) {
  if (i < 0 || i >= *n) return;
  memmove(&v[i], &v[i + 1], (size_t)(*n - i - 1) * sizeof *v);
  (*n)--;
}

/* A saved article as one string for the home screen: "source|lang|id|title". */
static void ref_key(const WkRef *r, char *out, size_t cap) {
  fm_snprintf(out, cap, "%s|%s|%s|%s", r->src, r->lang, r->id, r->title);
}

static bool key_split(const char *key, char *src, char *lang, char *id, char *title) {
  const char *p[4];
  p[0] = key;
  for (int i = 1; i < 4; i++) {
    const char *b = strchr(p[i - 1], '|');
    if (!b) return false;
    p[i] = b + 1;
  }
  size_t l0 = (size_t)(p[1] - p[0] - 1), l1 = (size_t)(p[2] - p[1] - 1), l2 = (size_t)(p[3] - p[2] - 1);
  if (l0 >= 16 || l1 >= WK_LANG || l2 >= 64) return false;
  memcpy(src, p[0], l0); src[l0] = 0;
  memcpy(lang, p[1], l1); lang[l1] = 0;
  memcpy(id, p[2], l2); id[l2] = 0;
  fm_strlcpy(title, p[3], 256);
  return src_find(src) >= 0;
}

int wiki_saved_count(void) {
  load_file();
  return S.nsaved;
}

bool wiki_saved_get(int i, char *key, size_t kcap, char *title, size_t tcap, char *desc, size_t dcap, char *thumb,
                    size_t thcap) {
  load_file();
  if (i < 0 || i >= S.nsaved) return false;
  ref_key(&S.saved[i], key, kcap);
  fm_strlcpy(title, S.saved[i].title, tcap);
  int s = src_find(S.saved[i].src);
  if (S.saved[i].desc[0]) fm_snprintf(desc, dcap, "%s \xC2\xB7 %s", s >= 0 ? kSrc[s].name : "", S.saved[i].desc);
  else fm_strlcpy(desc, s >= 0 ? kSrc[s].name : "", dcap);
  fm_strlcpy(thumb, S.saved[i].thumb, thcap);
  return true;
}

bool wiki_saved_has(const char *key) {
  load_file();
  char src[16], lang[WK_LANG], id[64], title[256];
  if (!key_split(key, src, lang, id, title)) return false;
  return ref_find(S.saved, S.nsaved, src, lang, id, title) >= 0;
}

/* ---- navigation ----------------------------------------------------------------------- */

static const struct { const char *code, *name; } kLangs[] = {
  { "en", "English" }, { "tl", "Tagalog" }, { "ceb", "Cebuano" }, { "simple", "Simple English" },
  { "ilo", "Ilokano" }, { "war", "Waray" }, { "pam", "Kapampangan" }, { "bcl", "Bikol" },
  { "es", "Espa\xC3\xB1ol" }, { "fr", "Fran\xC3\xA7" "ais" }, { "de", "Deutsch" }, { "it", "Italiano" },
  { "pt", "Portugu\xC3\xAAs" }, { "nl", "Nederlands" }, { "id", "Bahasa Indonesia" }, { "ms", "Bahasa Melayu" },
  { "vi", "Ti\xE1\xBA\xBFng Vi\xE1\xBB\x87t" }, { "tr", "T\xC3\xBCrk\xC3\xA7" "e" }, { "pl", "Polski" },
  { "sv", "Svenska" }, { "ro", "Rom\xC3\xA2n\xC4\x83" }, { "cs", "\xC4\x8C" "e\xC5\xA1tina" },
};

static const char *lang_name(const char *code) {
  for (int i = 0; i < FM_COUNT(kLangs); i++)
    if (!strcmp(kLangs[i].code, code)) return kLangs[i].name;
  return code;
}

static bool is_sub(int page) {
  return page == WP_MEDIA || page == WP_LANGS || page == WP_RELATED || page == WP_TOC || page == WP_SAVED ||
         page == WP_HISTORY || page == WP_PICKLANG || page == WP_CONTACT;
}

static void go(int page) {
  if (page != S.page && is_sub(page))
    S.back_page = S.page == WP_TOC || S.page == WP_PICKLANG || S.page == WP_CONTACT ? S.back_page : S.page;
  S.page = page;
  ui_focus(0);
  ui_redraw();
}

static void feed_start(void) {
  if (S.t_feed || (S.feed_ok && !strcmp(S.feed_lang, S.lang))) return;
  wk_feed_free(&S.feed);
  S.feed_ok = false;
  S.feed_error[0] = 0;
  fm_strlcpy(S.feed_lang, S.lang, sizeof S.feed_lang);
  S.t_feed = task_start(WT_FEED, 0, S.lang, NULL, NULL, 0);
}

/* The source's home list (books, a group's species, fruits); more = the next page. */
static void browse_start(bool more) {
  const Src *s = cur();
  if (!has_browse(s->kind)) return;
  char key[80];
  int ng;
  const Group *gs = groups_of(s->kind, &ng);
  const char *grp = gs ? gs[FM_CLAMP(S.group[S.src], 0, ng - 1)].arg : "";
  fm_snprintf(key, sizeof key, "%s|%s|%s", s->key, S.lang, grp);
  if (!more) {
    if (!strcmp(key, S.browse_key) && (S.browse.n || S.t_browse || S.browse_error[0])) return;
    task_drop(&S.t_browse);
    wk_list_free(&S.browse);
    S.browse.next = -1;
    S.browse_error[0] = 0;
    fm_strlcpy(S.browse_key, key, sizeof S.browse_key);
    S.scroll[WP_HOME].y = 0;
    S.t_browse = task_start(WT_BROWSE, S.src, S.lang, grp, NULL, 1);
  } else if (!S.t_browse && S.browse.next > 0) {
    S.t_browse = task_start(WT_BROWSE, S.src, S.lang, grp, NULL, S.browse.next);
  }
}

static void article_start(int src, const char *lang, const char *id, const char *title, bool push);

static void search_start(const char *q, bool more) {
  if (!q || !q[0]) return;
  if (cur()->kind == SK_DICT) {                      /* a word opens at once */
    article_start(S.src, "en", q, q, true);
    return;
  }
  if (!more) {
    task_drop(&S.t_search);
    wk_list_free(&S.res);
    S.res.next = -1;
    S.res_error[0] = 0;
    S.res_src = S.src;
    fm_strlcpy(S.res_query, q, sizeof S.res_query);
    S.scroll[WP_RESULTS].y = 0;
    go(WP_RESULTS);
  } else if (S.t_search || S.res.next < 0) {
    return;
  }
  S.t_search = task_start(WT_SEARCH, S.res_src, S.lang, S.res_query, NULL, more ? S.res.next : kSrc[S.res_src].kind == SK_GUTEN ? 1 : 0);
}

/* Opens an article; push: the one on screen goes on the Back stack. */
static void article_start(int src, const char *lang, const char *id, const char *title, bool push) {
  if ((!title || !title[0]) && (!id || !id[0])) return;
  if (push && S.art_ok && S.nstack < WK_STACK) {
    S.stack[S.nstack].src = S.art_src;
    fm_strlcpy(S.stack[S.nstack].lang, S.art.lang, sizeof S.stack[0].lang);
    fm_strlcpy(S.stack[S.nstack].id, S.art.id, sizeof S.stack[0].id);
    fm_strlcpy(S.stack[S.nstack].title, S.art.title, sizeof S.stack[0].title);
    S.stack[S.nstack++].y = S.scroll[WP_ARTICLE].y;
  }
  task_drop(&S.t_art);
  task_drop(&S.t_media);
  task_drop(&S.t_langs);
  task_drop(&S.t_rel);
  task_drop(&S.t_links);
  fm_strlcpy(S.want_title, title && title[0] ? title : id, sizeof S.want_title);
  S.art_error[0] = 0;
  S.art_src = FM_CLAMP(src, 0, NSRC - 1);
  S.t_art = task_start(WT_ARTICLE, S.art_src, lang && lang[0] ? lang : S.lang, title, id, 0);
  S.page = WP_ARTICLE;
  ui_focus(0);
  ui_redraw();
}

static void random_start(void) {
  if (S.t_random || !cur()->random) return;
  S.t_random = task_start(WT_RANDOM, S.src, S.lang, NULL, NULL, 0);
  ui_toast("Finding a random page\xE2\x80\xA6");
}

static void sub_start(int page) {
  if (!S.art_ok) return;
  const Src *s = &kSrc[S.art_src];
  if (page == WP_MEDIA && !S.media_for && !S.t_media && s->kind == SK_WIKI) {
    S.media_error[0] = 0;
    S.t_media = task_start(WT_MEDIA, S.art_src, S.art.lang, S.art.title, NULL, 0);
  }
  if (page == WP_LANGS && !S.langs_for && !S.t_langs) {
    S.langs_error[0] = 0;
    S.t_langs = task_start(WT_LANGS, S.art_src, S.art.lang, S.art.title, NULL, 0);
  }
  if (page == WP_RELATED && !S.rel_for && !S.t_rel) {
    S.rel_error[0] = 0;
    S.t_rel = task_start(WT_RELATED, S.art_src, S.art.lang, S.art.title, NULL, 0);
    S.t_links = task_start(WT_LINKS, S.art_src, S.art.lang, S.art.title, NULL, 0);
  }
  S.scroll[page].y = 0;
  go(page);
}

static void media_open(const WkMedia *m) {
  WkTask **slot = m->type == WM_IMAGE ? &S.t_image : &S.t_play;
  if (*slot) return;
  *slot = task_start(m->type == WM_IMAGE ? WT_IMAGE : WT_PLAY, S.art_src, S.art.lang, m->file, m->url, m->type);
  ui_toast(m->type == WM_IMAGE ? "Opening the picture\xE2\x80\xA6" : m->type == WM_VIDEO ? "Opening the video\xE2\x80\xA6"
                                                                                        : "Opening the sound\xE2\x80\xA6");
}

static void open_web(const char *url) {
  if (!url || !url[0]) return;
  if (SDL_OpenURL(url) != 0 && !plat_open_external(url)) ui_toast("No browser found");
}

static void toggle_saved(void) {
  if (!S.art_ok) return;
  const char *src = kSrc[S.art_src].key;
  int i = ref_find(S.saved, S.nsaved, src, S.art.lang, S.art.id, S.art.title);
  if (i >= 0) {
    ref_remove(S.saved, &S.nsaved, i);
    ui_toast("Removed from Saved");
  } else {
    ref_front(S.saved, &S.nsaved, WK_SAVED_MAX, src, S.art.lang, S.art.id, S.art.title, S.art.desc, S.art.lead);
    ui_toast("Saved (also on the home screen's Favorites)");
  }
  save_file();
}

static void set_source(int i) {
  if (i < 0 || i >= NSRC || i == S.src) return;
  S.src = i;
  save_file();
  ui_focus(0);
  if (S.page != WP_HOME) go(WP_HOME);
  ui_redraw();
}

static void go_back(void) {
  if (is_sub(S.page)) {
    go(S.back_page == WP_ARTICLE && !S.art_ok && !S.t_art ? WP_HOME : S.back_page);
    return;
  }
  switch (S.page) {
    case WP_ARTICLE:
      if (S.nstack > 0) {
        S.nstack--;
        S.restore_y = S.stack[S.nstack].y;
        article_start(S.stack[S.nstack].src, S.stack[S.nstack].lang, S.stack[S.nstack].id, S.stack[S.nstack].title, false);
        return;
      }
      task_drop(&S.t_art);
      go(S.res.n || S.t_search ? WP_RESULTS : WP_HOME);
      return;
    case WP_RESULTS: go(WP_HOME); return;
    default: wiki_close(); return;
  }
}

/* ---- finished requests --------------------------------------------------------------------- */

static void layout_reset(void) {
  fm_free(S.bh);
  S.bh = NULL;
  S.lay_w = -1;
}

static void list_append(WkList *dst, WkList *src) {
  for (int i = 0; i < src->n; i++) {
    if (dst->n == dst->cap) {
      dst->cap = dst->cap ? dst->cap * 2 : 32;
      dst->it = (WkPage *)fm_realloc(dst->it, (size_t)dst->cap * sizeof *dst->it);
    }
    dst->it[dst->n++] = src->it[i];
  }
  dst->next = src->next;
}

void wiki_pump(void) {
  reap();
  if (task_done(S.t_feed)) {
    WkTask *t = S.t_feed;
    S.t_feed = NULL;
    if (t->err == FM_OK) {
      S.feed = t->feed;
      memset(&t->feed, 0, sizeof t->feed);
      S.feed_ok = true;
    } else {
      fm_strlcpy(S.feed_error, t->error, sizeof S.feed_error);
    }
    task_free(t);
    ui_redraw();
  }
  if (task_done(S.t_browse)) {
    WkTask *t = S.t_browse;
    S.t_browse = NULL;
    if (t->err == FM_OK) list_append(&S.browse, &t->list);
    else { fm_strlcpy(S.browse_error, t->error, sizeof S.browse_error); S.browse.next = -1; }
    if (!S.browse.n && !S.browse_error[0]) fm_strlcpy(S.browse_error, t->error[0] ? t->error : "Nothing here", sizeof S.browse_error);
    task_free(t);
    ui_redraw();
  }
  if (task_done(S.t_search)) {
    WkTask *t = S.t_search;
    S.t_search = NULL;
    if (t->err == FM_OK) {
      list_append(&S.res, &t->list);
      if (!S.res.n) fm_strlcpy(S.res_error, t->error[0] ? t->error : "Nothing found", sizeof S.res_error);
    } else {
      fm_strlcpy(S.res_error, t->error, sizeof S.res_error);
      S.res.next = -1;
    }
    task_free(t);
    ui_redraw();
  }
  if (task_done(S.t_art)) {
    WkTask *t = S.t_art;
    S.t_art = NULL;
    if (t->err == FM_OK) {
      wk_article_free(&S.art);
      S.art = t->art;
      memset(&t->art, 0, sizeof t->art);
      S.art_ok = true;
      S.art_src = t->src;
      layout_reset();
      wk_media_free(&S.media);
      wk_langs_free(&S.langs);
      wk_list_free(&S.rel);
      wk_list_free(&S.links);
      S.media_for = S.langs_for = S.rel_for = false;
      if (t->media.it || t->media.error[0]) {          /* the source sent its pictures or sounds along */
        S.media = t->media;
        memset(&t->media, 0, sizeof t->media);
        fm_strlcpy(S.media_error, S.media.error, sizeof S.media_error);
        S.media_for = true;
      }
      S.scroll[WP_ARTICLE].y = S.restore_y;
      S.restore_y = 0;
      fm_free(S.find_blk);
      S.find_blk = NULL;
      S.find_n = 0;
      S.find_ran[0] = 1;                             /* find again in the new page */
      ref_front(S.hist, &S.nhist, WK_HIST_MAX, kSrc[S.art_src].key, S.art.lang, S.art.id, S.art.title, S.art.desc,
                S.art.lead);
      save_file();
      if (S.demo_page > 0) {
        int p = S.demo_page;
        S.demo_page = -1;
        if (p == WP_TOC) go(WP_TOC);
        else sub_start(p);
      }
    } else {
      fm_strlcpy(S.art_error, t->error, sizeof S.art_error);
    }
    task_free(t);
    ui_redraw();
  }
  if (task_done(S.t_media)) {
    WkTask *t = S.t_media;
    S.t_media = NULL;
    wk_media_free(&S.media);
    S.media = t->media;
    memset(&t->media, 0, sizeof t->media);
    fm_strlcpy(S.media_error, t->err == FM_OK ? S.media.error : t->error, sizeof S.media_error);
    S.media_for = true;
    task_free(t);
    ui_redraw();
  }
  if (task_done(S.t_langs)) {
    WkTask *t = S.t_langs;
    S.t_langs = NULL;
    wk_langs_free(&S.langs);
    S.langs = t->langs;
    memset(&t->langs, 0, sizeof t->langs);
    fm_strlcpy(S.langs_error, t->err == FM_OK ? S.langs.error : t->error, sizeof S.langs_error);
    S.langs_for = true;
    task_free(t);
    ui_redraw();
  }
  if (task_done(S.t_rel) && task_done(S.t_links)) {
    wk_list_free(&S.rel);
    wk_list_free(&S.links);
    S.rel = S.t_rel->list;
    S.links = S.t_links->list;
    memset(&S.t_rel->list, 0, sizeof S.t_rel->list);
    memset(&S.t_links->list, 0, sizeof S.t_links->list);
    fm_strlcpy(S.rel_error, S.t_rel->err == FM_OK ? "" : S.t_rel->error, sizeof S.rel_error);
    task_free(S.t_rel);
    task_free(S.t_links);
    S.t_rel = S.t_links = NULL;
    S.rel_for = true;
    ui_redraw();
  }
  if (task_done(S.t_random)) {
    WkTask *t = S.t_random;
    S.t_random = NULL;
    if (t->err == FM_OK) article_start(t->src, t->lang, NULL, t->title, true);
    else ui_toast("No random page: %s", t->error);
    task_free(t);
  }
  if (task_done(S.t_image)) {
    WkTask *t = S.t_image;
    S.t_image = NULL;
    if (t->err == FM_OK) app_open(t->path, NULL, 0, 0);
    else ui_toast("Could not open the picture: %s", t->error);
    task_free(t);
  }
  if (task_done(S.t_play)) {
    WkTask *t = S.t_play;
    S.t_play = NULL;
    if (t->err != FM_OK) {
      ui_toast("Could not open it: %s", t->error);
    } else {
      char title[256];
      fm_strlcpy(title, t->arg + (strncmp(t->arg, "File:", 5) ? 0 : 5), sizeof title);
      char *dot = strrchr(title, '.');
      if (dot && !strchr(dot, ' ') && !strchr(dot, ')')) *dot = 0;
      if (t->num == WM_VIDEO) {
        video_open_stream(title, t->path, NULL);
      } else {
        FmAudioEntry e;
        memset(&e, 0, sizeof e);
        e.url = t->path;
        e.file = true;
        e.title = title;
        e.artist = kSrc[t->src].name;
        if (audio_play_entries(&e, 1, 0)) audio_show_player();
      }
    }
    task_free(t);
  }
}

/* ---- drawing helpers --------------------------------------------------------------------- */

static float txt(void) { return ui.m.font * S.zoom; }

static void thumb_box(FmRect r, const char *url, FmIcon icon, float aspect) {
  float rad = DP(10);
  if (url && url[0]) {
    float fade = 1;
    SDL_Texture *tex = othumb_get_ex(url, (int)FM_MAX(r.w, r.h) * 2, aspect, strstr(url, "wikimedia.org") ? wk_headers() : NULL,
                                     &fade);
    if (tex) {
      gfx_tex_rounded(tex, r, rad, FM_RGBA(255, 255, 255, (u8)(255 * fade)));
      if (fade < 1) ui_animate_busy(r);   /* the fade-in, at the loading rate */
      return;
    }
  }
  gfx_rrect(r, rad, T.surface2);
  float is = FM_MIN(r.w, r.h) * 0.42f;
  icon_draw(icon, rect_center(r, is, is), T.text2);
}

/* A list row: picture, title, one line under it. true when clicked. */
static bool row(u32 id, FmRect r, const char *title, const char *sub, const char *thumb, FmIcon icon) {
  int f = ui_hit(id, r);
  if (f & UI_HOVER) gfx_rrect(r, DP(10), T.surface2);
  float ts = r.h - DP(14);
  FmRect th = { r.x + DP(6), r.y + DP(7), ts, ts };
  thumb_box(th, thumb, icon, 1.0f);
  float x = th.x + th.w + DP(12), w = r.x + r.w - x - DP(8);
  float tl = font_line_h(ui.m.font), sl = font_line_h(ui.m.font_small);
  bool two = sub && sub[0];
  float y = r.y + (r.h - tl - (two ? sl : 0)) * 0.5f;
  font_draw_ellipsis(FONT_BOLD, ui.m.font, x, y, title, w, T.text);
  if (two) font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, x, y + tl, sub, w, T.text2);
  if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
  return (f & UI_CLICK) != 0;
}

static void section_label(float x, float *y, const char *s) {
  font_draw(FONT_BOLD, ui.m.font_title, x, *y, s, -1, T.text);
  *y += font_line_h(ui.m.font_title) + DP(8);
}

static void message(FmRect r, const char *s, bool busy) {
  float y = r.y + DP(30);
  if (busy) {
    ui_spinner(FM_RECT(r.x + r.w * 0.5f - DP(14), y, DP(28), DP(28)), T.accent);
    y += DP(40);
  }
  if (s && s[0]) {
    float w = FM_MIN(r.w - DP(32), DP(520));
    font_draw_wrap(FONT_REGULAR, ui.m.font, r.x + (r.w - w) * 0.5f, y, w, s, T.text2, true);
  }
}

/* A row of chip buttons that wraps; returns the one clicked or -1. */
static int chips(u32 base, float x, float *y, float w, const char *const *labels, int n, int on, const FmIcon *icons) {
  float bh = DP(ui.touch_mode ? 40 : 34), bx = x;
  int hit = -1;
  for (int i = 0; i < n; i++) {
    float cw = font_width(FONT_REGULAR, ui.m.font_small, labels[i], -1) + DP(icons ? 48 : 28);
    if (bx > x && bx + cw > x + w) { bx = x; *y += bh + DP(8); }
    if (ui_button(ui_idn(base, (u32)i), FM_RECT(bx, *y, cw, bh), icons ? icons[i] : IC_NONE, labels[i],
                  i == on ? UI_BTN_FILLED : UI_BTN_TONAL))
      hit = i;
    bx += cw + DP(8);
  }
  *y += bh;
  return hit;
}

/* ---- the top bar ---------------------------------------------------------------------------- */

enum { MI_RANDOM = 1, MI_SAVED, MI_HISTORY, MI_LANG, MI_BIGGER, MI_SMALLER, MI_WEB, MI_COPY, MI_RELATED, MI_LANGS,
       MI_CONTACT, MI_WIKI, MI_FIND };
static u32 g_menu_id;

static void more_menu(float x, float y) {
  FmMenuItem m[14];
  int n = 0;
  static char lang_label[80];
  fm_snprintf(lang_label, sizeof lang_label, "Language: %s", lang_name(S.lang));
  bool art = S.page == WP_ARTICLE && S.art_ok;
  const Src *s = art ? &kSrc[S.art_src] : cur();
  if (s->random) m[n++] = (FmMenuItem){ MI_RANDOM, IC_SHUFFLE, "Random page", NULL, 0 };
  m[n++] = (FmMenuItem){ MI_SAVED, IC_STAR, "Saved", NULL, 0 };
  m[n++] = (FmMenuItem){ MI_HISTORY, IC_HISTORY, "History", NULL, 0 };
  m[n++] = (FmMenuItem){ MI_LANG, IC_NETWORK, lang_label, NULL, 0 };
  m[n++] = (FmMenuItem){ MI_CONTACT, IC_PERSON, "Wikimedia contact", NULL, 0 };
  if (art) {
    m[n++] = (FmMenuItem){ MI_FIND, IC_SEARCH, "Find in page", "Ctrl+F", 0 };
    if (S.art.wiki[0]) m[n++] = (FmMenuItem){ MI_WIKI, IC_TEXT, "Read on Wikipedia", NULL, 0 };
    if (s->related) m[n++] = (FmMenuItem){ MI_RELATED, IC_LINK, "Related pages and links", NULL, 0 };
    if (s->langs) m[n++] = (FmMenuItem){ MI_LANGS, IC_NETWORK, "In other languages", NULL, 0 };
    m[n++] = (FmMenuItem){ MI_BIGGER, IC_ZOOM_IN, "Bigger text", NULL, 0 };
    m[n++] = (FmMenuItem){ MI_SMALLER, IC_ZOOM_OUT, "Smaller text", NULL, 0 };
    m[n++] = (FmMenuItem){ MI_WEB, IC_OPEN_WITH, "Open in browser", NULL, 0 };
    m[n++] = (FmMenuItem){ MI_COPY, IC_COPY, "Copy link", NULL, 0 };
  }
  ui_menu_open(g_menu_id, x, y, m, n);
}

static void open_wiki_of_art(void) {
  if (!S.art.wiki[0]) return;
  article_start(0, kSrc[S.art_src].kind == SK_DICT ? "en" : S.art.lang, NULL, S.art.wiki, true);
}

static void menu_results(void) {
  int r = ui_menu_result(g_menu_id);
  if (r <= 0) return;
  switch (r) {
    case MI_RANDOM: random_start(); break;
    case MI_SAVED: S.scroll[WP_SAVED].y = 0; go(WP_SAVED); break;
    case MI_HISTORY: S.scroll[WP_HISTORY].y = 0; go(WP_HISTORY); break;
    case MI_LANG: S.scroll[WP_PICKLANG].y = 0; go(WP_PICKLANG); break;
    case MI_RELATED: sub_start(WP_RELATED); break;
    case MI_LANGS: sub_start(WP_LANGS); break;
    case MI_WIKI: open_wiki_of_art(); break;
    case MI_FIND: S.find_open = S.find_focus = true; S.find_ran[0] = 1; break;
    case MI_BIGGER: S.zoom = FM_MIN(1.6f, S.zoom + 0.1f); layout_reset(); save_file(); break;
    case MI_SMALLER: S.zoom = FM_MAX(0.8f, S.zoom - 0.1f); layout_reset(); save_file(); break;
    case MI_WEB: open_web(S.art.url); break;
    case MI_COPY: ui_clipboard_set(S.art.url); ui_toast("Link copied"); break;
    case MI_CONTACT: fm_strlcpy(S.contact_field, S.contact, sizeof S.contact_field); go(WP_CONTACT); break;
    default: break;
  }
}

static const char *page_name(void) {
  switch (S.page) {
    case WP_RESULTS: return "Search";
    case WP_ARTICLE: return S.art_ok && !S.t_art ? S.art.title : S.want_title;
    case WP_MEDIA: return kSrc[S.art_src].kind == SK_DICT ? "Pronunciation" : "Pictures, videos and sounds";
    case WP_LANGS: return "Other languages";
    case WP_RELATED: return "Related pages and links";
    case WP_TOC: return "Contents";
    case WP_SAVED: return "Saved";
    case WP_HISTORY: return "History";
    case WP_PICKLANG: return "Language";
    case WP_CONTACT: return "Wikimedia contact";
    default: return "Online reading";
  }
}

static void draw_top(FmRect r, bool narrow) {
  u32 base = ui_id("wk.top");
  float bs = DP(ui.touch_mode ? 46 : 38);
  title_bar(r);
  FmRect in = r;
  title_buttons(&in);
  in = rect_inset2(in, DP(8), 0);
  FmRect b = rect_center(rect_cut_left(&in, bs), bs, bs);
  title_nodrag(b);
  if (ui_icon_btn(ui_idn(base, 1), b, IC_BACK, T.text, S.page == WP_HOME ? "Close Online reading" : "Back")) go_back();
  rect_cut_left(&in, DP(4));
  bool art = S.page == WP_ARTICLE && S.art_ok && !S.t_art;
  const Src *s = art ? &kSrc[S.art_src] : cur();
  if (!narrow) {
    FmRect m = rect_center(rect_cut_left(&in, DP(34)), DP(28), DP(28));
    gfx_rrect(m, DP(8), FM_HEX(0xF4F4F2));
    icon_draw(s->icon, rect_inset(m, DP(6)), FM_HEX(0x202122));
    rect_cut_left(&in, DP(8));
  }
  b = rect_center(rect_cut_right(&in, bs), bs, bs);
  title_nodrag(b);
  if (ui_icon_btn(ui_idn(base, 2), b, IC_MORE, T.text2, "More")) more_menu(b.x, b.y + b.h);
  if (art) {
    b = rect_center(rect_cut_right(&in, bs), bs, bs);
    title_nodrag(b);
    bool sv = ref_find(S.saved, S.nsaved, s->key, S.art.lang, S.art.id, S.art.title) >= 0;
    if (ui_icon_btn(ui_idn(base, 3), b, sv ? IC_STAR_FILL : IC_STAR, sv ? T.accent : T.text2, sv ? "Saved: remove" : "Save"))
      toggle_saved();
    bool media = s->media && (s->kind == SK_WIKI || S.media.n);
    if (media) {
      b = rect_center(rect_cut_right(&in, bs), bs, bs);
      title_nodrag(b);
      if (ui_icon_btn(ui_idn(base, 4), b, s->kind == SK_DICT ? IC_VOLUME : IC_IMAGE, T.text2,
                      s->kind == SK_DICT ? "Pronunciation" : "Pictures, videos and sounds"))
        sub_start(WP_MEDIA);
    }
    b = rect_center(rect_cut_right(&in, bs), bs, bs);
    title_nodrag(b);
    if (ui_icon_btn(ui_idn(base, 5), b, IC_LIST, T.text2, "Contents")) { S.scroll[WP_TOC].y = 0; go(WP_TOC); }
  }
  if (S.page != WP_HOME && S.page != WP_RESULTS) {
    b = rect_center(rect_cut_right(&in, bs), bs, bs);
    title_nodrag(b);
    if (ui_icon_btn(ui_idn(base, 6), b, IC_SEARCH, T.text2, "Search")) {
      S.focus_search = true;
      go(WP_HOME);
    }
  }
  char sub[160];
  if (s->langs) fm_snprintf(sub, sizeof sub, "%s \xC2\xB7 %s", s->name, lang_name(art ? S.art.lang : S.lang));
  else fm_strlcpy(sub, s->name, sizeof sub);
  float fs = ui.m.font_title * (narrow ? 0.9f : 1.0f);
  float tl = font_line_h(fs), sl = font_line_h(ui.m.font_small);
  float y = in.y + (in.h - tl - sl) * 0.5f;
  font_draw_ellipsis(FONT_BOLD, fs, in.x + DP(2), y, page_name(), in.w - DP(4), T.text);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, in.x + DP(2), y + tl, sub, in.w - DP(4), T.text2);
}

static void search_field(FmRect r) {
  u32 id = ui_id("wk.search");
  if (S.focus_search) { ui_focus(id); S.focus_search = false; }
  gfx_rrect(r, r.h * 0.5f, T.surface2);
  FmRect ic = rect_center(rect_cut_left(&r, r.h), DP(18), DP(18));
  icon_draw(IC_SEARCH, ic, T.text2);
  char hint[120];
  const Src *s = cur();
  switch (s->kind) {
    case SK_DICT: fm_strlcpy(hint, "Look up an English word", sizeof hint); break;
    case SK_GUTEN: fm_strlcpy(hint, "Search free books: a title or an author", sizeof hint); break;
    case SK_INAT: fm_strlcpy(hint, "Search plants and animals (mango, eagle, rose...)", sizeof hint); break;
    case SK_FRUIT: fm_strlcpy(hint, "Search fruits", sizeof hint); break;
    case SK_DEVTO: fm_strlcpy(hint, "A topic, one word: c, python, rust, beginners...", sizeof hint); break;
    case SK_SO: fm_strlcpy(hint, "Search questions (linked list in c, git undo commit...)", sizeof hint); break;
    default: fm_snprintf(hint, sizeof hint, "Search %s (%s)", s->name, s->langs ? lang_name(S.lang) : "English"); break;
  }
  int res = ui_textfield(id, rect_inset2(r, 0, DP(2)), S.field, sizeof S.field, hint, 0);
  if (res & UI_TF_SUBMIT) search_start(S.field, false);
}

/* The sources, as chips under the search field. */
static void source_chips(float x, float *y, float col) {
  const char *labels[NSRC];
  FmIcon icons[NSRC];
  for (int i = 0; i < NSRC; i++) { labels[i] = kSrc[i].name; icons[i] = kSrc[i].icon; }
  int hit = chips(ui_id("wk.src"), x, y, col, labels, NSRC, S.src, icons);
  if (hit >= 0) set_source(hit);
}

/* ---- pages ------------------------------------------------------------------------------------ */

static void recent_rows(float x, float *y, float col, const char *src_key, int max) {
  int shown = 0;
  float rh = DP(64);
  for (int i = 0; i < S.nhist && shown < max; i++) {
    if (src_key && strcmp(S.hist[i].src, src_key)) continue;
    if (!shown) section_label(x, y, "Recently read");
    FmRect c = { x, *y, col, rh };
    int s = src_find(S.hist[i].src);
    if (row(ui_idn(ui_id("wk.home.h"), (u32)i), c, S.hist[i].title, S.hist[i].desc, S.hist[i].thumb,
            s >= 0 ? kSrc[s].icon : IC_TEXT))
      article_start(s < 0 ? 0 : s, S.hist[i].lang, S.hist[i].id, S.hist[i].title, true);
    *y += rh;
    shown++;
  }
  if (shown) *y += DP(16);
}

static void home_wikipedia(float x, float *py, float col) {
  float y = *py;
  feed_start();
  if (!S.feed_ok) {
    message(FM_RECT(x, y - DP(20), col, DP(140)), S.t_feed ? "Loading today's Wikipedia\xE2\x80\xA6" : S.feed_error, S.t_feed != NULL);
    *py = y + DP(150);
    return;
  }
  WkFeed *f = &S.feed;
  u32 fb = ui_id("wk.feed");
  if (f->has_tfa) {
    section_label(x, &y, "Today's featured article");
    float h = DP(150);
    FmRect c = { x, y, col, h };
    int fl = ui_hit(ui_idn(fb, 1), c);
    gfx_rrect(c, DP(14), (fl & UI_HOVER) ? T.surface2 : T.surface);
    gfx_rrect_line(c, DP(14), DP(1), T.border);
    FmRect th = { c.x + DP(10), c.y + DP(10), h - DP(20), h - DP(20) };
    thumb_box(th, f->tfa.thumb, IC_TEXT, 1.0f);
    float tx = th.x + th.w + DP(14), tw = c.x + c.w - tx - DP(12);
    font_draw_ellipsis(FONT_BOLD, ui.m.font_title, tx, c.y + DP(12), f->tfa.title, tw, T.text);
    gfx_clip_push(FM_RECT(tx, c.y, tw, c.h - DP(10)));
    font_draw_wrap(FONT_REGULAR, ui.m.font_small, tx, c.y + DP(14) + font_line_h(ui.m.font_title), tw, f->tfa_extract, T.text2,
                   true);
    gfx_clip_pop();
    if (fl & UI_CLICK) article_start(0, S.lang, NULL, f->tfa.title, true);
    y += h + DP(22);
  }
  if (f->most.n) {
    section_label(x, &y, "Most read yesterday");
    float rh = DP(64);
    int cols = col >= DP(700) ? 2 : 1;
    float cw = (col - DP(10) * (cols - 1)) / cols;
    int n = FM_MIN(f->most.n, 12);
    for (int i = 0; i < n; i++) {
      FmRect c = { x + (i % cols) * (cw + DP(10)), y + (i / cols) * rh, cw, rh };
      char sub[300];
      fm_snprintf(sub, sizeof sub, "%d. %s", i + 1, f->most.it[i].desc);
      if (gfx_visible(c) && row(ui_idn(fb, 100 + (u32)i), c, f->most.it[i].title, sub, f->most.it[i].thumb, IC_TEXT))
        article_start(0, S.lang, NULL, f->most.it[i].title, true);
    }
    y += ((n + cols - 1) / cols) * rh + DP(22);
  }
  if (f->has_potd && f->potd_thumb[0]) {
    section_label(x, &y, "Picture of the day");
    float h = FM_MIN(col * 0.56f, DP(360));
    FmRect c = { x, y, col, h };
    int fl = ui_hit(ui_idn(fb, 2), c);
    thumb_box(c, f->potd_thumb, IC_IMAGE, col / h);
    if (fl & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
    if ((fl & UI_CLICK) && !S.t_image) {
      S.t_image = task_start(WT_IMAGE, 0, S.lang, f->potd_title, NULL, WM_IMAGE);
      ui_toast("Opening the picture\xE2\x80\xA6");
    }
    y += h + DP(6);
    y += font_draw_wrap(FONT_REGULAR, ui.m.font_small, x, y, col, f->potd_desc, T.text2, true) + DP(22);
  }
  if (f->notd) {
    section_label(x, &y, "On this day");
    for (int i = 0; i < f->notd; i++) {
      WkOtd *d = &f->otd[i];
      char yr[16];
      fm_snprintf(yr, sizeof yr, "%d", d->year);
      float th = font_draw_wrap(FONT_REGULAR, ui.m.font, x + DP(64), 0, col - DP(64), d->text, T.text, false);
      float h = FM_MAX(th, font_line_h(ui.m.font)) + DP(14);
      FmRect c = { x, y, col, h };
      int fl = d->page.title[0] ? ui_hit(ui_idn(fb, 300 + (u32)i), c) : 0;
      if (fl & UI_HOVER) { gfx_rrect(c, DP(8), T.surface2); ui_set_cursor(SDL_SYSTEM_CURSOR_HAND); }
      font_draw(FONT_BOLD, ui.m.font, x + DP(6), y + DP(7), yr, -1, T.accent);
      font_draw_wrap(FONT_REGULAR, ui.m.font, x + DP(64), y + DP(7), col - DP(70), d->text, T.text, true);
      if (fl & UI_CLICK) article_start(0, S.lang, NULL, d->page.title, true);
      y += h;
    }
    y += DP(16);
  }
  *py = y;
}

/* Books, a group's species, fruits: the source's own list on its home. */
static void home_browse(float x, float *py, float col, FmRect view) {
  float y = *py;
  const Src *s = cur();
  int ng;
  const Group *gs = groups_of(s->kind, &ng);
  int g = FM_CLAMP(S.group[S.src], 0, FM_MAX(ng - 1, 0));
  if (gs) {
    const char *labels[16];
    ng = FM_MIN(ng, (int)FM_COUNT(labels));
    for (int i = 0; i < ng; i++) labels[i] = gs[i].name;
    int hit = chips(ui_idn(ui_id("wk.grp"), (u32)S.src), x, &y, col, labels, ng, g, NULL);
    if (hit >= 0 && hit != g) S.group[S.src] = g = hit;
    y += DP(18);
  }
  browse_start(false);
  const char *label = "Every fruit";
  switch (s->kind) {
    case SK_GUTEN: label = strcmp(S.lang, "en") ? "Popular books in this language" : "Popular books"; break;
    case SK_INAT: label = "Most observed"; break;
    case SK_DEVTO: label = g == 0 ? "Most liked this week" : "Most liked this year"; break;
    case SK_SO: label = "Most voted questions"; break;
  }
  section_label(x, &y, label);
  if (!S.browse.n) {
    message(FM_RECT(x, y - DP(20), col, DP(140)),
            S.t_browse ? (s->kind == SK_GUTEN ? "Loading the books\xE2\x80\xA6 (Project Gutenberg can take 20 s)" : "Loading\xE2\x80\xA6")
                       : S.browse_error,
            S.t_browse != NULL);
    *py = y + DP(150);
    return;
  }
  float rh = DP(ui.touch_mode ? 72 : 64);
  int cols = col >= DP(700) ? 2 : 1;
  float cw = (col - DP(10) * (cols - 1)) / cols;
  u32 base = ui_id("wk.browse");
  for (int i = 0; i < S.browse.n; i++) {
    FmRect c = { x + (i % cols) * (cw + DP(10)), y + (i / cols) * rh, cw, rh };
    if (!gfx_visible(c)) continue;
    WkPage *p = &S.browse.it[i];
    if (row(ui_idn(base, (u32)i), c, p->title, p->desc, p->thumb, s->icon)) article_start(S.src, S.lang, p->id, p->title, true);
  }
  y += ((S.browse.n + cols - 1) / cols) * rh + DP(10);
  if (S.browse.next > 0 && y < view.y + view.h + rh * 4) browse_start(true);
  if (S.t_browse) {
    ui_spinner(FM_RECT(x + col * 0.5f - DP(12), y, DP(24), DP(24)), T.accent);
    y += DP(34);
  }
  *py = y + DP(16);
}

static void home_intro(float x, float *py, float col) {
  const Src *s = cur();
  float y = *py;
  font_draw_wrap(FONT_REGULAR, ui.m.font, x, y, col, s->about, T.text2, true);
  y += font_line_h(ui.m.font) + DP(10);
  if (s->random) {
    if (ui_button(ui_id("wk.home.rnd"), FM_RECT(x, y, DP(170), DP(ui.touch_mode ? 40 : 34)), IC_SHUFFLE, "Random page",
                  UI_BTN_TONAL))
      random_start();
    y += DP(ui.touch_mode ? 40 : 34) + DP(16);
  }
  *py = y + DP(6);
}

static void page_home(FmRect r) {
  FmScroll *sc = &S.scroll[WP_HOME];
  float col = FM_MIN(r.w - DP(24), DP(860));
  float x = r.x + (r.w - col) * 0.5f;
  static float content = 1000;
  ui_scroll(sc, ui_id("wk.home"), r, content);
  gfx_clip_push(r);
  float y = r.y + DP(8) - sc->y;
  float top = y;
  source_chips(x, &y, col);
  y += DP(10);
  /* saved, history, language */
  {
    char lab[64];
    fm_snprintf(lab, sizeof lab, "%s", lang_name(S.lang));
    const char *labels[] = { "Saved", "History", lab };
    const FmIcon icons[] = { IC_STAR, IC_HISTORY, IC_NETWORK };
    int hit = chips(ui_id("wk.home.btn"), x, &y, col, labels, 3, -1, icons);
    if (hit == 0) { S.scroll[WP_SAVED].y = 0; go(WP_SAVED); }
    else if (hit == 1) { S.scroll[WP_HISTORY].y = 0; go(WP_HISTORY); }
    else if (hit == 2) { S.scroll[WP_PICKLANG].y = 0; go(WP_PICKLANG); }
    y += DP(20);
  }
  const Src *s = cur();
  if (!S.contact[0] && s->kind == SK_WIKI) contact_card(x, &y, col, false);
  if (s->feed) {
    home_wikipedia(x, &y, col);
  } else if (has_browse(s->kind)) {
    home_browse(x, &y, col, r);
  } else {
    home_intro(x, &y, col);
  }
  recent_rows(x, &y, col, s->feed ? NULL : s->key, 6);
  gfx_clip_pop();
  content = y + sc->y - top + DP(30);
  ui_scrollbar(sc, r, content);
}

static void list_page(FmRect r, int page, const WkPage *it, int n, int src, const char *lang, u32 base, bool more_at_end) {
  FmScroll *sc = &S.scroll[page];
  float col = FM_MIN(r.w - DP(16), DP(900));
  float x = r.x + (r.w - col) * 0.5f, rh = DP(ui.touch_mode ? 72 : 64);
  float content = n * rh + DP(80);
  ui_scroll(sc, base, r, content);
  gfx_clip_push(r);
  float y = r.y + DP(8) - sc->y;
  for (int i = 0; i < n; i++) {
    FmRect c = { x, y + i * rh, col, rh };
    if (!gfx_visible(c)) continue;
    if (row(ui_idn(base, (u32)i + 1), c, it[i].title, it[i].desc, it[i].thumb, kSrc[src].icon))
      article_start(src, lang, it[i].id, it[i].title, true);
  }
  gfx_clip_pop();
  ui_scrollbar(sc, r, content);
  if (more_at_end && sc->y + r.h >= content - rh * 4) search_start(S.res_query, true);
}

static void page_results(FmRect r) {
  if (!S.res.n) {
    message(r, S.t_search ? (kSrc[S.res_src].kind == SK_GUTEN ? "Searching the books\xE2\x80\xA6 (Project Gutenberg can take 20 s)"
                                                              : "Searching\xE2\x80\xA6")
                          : S.res_error,
            S.t_search != NULL);
    return;
  }
  list_page(r, WP_RESULTS, S.res.it, S.res.n, S.res_src, S.lang, ui_id("wk.res"), S.res.next >= 0);
  if (S.t_search) {
    ui_spinner(FM_RECT(r.x + r.w * 0.5f - DP(12), r.y + r.h - DP(36), DP(24), DP(24)), T.accent);
  }
}

/* ---- the HTML document: pictures, links -------------------------------------------- */

/* A picture in a page: drawn in its own shape; one the page gave no size
** learns it from the picture (the page is laid out again). */
static void doc_thumb(FmDoc *d, DocImage *im, FmRect r) {
  float fade = 1;
  SDL_Texture *tex = othumb_get_ex(im->src, (int)FM_MAX(r.w, r.h) * 2, 0, strstr(im->src, "wikimedia.org") ? wk_headers() : NULL,
                                   &fade);
  int tw = 0, th = 0;
  bool layer = im->layer_of >= 0;                    /* a chess piece on its board: no box while it loads */
  if (!tex || SDL_QueryTexture(tex, NULL, NULL, &tw, &th) != 0 || tw <= 0 || th <= 0) {
    if (layer) return;
    gfx_rrect(r, DP(8), T.surface2);
    float is = FM_MIN(r.w, r.h) * 0.3f;
    icon_draw(IC_IMAGE, rect_center(r, is, is), T.text2);
    return;
  }
  doc_image_shape(d, im, (int)(r.w / ui.scale), (int)((float)(r.w / ui.scale) * th / tw));
  float k = FM_MIN(r.w / tw, r.h / th);
  FmColor tint = FM_RGBA(255, 255, 255, (u8)(255 * fade));
  if (layer) gfx_tex(tex, NULL, rect_center(r, tw * k, th * k), tint);
  else gfx_tex_rounded(tex, rect_center(r, tw * k, th * k), DP(6), tint);
  if (fade < 1) ui_animate_busy(r);   /* the fade-in, at the loading rate */
}

/* A wiki page title from a link inside a wiki page ("/wiki/Mango",
** "./Mango", "https://en.wikipedia.org/wiki/Mango#Fruit"); false = not one. */
static bool wiki_title(const char *href, char *host, size_t hcap, char *out, size_t cap) {
  host[0] = 0;
  const char *p = NULL;
  if (!strncmp(href, "/wiki/", 6)) p = href + 6;
  else if (!strncmp(href, "./", 2)) p = href + 2;
  else if (!strncmp(href, "https://", 8) || !strncmp(href, "//", 2)) {
    const char *h = href + (href[0] == '/' ? 2 : 8);
    const char *w = strstr(h, "/wiki/");
    if (!w || (size_t)(w - h) >= hcap) return false;
    memcpy(host, h, (size_t)(w - h));
    host[w - h] = 0;
    if (!wk_host_ok(host)) return false;
    p = w + 6;
  }
  if (!p || !*p) return false;
  char raw[600];
  fm_strlcpy(raw, p, sizeof raw);
  raw[strcspn(raw, "#?")] = 0;
  doc_url_decode(raw, out, cap);
  return out[0] != 0;
}

/* A link to a Stack Overflow question or a dev.to article opens here, in
** its source; true = it did. */
static bool open_site_link(const char *url) {
  const char *p = NULL;
  if (!strncmp(url, "https://stackoverflow.com/", 26)) p = url + 26;
  else if (!strncmp(url, "http://stackoverflow.com/", 25)) p = url + 25;
  if (p) {
    /* questions/<id>/<slug>, q/<id> */
    if (!strncmp(p, "questions/", 10)) p += 10;
    else if (!strncmp(p, "q/", 2)) p += 2;
    else return false;
    char id[16];
    size_t n = 0;
    while (p[n] >= '0' && p[n] <= '9' && n + 1 < sizeof id) { id[n] = p[n]; n++; }
    id[n] = 0;
    if (!n || (p[n] && p[n] != '/' && p[n] != '?' && p[n] != '#')) return false;
    article_start(src_find("stackoverflow"), "en", id, NULL, true);
    return true;
  }
  if (!strncmp(url, "https://dev.to/", 15)) {
    /* <user>/<slug>: two parts, letters, digits, - and _ */
    char path[300];
    fm_strlcpy(path, url + 15, sizeof path);
    path[strcspn(path, "?#")] = 0;
    size_t n = strlen(path);
    if (n && path[n - 1] == '/') path[--n] = 0;
    const char *slash = strchr(path, '/');
    if (!slash || slash == path || !slash[1] || strchr(slash + 1, '/')) return false;
    if (!strncmp(path, "t/", 2) || !strncmp(path, "api/", 4) || !strncmp(path, "search", 6)) return false;
    for (const char *c = path; *c; c++)
      if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || strchr("-_/", *c))) return false;
    article_start(src_find("devto"), "en", NULL, path, true);   /* the article names its number */
    return true;
  }
  return false;
}

static void open_doc_link(const char *href) {
  const Src *s = &kSrc[S.art_src];
  FmDoc *d = S.art.doc;
  if (href[0] == '#') {                              /* a place in this page */
    int b = d ? doc_anchor(d, href + 1) : -1;
    if (b >= 0) {
      S.scroll[WP_ARTICLE].y = S.lay_top + d->b[b].y - DP(8);
      S.scroll[WP_ARTICLE].vel = 0;
      ui_redraw();
    }
    return;
  }
  char host[64], title[512];
  if (s->kind == SK_WIKI && wiki_title(href, host, sizeof host, title, sizeof title)) {
    const char *colon = strchr(title, ':');
    if (colon && (!fm_strnicmp(title, "File:", 5) || !fm_strnicmp(title, "Image:", 6))) {
      if (!S.t_image) {
        S.t_image = task_start(WT_IMAGE, S.art_src, S.art.lang, title, NULL, WM_IMAGE);
        ui_toast("Opening the picture\xE2\x80\xA6");
      }
      return;
    }
    if (!colon || !host[0]) {
      /* another wiki of ours (a link to Wiktionary, or another language) */
      int src = S.art_src;
      char lang[WK_LANG];
      fm_strlcpy(lang, S.art.lang, sizeof lang);
      if (host[0]) {
        for (int i = 0; i < NSRC; i++) {
          if (kSrc[i].kind != SK_WIKI) continue;
          const char *dot = strchr(host, '.');
          char suffix[64];
          fm_strlcpy(suffix, kSrc[i].host, sizeof suffix);
          const char *ps = strstr(suffix, "%s");
          if (ps && dot && !strcmp(dot, ps + 2) && (size_t)(dot - host) < sizeof lang) {
            src = i;
            memcpy(lang, host, (size_t)(dot - host));
            lang[dot - host] = 0;
          } else if (!ps && !strcmp(host, suffix)) {
            src = i;
          }
        }
      }
      if (!colon) { article_start(src, lang, NULL, title, true); return; }
    }
  }
  char abs[1200];
  if (!strncmp(href, "http://", 7) || !strncmp(href, "https://", 8)) fm_strlcpy(abs, href, sizeof abs);
  else if (!strncmp(href, "//", 2)) fm_snprintf(abs, sizeof abs, "https:%s", href);
  else if (s->kind == SK_WIKI && href[0] == '/') {
    char h2[64];
    host_of(S.art_src, S.art.lang, h2, sizeof h2);
    fm_snprintf(abs, sizeof abs, "https://%s%s", h2, href);
  } else if (s->kind == SK_GUTEN) {
    fm_snprintf(abs, sizeof abs, "https://www.gutenberg.org/cache/epub/%s/%s", S.art.id, href);
  } else if (href[0] == '/' && (s->kind == SK_SO || s->kind == SK_DEVTO)) {
    fm_snprintf(abs, sizeof abs, "https://%s%s", s->kind == SK_SO ? "stackoverflow.com" : "dev.to", href);
  } else {
    fm_strlcpy(abs, href, sizeof abs);
  }
  if (open_site_link(abs)) return;
  open_web(abs);
}

static bool image_like(const char *u) {
  char t[600];
  fm_strlcpy(t, u, sizeof t);
  t[strcspn(t, "?#")] = 0;
  size_t n = strlen(t);
  static const char *const kExt[] = { ".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp" };
  for (int i = 0; i < FM_COUNT(kExt); i++) {
    size_t e = strlen(kExt[i]);
    if (n > e && !fm_stricmp(t + n - e, kExt[i])) return true;
  }
  return false;
}

static void open_doc_image(const DocImage *im) {
  if (S.t_image) return;
  char host[64], title[512];
  if (im->href && kSrc[S.art_src].kind == SK_WIKI && wiki_title(im->href, host, sizeof host, title, sizeof title) &&
      (!fm_strnicmp(title, "File:", 5) || !fm_strnicmp(title, "Image:", 6))) {
    S.t_image = task_start(WT_IMAGE, S.art_src, S.art.lang, title, NULL, WM_IMAGE);   /* full size, from the wiki */
  } else {
    char url[1100];
    const char *u = im->href && image_like(im->href) ? im->href : im->src;
    if (!strncmp(u, "http", 4)) fm_strlcpy(url, u, sizeof url);
    else if (!strncmp(u, "//", 2)) fm_snprintf(url, sizeof url, "https:%s", u);
    else if (kSrc[S.art_src].kind == SK_GUTEN) fm_snprintf(url, sizeof url, "https://www.gutenberg.org/cache/epub/%s/%s", S.art.id, u);
    else fm_strlcpy(url, im->src, sizeof url);
    S.t_image = task_start(WT_IMAGE, S.art_src, S.art.lang, im->alt ? im->alt : "Picture", url, WM_IMAGE);
  }
  ui_toast("Opening the picture\xE2\x80\xA6");
}

/* Article layout: block heights at the current width and text size. */
static float block_font(int kind) {
  return kind == WB_H2 ? ui.m.font_title * 1.25f * S.zoom : kind == WB_H3 ? ui.m.font_title * 1.05f * S.zoom
       : kind == WB_H4 ? txt() * 1.05f : txt();
}

static void layout(float w) {
  if (S.bh && S.lay_w == w && S.lay_zoom == S.zoom && S.lay_gen == font_generation()) return;
  S.lay_gen = font_generation();
  fm_free(S.bh);
  S.bh = (float *)fm_alloc((size_t)(S.art.nb ? S.art.nb : 1) * sizeof *S.bh);
  S.lay_w = w;
  S.lay_zoom = S.zoom;
  float total = 0;
  for (int i = 0; i < S.art.nb; i++) {
    const WkBlock *b = &S.art.b[i];
    float fs = block_font(b->kind);
    float h = font_draw_wrap(b->kind == WB_PARA ? FONT_REGULAR : FONT_BOLD, fs, 0, 0, w, b->s, T.text, false);
    h += b->kind == WB_PARA ? DP(12) * S.zoom : b->kind == WB_H2 ? DP(30) : DP(18);
    S.bh[i] = h;
    total += h;
  }
  S.lay_total = total;
}

/* ---- find in page ------------------------------------------------------------------------ */

static char fold_c(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

static int count_in(const char *s, const char *q, size_t qn) {
  int n = 0;
  for (; *s; s++) {
    size_t k = 0;
    while (k < qn && s[k] && fold_c(s[k]) == fold_c(q[k])) k++;
    if (k == qn) { n++; s += qn - 1; }
  }
  return n;
}

/* Where match i sits, as the article's scroll position. */
static float find_y(int i) {
  if (S.art.doc) return S.lay_top + doc_offset_y(S.art.doc, S.art.doc->hl[i]);
  float y = S.lay_top;
  int b = S.find_blk ? S.find_blk[i] : 0;
  for (int k = 0; k < b && k < S.art.nb && S.bh; k++) y += S.bh[k];
  return y;
}

static void find_show(int i) {
  if (S.find_n <= 0) return;
  S.find_cur = (i % S.find_n + S.find_n) % S.find_n;
  if (S.art.doc) S.art.doc->hl_cur = S.find_cur;
  S.scroll[WP_ARTICLE].y = FM_MAX(0, find_y(S.find_cur) - S.art_view_h * 0.3f);
  S.scroll[WP_ARTICLE].vel = 0;
  ui_redraw();
}

/* The query changed: every match again, then the first one below the view. */
static void find_run(void) {
  fm_strlcpy(S.find_ran, S.find_q, sizeof S.find_ran);
  fm_free(S.find_blk);
  S.find_blk = NULL;
  S.find_n = 0;
  S.find_cur = -1;
  size_t qn = strlen(S.find_q);
  if (S.art.doc) {
    S.find_n = doc_find(S.art.doc, qn ? S.find_q : NULL);
  } else if (qn) {
    int cap = 0;
    for (int b = 0; b < S.art.nb; b++) {
      int c = count_in(S.art.b[b].s, S.find_q, qn);
      for (int k = 0; k < c; k++) {
        if (S.find_n == cap) {
          cap = cap ? cap * 2 : 64;
          S.find_blk = (int *)fm_realloc(S.find_blk, (size_t)cap * sizeof *S.find_blk);
        }
        S.find_blk[S.find_n++] = b;
      }
    }
  }
  if (!S.find_n) return;
  float at = S.scroll[WP_ARTICLE].y + S.art_view_h * 0.3f - DP(4);
  int first = 0;
  while (first < S.find_n && find_y(first) < at) first++;
  find_show(first < S.find_n ? first : 0);
}

static void find_close(void) {
  S.find_open = false;
  S.find_q[0] = S.find_ran[0] = 0;
  if (S.art.doc) doc_find(S.art.doc, NULL);
  fm_free(S.find_blk);
  S.find_blk = NULL;
  S.find_n = 0;
  ui_focus(0);
  ui_redraw();
}

static void find_bar(FmRect r) {
  float bh = DP(ui.touch_mode ? 52 : 46), w = FM_MIN(r.w - DP(16), DP(560));
  FmRect c = { r.x + (r.w - w) * 0.5f, r.y + r.h - bh - DP(10), w, bh };
  gfx_shadow(c, DP(12), DP(12), col_alpha(T.shadow, 0.8f));
  gfx_rrect(c, DP(12), T.surface2);
  gfx_rrect_line(c, DP(12), DP(1), T.border);
  FmRect in = rect_inset2(c, DP(8), DP(6));
  float bs = in.h;
  u32 base = ui_id("wk.find");
  FmRect bx = rect_cut_right(&in, bs);
  if (ui_icon_btn(ui_idn(base, 1), bx, IC_CLOSE, T.text2, "Close (Esc)")) { find_close(); return; }
  bx = rect_cut_right(&in, bs);
  if (ui_icon_btn(ui_idn(base, 2), bx, IC_CHEVRON_DOWN, T.text, "Next (Enter, F3)")) find_show(S.find_cur + 1);
  bx = rect_cut_right(&in, bs);
  if (ui_icon_btn(ui_idn(base, 3), bx, IC_CHEVRON_UP, T.text, "Previous (Shift+F3)")) find_show(S.find_cur - 1);
  char cnt[40];
  if (!S.find_q[0]) cnt[0] = 0;
  else if (!S.find_n) fm_strlcpy(cnt, "No match", sizeof cnt);
  else fm_snprintf(cnt, sizeof cnt, "%d of %d", S.find_cur + 1, S.find_n);
  float cw = font_width(FONT_REGULAR, ui.m.font_small, cnt, -1) + DP(12);
  FmRect cr = rect_cut_right(&in, cw);
  font_draw(FONT_REGULAR, ui.m.font_small, cr.x + DP(4), cr.y + (cr.h - font_line_h(ui.m.font_small)) * 0.5f, cnt, -1,
            S.find_q[0] && !S.find_n ? T.warn : T.text2);
  FmRect ic = rect_center(rect_cut_left(&in, DP(28)), DP(16), DP(16));
  icon_draw(IC_SEARCH, ic, T.text2);
  u32 fid = ui_idn(base, 4);
  if (S.find_focus) { ui_focus(fid); S.find_focus = false; }
  int res = ui_textfield(fid, in, S.find_q, sizeof S.find_q, "Find in page", 0);
  if (strcmp(S.find_q, S.find_ran)) find_run();
  if (res & UI_TF_SUBMIT) { find_show(S.find_cur + 1); ui_focus(fid); }
  if (ui_key(SDLK_F3, KMOD_SHIFT)) find_show(S.find_cur - 1);
  else if (ui_key(SDLK_F3, 0)) find_show(S.find_cur + 1);
}

static void page_article(FmRect r) {
  if (S.t_art || !S.art_ok) {
    bool book = S.t_art && kSrc[S.t_art->src].kind == SK_GUTEN;
    message(r, S.t_art ? (book ? "Opening the book\xE2\x80\xA6 (a whole book: it can take a while)" : "Opening\xE2\x80\xA6") : S.art_error,
            S.t_art != NULL);
    return;
  }
  FmScroll *sc = &S.scroll[WP_ARTICLE];
  float col = FM_MIN(r.w - DP(32), DP(780));
  float x = r.x + (r.w - col) * 0.5f;
  FmDoc *doc = S.art.doc;
  S.art_view_h = r.h;
  if (doc) {
    S.lay_total = doc_layout(doc, col, txt());
  } else {
    layout(col);
  }
  float lead_h = 0, ar = 1.5f;
  /* a page from HTML shows its own pictures; a book its cover first */
  if (S.art.lead[0] && (!doc || kSrc[S.art_src].kind == SK_GUTEN || kSrc[S.art_src].kind == SK_DEVTO)) {
    ar = S.art.lead_w > 0 && S.art.lead_h > 0 ? (float)S.art.lead_w / (float)S.art.lead_h : kSrc[S.art_src].kind == SK_GUTEN ? 0.66f : 1.5f;
    lead_h = FM_MIN(col / ar, r.h * 0.5f);
  }
  float title_fs = ui.m.font_title * 1.6f * S.zoom;
  float title_h = font_draw_wrap(FONT_BOLD, title_fs, 0, 0, col, S.art.title, T.text, false);
  float desc_h = S.art.desc[0] ? font_draw_wrap(FONT_REGULAR, txt(), 0, 0, col, S.art.desc, T.text2, false) : 0;
  float head = DP(16) + (lead_h > 0 ? lead_h + DP(16) : 0) + title_h + DP(6) + desc_h + DP(18);
  float content = head + S.lay_total + DP(120);
  ui_scroll(sc, ui_id("wk.art"), r, content);
  gfx_clip_push(r);
  float y = r.y - sc->y + DP(16);
  if (lead_h > 0) {
    float lw = FM_MIN(col, lead_h * ar);
    FmRect lr = { x + (col - lw) * 0.5f, y, lw, lead_h };
    if (gfx_visible(lr)) thumb_box(lr, S.art.lead, IC_IMAGE, 0);
    y += lead_h + DP(16);
  }
  font_draw_wrap(FONT_BOLD, title_fs, x, y, col, S.art.title, T.text, true);
  y += title_h + DP(6);
  if (desc_h > 0) font_draw_wrap(FONT_REGULAR, txt(), x, y, col, S.art.desc, T.text2, true);
  y += desc_h + DP(18);
  S.lay_top = y + sc->y - r.y;              /* where the first block starts, scroll 0 */
  if (doc) {
    DocHit hit = doc_draw(doc, x, y, r, ui_id("wk.doc"), doc_thumb);
    y += doc->total;
    if (hit.link >= 0 && hit.link < doc->nlinks) open_doc_link(doc->links[hit.link]);
    if (hit.img >= 0 && hit.img < doc->nimgs) open_doc_image(&doc->imgs[hit.img]);
  }
  for (int i = 0; i < (doc ? 0 : S.art.nb); i++) {
    float h = S.bh[i];
    if (y + h >= r.y && y <= r.y + r.h) {
      const WkBlock *b = &S.art.b[i];
      float fs = block_font(b->kind);
      if (b->kind == WB_PARA) {
        font_draw_wrap(FONT_REGULAR, fs, x, y, col, b->s, T.text, true);
      } else {
        float pad = b->kind == WB_H2 ? DP(18) : DP(10);
        float th = font_draw_wrap(FONT_BOLD, fs, x, y + pad, col, b->s, T.text, true);
        if (b->kind == WB_H2) ui_divider(x, x + col, y + pad + th + DP(4));
      }
    }
    y += h;
  }
  /* the end: where the article leads */
  y += DP(10);
  const Src *s = &kSrc[S.art_src];
  const char *labels[5];
  FmIcon icons[5];
  int act[5], n = 0;
  if (s->media && (s->kind == SK_WIKI || S.media.n)) {
    labels[n] = s->kind == SK_DICT ? "Hear it" : "Pictures, videos, sounds";
    icons[n] = s->kind == SK_DICT ? IC_VOLUME : IC_IMAGE;
    act[n++] = WP_MEDIA;
  }
  if (s->related) { labels[n] = "Related and links"; icons[n] = IC_LINK; act[n++] = WP_RELATED; }
  if (s->langs) { labels[n] = "Other languages"; icons[n] = IC_NETWORK; act[n++] = WP_LANGS; }
  if (S.art.wiki[0] && S.art_src != 0) { labels[n] = "Read on Wikipedia"; icons[n] = IC_TEXT; act[n++] = -1; }
  if (n) {
    int hit = chips(ui_id("wk.art.end"), x, &y, col, labels, n, -1, icons);
    if (hit >= 0) {
      if (act[hit] < 0) open_wiki_of_art();
      else sub_start(act[hit]);
    }
  }
  gfx_clip_pop();
  ui_scrollbar(sc, r, content);
  if (S.find_open) find_bar(r);
  if (ui_input_ok() && !ui.focus && !ui_menu_is_open()) {
    if (ui_key(SDLK_EQUALS, KMOD_CTRL) || ui_key(SDLK_PLUS, KMOD_CTRL)) { S.zoom = FM_MIN(1.6f, S.zoom + 0.1f); save_file(); }
    if (ui_key(SDLK_MINUS, KMOD_CTRL)) { S.zoom = FM_MAX(0.8f, S.zoom - 0.1f); save_file(); }
    if (ui_key(SDLK_PAGEDOWN, 0) || ui_key(SDLK_SPACE, 0)) sc->y = FM_MIN(sc->y + r.h * 0.9f, FM_MAX(0, content - r.h));
    if (ui_key(SDLK_PAGEUP, 0)) sc->y = FM_MAX(0, sc->y - r.h * 0.9f);
    if (ui_key(SDLK_HOME, 0)) sc->y = 0;
  }
}

/* Contents of an HTML page: its headings (levels 1..4). */
static void page_toc_doc(FmRect r, FmDoc *d) {
  FmScroll *sc = &S.scroll[WP_TOC];
  float col = FM_MIN(r.w - DP(16), DP(700)), x = r.x + (r.w - col) * 0.5f, rh = DP(ui.touch_mode ? 48 : 40);
  int n = 0;
  for (int i = 0; i < d->nb; i++) n += d->b[i].kind == DB_H && d->b[i].level <= 4;
  float content = (n + 1) * rh + DP(40);
  ui_scroll(sc, ui_id("wk.toc"), r, content);
  gfx_clip_push(r);
  float y = r.y + DP(8) - sc->y;
  u32 base = ui_id("wk.toc.row");
  int lv0 = 6;
  for (int i = 0; i < d->nb; i++)
    if (d->b[i].kind == DB_H) lv0 = FM_MIN(lv0, d->b[i].level);
  for (int i = -1; i < d->nb; i++) {
    if (i >= 0 && (d->b[i].kind != DB_H || d->b[i].level > 4)) continue;
    FmRect c = { x, y, col, rh };
    y += rh;
    if (!gfx_visible(c)) continue;
    char label[200];
    if (i < 0) fm_strlcpy(label, "(Top)", sizeof label);
    else doc_block_text(d, i, label, sizeof label);
    int lv = i < 0 ? lv0 : d->b[i].level;
    float indent = DP(22) * FM_MAX(0, lv - lv0);
    int f = ui_hit(ui_idn(base, (u32)(i + 1)), c);
    if (f & UI_HOVER) gfx_rrect(c, DP(8), T.surface2);
    font_draw_ellipsis(lv == lv0 ? FONT_BOLD : FONT_REGULAR, ui.m.font, x + DP(10) + indent, c.y + (rh - font_line_h(ui.m.font)) * 0.5f,
                       label, col - DP(20) - indent, T.text);
    if (f & UI_CLICK) {
      S.scroll[WP_ARTICLE].y = i < 0 ? 0 : S.lay_top + d->b[i].y - DP(8);
      S.scroll[WP_ARTICLE].vel = 0;
      go(WP_ARTICLE);
    }
  }
  gfx_clip_pop();
  ui_scrollbar(sc, r, content);
}

static void page_toc(FmRect r) {
  if (!S.art_ok) { go(WP_ARTICLE); return; }
  if (S.art.doc) { page_toc_doc(r, S.art.doc); return; }
  FmScroll *sc = &S.scroll[WP_TOC];
  float col = FM_MIN(r.w - DP(16), DP(700)), x = r.x + (r.w - col) * 0.5f, rh = DP(ui.touch_mode ? 48 : 40);
  int n = 0;
  for (int i = 0; i < S.art.nb; i++) n += S.art.b[i].kind != WB_PARA;
  float content = (n + 1) * rh + DP(40);
  ui_scroll(sc, ui_id("wk.toc"), r, content);
  gfx_clip_push(r);
  float y = r.y + DP(8) - sc->y, at = 0;
  u32 base = ui_id("wk.toc.row");
  layout(S.lay_w > 0 ? S.lay_w : FM_MIN(r.w - DP(32), DP(780)));
  for (int i = -1; i < S.art.nb; i++) {
    if (i >= 0 && S.art.b[i].kind == WB_PARA) { at += S.bh[i]; continue; }
    FmRect c = { x, y, col, rh };
    if (gfx_visible(c)) {
      const char *label = i < 0 ? "(Top)" : S.art.b[i].s;
      float indent = i < 0 ? 0 : S.art.b[i].kind == WB_H2 ? 0 : S.art.b[i].kind == WB_H3 ? DP(22) : DP(44);
      int f = ui_hit(ui_idn(base, (u32)(i + 1)), c);
      if (f & UI_HOVER) gfx_rrect(c, DP(8), T.surface2);
      font_draw_ellipsis(i < 0 || S.art.b[i].kind == WB_H2 ? FONT_BOLD : FONT_REGULAR, ui.m.font, x + DP(10) + indent,
                         y + (rh - font_line_h(ui.m.font)) * 0.5f, label, col - DP(20) - indent, T.text);
      if (f & UI_CLICK) {
        S.scroll[WP_ARTICLE].y = i < 0 ? 0 : S.lay_top + at - DP(8);
        S.scroll[WP_ARTICLE].vel = 0;
        go(WP_ARTICLE);
      }
    }
    if (i >= 0) at += S.bh[i];
    y += rh;
  }
  gfx_clip_pop();
  ui_scrollbar(sc, r, content);
}

static void page_media(FmRect r) {
  if (S.t_media || !S.media.n) {
    message(r, S.t_media ? "Finding the pictures, videos and sounds\xE2\x80\xA6" : S.media_error, S.t_media != NULL);
    return;
  }
  FmScroll *sc = &S.scroll[WP_MEDIA];
  float gap = DP(10), pad = DP(12);
  float avail = r.w - pad * 2;
  int cols = FM_MAX(2, (int)(avail / DP(200)));
  float tw = (avail - gap * (cols - 1)) / cols, th = tw * 0.75f, cap_h = font_line_h(ui.m.font_small) * 2 + DP(6);
  int rows = (S.media.n + cols - 1) / cols;
  float content = rows * (th + cap_h + gap) + pad * 2;
  ui_scroll(sc, ui_id("wk.media"), r, content);
  gfx_clip_push(r);
  u32 base = ui_id("wk.media.tile");
  for (int i = 0; i < S.media.n; i++) {
    WkMedia *m = &S.media.it[i];
    FmRect c = { r.x + pad + (i % cols) * (tw + gap), r.y + pad - sc->y + (i / cols) * (th + cap_h + gap), tw, th };
    FmRect whole = { c.x, c.y, c.w, c.h + cap_h };
    if (!gfx_visible(whole)) continue;
    int f = ui_hit(ui_idn(base, (u32)i), whole);
    thumb_box(c, m->thumb, m->type == WM_AUDIO ? IC_MUSIC : m->type == WM_VIDEO ? IC_VIDEO : IC_IMAGE, tw / th);
    if (m->type != WM_IMAGE) {
      float bs = DP(40);
      FmRect bd = rect_center(c, bs, bs);
      gfx_circle(bd.x + bs * 0.5f, bd.y + bs * 0.5f, bs * 0.5f, FM_RGBA(0, 0, 0, 150));
      icon_draw(m->type == WM_VIDEO ? IC_PLAY : IC_MUSIC, rect_inset(bd, DP(10)), FM_HEX(0xFFFFFF));
    }
    const char *cap = m->caption[0] ? m->caption : !strncmp(m->file, "File:", 5) ? m->file + 5 : m->file;
    gfx_clip_push(FM_RECT(c.x, c.y + c.h, c.w, cap_h));
    font_draw_wrap(FONT_REGULAR, ui.m.font_small, c.x + DP(2), c.y + c.h + DP(4), c.w - DP(4), cap, T.text2, true);
    gfx_clip_pop();
    if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
    if (f & UI_CLICK) media_open(m);
  }
  gfx_clip_pop();
  ui_scrollbar(sc, r, content);
}

static void page_langs(FmRect r) {
  if (S.t_langs || !S.langs.n) {
    message(r, S.t_langs ? "Finding the other languages\xE2\x80\xA6" : S.langs_error, S.t_langs != NULL);
    return;
  }
  FmScroll *sc = &S.scroll[WP_LANGS];
  float col = FM_MIN(r.w - DP(16), DP(700)), x = r.x + (r.w - col) * 0.5f, rh = DP(ui.touch_mode ? 56 : 50);
  float content = S.langs.n * rh + DP(40);
  ui_scroll(sc, ui_id("wk.langs"), r, content);
  gfx_clip_push(r);
  u32 base = ui_id("wk.langs.row");
  for (int i = 0; i < S.langs.n; i++) {
    WkLang *l = &S.langs.it[i];
    FmRect c = { x, r.y + DP(8) - sc->y + i * rh, col, rh };
    if (!gfx_visible(c)) continue;
    int f = ui_hit(ui_idn(base, (u32)i), c);
    if (f & UI_HOVER) gfx_rrect(c, DP(8), T.surface2);
    float tl = font_line_h(ui.m.font), sl = font_line_h(ui.m.font_small);
    float ty = c.y + (rh - tl - sl) * 0.5f;
    font_draw_ellipsis(FONT_BOLD, ui.m.font, c.x + DP(12), ty, l->title, col - DP(24), T.text);
    char sub[100];
    fm_snprintf(sub, sizeof sub, "%s (%s)", l->autonym, l->lang);
    font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, c.x + DP(12), ty + tl, sub, col - DP(24), T.text2);
    if (f & UI_CLICK) article_start(S.art_src, l->lang, NULL, l->title, true);
  }
  gfx_clip_pop();
  ui_scrollbar(sc, r, content);
}

static void page_related(FmRect r) {
  if (S.t_rel || (!S.rel.n && !S.links.n)) {
    message(r, S.t_rel ? "Finding related pages\xE2\x80\xA6" : S.rel_error[0] ? S.rel_error : "Nothing found", S.t_rel != NULL);
    return;
  }
  FmScroll *sc = &S.scroll[WP_RELATED];
  float col = FM_MIN(r.w - DP(16), DP(900)), x = r.x + (r.w - col) * 0.5f;
  float rh = DP(ui.touch_mode ? 72 : 64), lh = DP(ui.touch_mode ? 44 : 36), head = font_line_h(ui.m.font_title) + DP(16);
  int lcols = col >= DP(700) ? 3 : col >= DP(460) ? 2 : 1;
  float content = head + S.rel.n * rh + DP(20) + head + ((S.links.n + lcols - 1) / lcols) * lh + DP(40);
  ui_scroll(sc, ui_id("wk.rel"), r, content);
  gfx_clip_push(r);
  float y = r.y + DP(10) - sc->y;
  u32 base = ui_id("wk.rel.row");
  if (S.rel.n) {
    section_label(x, &y, "Related");
    for (int i = 0; i < S.rel.n; i++, y += rh) {
      FmRect c = { x, y, col, rh };
      if (gfx_visible(c) && row(ui_idn(base, (u32)i), c, S.rel.it[i].title, S.rel.it[i].desc, S.rel.it[i].thumb, IC_TEXT))
        article_start(S.art_src, S.art.lang, NULL, S.rel.it[i].title, true);
    }
    y += DP(20);
  }
  if (S.links.n) {
    char lab[64];
    fm_snprintf(lab, sizeof lab, "Linked from this page (%d)", S.links.n);
    section_label(x, &y, lab);
    float cw = (col - DP(8) * (lcols - 1)) / lcols;
    u32 lb = ui_id("wk.rel.link");
    for (int i = 0; i < S.links.n; i++) {
      FmRect c = { x + (i % lcols) * (cw + DP(8)), y + (i / lcols) * lh, cw, lh };
      if (!gfx_visible(c)) continue;
      int f = ui_hit(ui_idn(lb, (u32)i), c);
      if (f & UI_HOVER) { gfx_rrect(c, DP(8), T.surface2); ui_set_cursor(SDL_SYSTEM_CURSOR_HAND); }
      font_draw_ellipsis(FONT_REGULAR, ui.m.font, c.x + DP(8), c.y + (lh - font_line_h(ui.m.font)) * 0.5f, S.links.it[i].title,
                         cw - DP(16), T.accent);
      if (f & UI_CLICK) article_start(S.art_src, S.art.lang, NULL, S.links.it[i].title, true);
    }
  }
  gfx_clip_pop();
  ui_scrollbar(sc, r, content);
}

static void page_refs(FmRect r, int page) {
  bool saved = page == WP_SAVED;
  WkRef *v = saved ? S.saved : S.hist;
  int *n = saved ? &S.nsaved : &S.nhist;
  if (!*n) {
    message(r, saved ? "Nothing saved yet: tap the star on a page to keep it here (and on the home screen)."
                     : "Nothing read yet.", false);
    return;
  }
  FmScroll *sc = &S.scroll[page];
  float col = FM_MIN(r.w - DP(16), DP(900)), x = r.x + (r.w - col) * 0.5f, rh = DP(ui.touch_mode ? 72 : 64);
  float head = saved ? 0 : DP(ui.touch_mode ? 52 : 46);      /* history: its count and Clear all */
  float content = head + *n * rh + DP(70);
  ui_scroll(sc, ui_id(saved ? "wk.saved" : "wk.hist"), r, content);
  gfx_clip_push(r);
  u32 base = ui_id(saved ? "wk.saved.row" : "wk.hist.row");
  int remove = -1;
  bool clear_all = false;
  if (!saved) {
    /* Clear all asks twice: the first tap arms it for 3 s */
    static u64 armed_until;
    FmRect hr = { x, r.y + DP(8) - sc->y, col, head };
    char cnt[48];
    fm_snprintf(cnt, sizeof cnt, "%d %s read", *n, *n == 1 ? "page" : "pages");
    font_draw(FONT_REGULAR, ui.m.font_small, hr.x + DP(4), hr.y + (hr.h - font_line_h(ui.m.font_small)) * 0.5f, cnt, -1, T.text2);
    bool armed = ui.now < armed_until;
    const char *lab = armed ? "Tap again to clear" : "Clear all";
    float bw = font_width(FONT_BOLD, ui.m.font, lab, -1) + DP(52), bh = DP(ui.touch_mode ? 40 : 34);
    FmRect b = { hr.x + hr.w - bw, hr.y + (hr.h - bh) * 0.5f, bw, bh };
    if (ui_button(ui_id("wk.hist.clear"), b, IC_DELETE, lab, armed ? UI_BTN_DANGER : UI_BTN_TEXT)) {
      if (armed) { clear_all = true; armed_until = 0; }
      else armed_until = ui.now + 3000;
    }
    if (armed) view_wake_in((u32)(armed_until - ui.now) + 20);   /* back to "Clear all" when it runs out */
  }
  for (int i = 0; i < *n; i++) {
    FmRect c = { x, r.y + DP(8) + head - sc->y + i * rh, col, rh };
    if (!gfx_visible(c)) continue;
    FmRect del = { c.x + c.w - DP(44), c.y + (rh - DP(36)) * 0.5f, DP(36), DP(36) };
    FmRect body = c;
    body.w -= DP(48);
    int s = src_find(v[i].src);
    char sub[340];
    fm_snprintf(sub, sizeof sub, "%s%s%s", s >= 0 ? kSrc[s].name : "", v[i].desc[0] ? " \xC2\xB7 " : "", v[i].desc);
    if (row(ui_idn(base, (u32)i * 2), body, v[i].title, sub, v[i].thumb, s >= 0 ? kSrc[s].icon : IC_TEXT))
      article_start(s < 0 ? 0 : s, v[i].lang, v[i].id, v[i].title, true);
    if (ui_icon_btn(ui_idn(base, (u32)i * 2 + 1), del, IC_CLOSE, T.text2, saved ? "Remove from Saved" : "Remove from History"))
      remove = i;
  }
  gfx_clip_pop();
  ui_scrollbar(sc, r, content);
  if (clear_all) {
    *n = 0;
    S.scroll[page].y = 0;
    save_file();
    ui_toast("History cleared");
  } else if (remove >= 0) {
    ref_remove(v, n, remove);
    save_file();
  }
}

/* The contact for Wikimedia's rate limits. */
static void contact_card(float x, float *y, float col, bool page) {
  static const char kWhy[] =
    "Wikipedia lets an app it cannot identify make only 10 requests a minute, pictures included, so pages may "
    "stay half loaded. Give an email address or a web page of yours: it is sent only to Wikimedia, in each "
    "request's Api-User-Agent header (their policy), and then 200 requests a minute are allowed.";
  float th = font_draw_wrap(FONT_REGULAR, ui.m.font_small, 0, 0, col - DP(28), kWhy, T.text2, false);
  float fh = DP(ui.touch_mode ? 44 : 38);
  FmRect c = { x, *y, col, DP(14) + font_line_h(ui.m.font) + DP(6) + th + DP(10) + fh + DP(14) };
  gfx_rrect(c, DP(14), T.surface);
  gfx_rrect_line(c, DP(14), DP(1), page ? T.border : T.accent);
  float cy = c.y + DP(14);
  font_draw(FONT_BOLD, ui.m.font, c.x + DP(14), cy, S.contact[0] ? "Your contact for Wikimedia" : "Add your contact for Wikimedia",
            -1, T.text);
  cy += font_line_h(ui.m.font) + DP(6);
  font_draw_wrap(FONT_REGULAR, ui.m.font_small, c.x + DP(14), cy, col - DP(28), kWhy, T.text2, true);
  cy += th + DP(10);
  float bw = DP(96);
  FmRect tf = { c.x + DP(14), cy, col - DP(28) - bw - DP(8), fh };
  gfx_rrect(tf, DP(8), T.surface2);
  int res = ui_textfield(ui_id("wk.contact"), rect_inset2(tf, DP(8), DP(2)), S.contact_field, sizeof S.contact_field,
                         "you@example.com or https://your.site", 0);
  bool save = ui_button(ui_id("wk.contact.save"), FM_RECT(tf.x + tf.w + DP(8), cy, bw, fh), IC_CHECK, "Save", UI_BTN_FILLED);
  if (save || (res & UI_TF_SUBMIT)) {
    char *t = S.contact_field;
    while (*t == ' ') t++;
    fm_strlcpy(S.contact, t, sizeof S.contact);
    wk_set_contact(S.contact);
    save_file();
    ui_focus(0);
    ui_toast(S.contact[0] ? "Saved: Wikipedia now allows 200 requests a minute" : "Contact removed");
    if (page) go_back();
  }
  *y += c.h + DP(20);
}

static void page_contact(FmRect r) {
  float col = FM_MIN(r.w - DP(24), DP(700));
  float y = r.y + DP(16);
  contact_card(r.x + (r.w - col) * 0.5f, &y, col, true);
}

static void page_picklang(FmRect r) {
  FmScroll *sc = &S.scroll[WP_PICKLANG];
  float col = FM_MIN(r.w - DP(16), DP(600)), x = r.x + (r.w - col) * 0.5f, rh = DP(ui.touch_mode ? 52 : 44);
  float head = font_draw_wrap(FONT_REGULAR, ui.m.font_small, 0, 0, col, "For the wikis, the free books (books in that language) "
                              "and plants and animals (their local names). The dictionary is English.", T.text2, false) + DP(16);
  float content = head + FM_COUNT(kLangs) * rh + DP(40);
  ui_scroll(sc, ui_id("wk.pick"), r, content);
  gfx_clip_push(r);
  float y0 = r.y + DP(8) - sc->y;
  font_draw_wrap(FONT_REGULAR, ui.m.font_small, x, y0, col, "For the wikis, the free books (books in that language) "
                 "and plants and animals (their local names). The dictionary is English.", T.text2, true);
  u32 base = ui_id("wk.pick.row");
  for (int i = 0; i < FM_COUNT(kLangs); i++) {
    FmRect c = { x, y0 + head + i * rh, col, rh };
    if (!gfx_visible(c)) continue;
    bool on = !strcmp(kLangs[i].code, S.lang);
    if (ui_radio(ui_idn(base, (u32)i), c, kLangs[i].name, on) && !on) {
      fm_strlcpy(S.lang, kLangs[i].code, sizeof S.lang);
      save_file();
      wk_list_free(&S.res);
      S.res_error[0] = 0;
      go(WP_HOME);
    }
  }
  gfx_clip_pop();
  ui_scrollbar(sc, r, content);
}

/* ---- the view ------------------------------------------------------------------------------- */

bool wiki_is_open(void) { return S.open; }

void wiki_open(void) {
  load_file();
  if (!g_menu_id) g_menu_id = ui_id("wk.menu");
  S.open = true;
  if (S.page == WP_HOME && !ui.touch_mode) S.focus_search = true;
  ui_redraw();
}

void wiki_open_article(const char *lang, const char *title) {
  wiki_open();
  S.focus_search = false;
  /* a book or a species is opened by its number */
  article_start(S.src, lang && lang[0] ? lang : S.lang, cur()->kind == SK_WIKI ? NULL : title, title, S.art_ok);
}

bool wiki_open_saved(const char *key) {
  char src[16], lang[WK_LANG], id[64], title[256];
  wiki_open();
  if (!key_split(key, src, lang, id, title)) return false;
  S.focus_search = false;
  article_start(src_find(src), lang, id, title, S.art_ok);
  return true;
}

void wiki_close(void) {
  S.open = false;
  ui_focus(0);
  ui_redraw();
}

void wiki_frame(FmRect area) {
  if (!S.open) return;
  menu_results();
  theme_draw_bg(area);
  bool narrow = area.w / ui.scale < 760;
  FmRect r = area;
  FmRect top = rect_cut_top(&r, ui.m.bar_h);
  if (video_mini_active()) {
    FmRect mini = rect_cut_bottom(&r, DP(ui.touch_mode ? 64 : 56));
    video_mini_draw(rect_inset2(mini, DP(8), DP(4)));
  } else if (audio_mini_active()) {
    FmRect mini = rect_cut_bottom(&r, DP(ui.touch_mode ? 64 : 56));
    audio_mini_draw(rect_inset2(mini, DP(8), DP(4)));
  }
  if (S.page == WP_HOME || S.page == WP_RESULTS) {
    float col = FM_MIN(r.w - DP(24), DP(860));
    FmRect sf = { r.x + (r.w - col) * 0.5f, r.y + DP(10), col, DP(ui.touch_mode ? 46 : 40) };
    search_field(sf);
    rect_cut_top(&r, sf.h + DP(14));
  }
  switch (S.page) {
    case WP_HOME: page_home(r); break;
    case WP_RESULTS: page_results(r); break;
    case WP_ARTICLE: page_article(r); break;
    case WP_MEDIA: page_media(r); break;
    case WP_LANGS: page_langs(r); break;
    case WP_RELATED: page_related(r); break;
    case WP_TOC: page_toc(r); break;
    case WP_SAVED: case WP_HISTORY: page_refs(r, S.page); break;
    case WP_PICKLANG: page_picklang(r); break;
    case WP_CONTACT: page_contact(r); break;
    default: break;
  }
  if (!S.open) return;
  draw_top(top, narrow);
  if (!S.open) return;
  WkTask *dl = S.t_play ? S.t_play : S.t_image;
  if (dl) {                                            /* a file on its way to a viewer */
    float w = FM_MIN(area.w - DP(32), DP(420)), h = DP(44);
    FmRect c = { area.x + (area.w - w) * 0.5f, area.y + area.h - h - DP(84), w, h };
    gfx_shadow(c, DP(12), DP(12), col_alpha(T.shadow, 0.8f));
    gfx_rrect(c, DP(12), T.surface2);
    int pm = SDL_AtomicGet(&dl->permille);
    char s[96];
    if (pm > 0) fm_snprintf(s, sizeof s, "Downloading\xE2\x80\xA6 %d%%", pm / 10);
    else fm_strlcpy(s, "Finding the file\xE2\x80\xA6", sizeof s);
    font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, c.x + DP(14), c.y + DP(8), s, c.w - DP(28), T.text);
    FmRect bar = { c.x + DP(14), c.y + c.h - DP(12), c.w - DP(28), DP(4) };
    gfx_rrect(bar, DP(2), T.surface3);
    bar.w *= FM_CLAMP(pm / 1000.0f, 0.0f, 1.0f);
    if (bar.w > 0) gfx_rrect(bar, DP(2), T.accent);
  }
  /* the download card needs no polling: tasks wake the loop on progress and when done */
  if (ui_input_ok() && !ui_menu_is_open()) {
    if (ui_key(SDLK_ESCAPE, 0) || ui_key(SDLK_AC_BACK, 0) || ui_key(SDLK_BACKSPACE, KMOD_ALT)) {
      if (S.find_open && S.page == WP_ARTICLE) find_close();
      else if (ui.focus) ui_focus(0);
      else go_back();
    }
    if (ui_key(SDLK_f, KMOD_CTRL) && S.page == WP_ARTICLE && S.art_ok) {
      S.find_open = S.find_focus = true;
      S.find_ran[0] = 1;
    } else if (ui_key(SDLK_f, KMOD_CTRL) || ui_key(SDLK_l, KMOD_CTRL)) {
      S.focus_search = true;
      go(WP_HOME);
    }
  }
}

void wiki_demo_page(const char *page) {
  load_file();
  if (page && !strncmp(page, "find:", 5)) {            /* --demo-wiki-page find:WORD */
    S.find_open = true;
    fm_strlcpy(S.find_q, page + 5, sizeof S.find_q);
    S.find_ran[0] = 1;
    S.demo_page = -1;
    return;
  }
  if (page && (!strcmp(page, "history") || !strcmp(page, "saved"))) {   /* the lists, at once */
    S.back_page = WP_HOME;
    S.page = !strcmp(page, "history") ? WP_HISTORY : WP_SAVED;
    S.demo_page = -1;
    return;
  }
  S.demo_page = !page ? -1 : !strcmp(page, "media") ? WP_MEDIA : !strcmp(page, "toc") ? WP_TOC
              : !strcmp(page, "langs") ? WP_LANGS : !strcmp(page, "related") ? WP_RELATED : -1;
}

void wiki_preload(void) { load_file(); }

void wiki_demo_source(const char *key) {
  load_file();
  int i = key ? src_find(key) : -1;
  if (i >= 0) S.src = i;
}

void wiki_shutdown(void) {
  task_drop(&S.t_feed);
  task_drop(&S.t_browse);
  task_drop(&S.t_search);
  task_drop(&S.t_art);
  task_drop(&S.t_media);
  task_drop(&S.t_langs);
  task_drop(&S.t_rel);
  task_drop(&S.t_links);
  task_drop(&S.t_play);
  task_drop(&S.t_image);
  task_drop(&S.t_random);
  for (int i = 0; i < FM_COUNT(g_dropped); i++)
    if (g_dropped[i]) { task_free(g_dropped[i]); g_dropped[i] = NULL; }
  wk_feed_free(&S.feed);
  wk_list_free(&S.browse);
  wk_list_free(&S.res);
  wk_article_free(&S.art);
  wk_media_free(&S.media);
  wk_langs_free(&S.langs);
  wk_list_free(&S.rel);
  wk_list_free(&S.links);
  fm_free(S.bh);
  fm_free(S.find_blk);
  fm_free(S.saved);
  fm_free(S.hist);
  memset(&S, 0, sizeof S);
}
