/* fwiki.c -- Wikipedia: the view (see fwiki.h; the API calls are fwiki_api.c).
**
** Design decisions:
**   - One full-window view, drawn like the other online views, with pages:
**     home (search, today's featured content), results, an article, its
**     media, its languages, related articles and links, contents, saved
**     articles and history. Back walks the pages, then the articles read.
**   - Every request runs on its own thread (WkTask), polled each frame; one
**     that is no longer wanted is cancelled and joined later (reap), as the
**     photo and video views do.
**   - Articles are drawn as wrapped text: block heights are measured once per
**     width and text size, then only the blocks on screen are drawn.
**   - Pictures open in the app's image viewer (downloaded at screen size into
**     the cache), videos in the video player (a VP9/Opus WebM: the built-in
**     decoder plays it everywhere), sounds in the music player (MP3).
**   - Videos and sounds are downloaded whole (one request, with progress)
**     and played from the cache: upload.wikimedia.org answers 429 to the
**     many range requests streaming makes from an app (checked 2026-10-10).
**   - Language, text size, saved articles and history live in
**     PLACE_CONFIG/wiki.txt (one tab-separated line each); no settings keys.
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

/* ---- requests ------------------------------------------------------------------- */

enum { WT_FEED, WT_SEARCH, WT_ARTICLE, WT_MEDIA, WT_PLAY, WT_IMAGE, WT_LANGS, WT_RELATED, WT_LINKS, WT_RANDOM };

typedef struct WkTask {
  int kind;
  SDL_Thread *thr;
  SDL_atomic_t done;
  volatile int cancel;
  char lang[WK_LANG];
  char arg[512];              /* query, title or file */
  int num;                    /* search offset, media type */
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
  const char *dot = strrchr(url, '.');
  char ext[8] = "jpg";
  if (dot && strlen(dot) <= 5 && !strpbrk(dot, "/?&")) fm_strlcpy(ext, dot + 1, sizeof ext);
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

static int task_main(void *u) {
  WkTask *t = (WkTask *)u;
  switch (t->kind) {
    case WT_FEED: t->err = wk_feed(t->lang, &t->feed, &t->cancel); fm_strlcpy(t->error, t->feed.error, sizeof t->error); break;
    case WT_SEARCH: t->err = wk_search(t->lang, t->arg, t->num, &t->list, &t->cancel); fm_strlcpy(t->error, t->list.error, sizeof t->error); break;
    case WT_ARTICLE: t->err = wk_article(t->lang, t->arg, &t->art, &t->cancel); fm_strlcpy(t->error, t->art.error, sizeof t->error); break;
    case WT_MEDIA: t->err = wk_media(t->lang, t->arg, &t->media, &t->cancel); fm_strlcpy(t->error, t->media.error, sizeof t->error); break;
    case WT_LANGS: t->err = wk_langs(t->lang, t->arg, &t->langs, &t->cancel); fm_strlcpy(t->error, t->langs.error, sizeof t->error); break;
    case WT_RELATED: t->err = wk_related(t->lang, t->arg, &t->list, &t->cancel); fm_strlcpy(t->error, t->list.error, sizeof t->error); break;
    case WT_LINKS: t->err = wk_links(t->lang, t->arg, &t->list, &t->cancel); fm_strlcpy(t->error, t->list.error, sizeof t->error); break;
    case WT_RANDOM: t->err = wk_random(t->lang, t->title, sizeof t->title, &t->cancel); break;
    case WT_PLAY:
    case WT_IMAGE:
      t->err = wk_file(t->lang, t->arg, t->num, &t->file, &t->cancel);
      fm_strlcpy(t->error, t->file.error, sizeof t->error);
      if (t->err == FM_OK) {                         /* picture, video, sound: into the cache */
        if (!cache_path(t->file.url, t->path, sizeof t->path)) { t->err = FM_ERR_IO; break; }
        if (!plat_exists(t->path)) {
          FmNetResp r;
          memset(&r, 0, sizeof r);
          t->err = net_download(t->file.url, wk_headers(), t->path, dl_progress, t, &r, &t->cancel);
          if (t->err == FM_OK && r.status != 200) {
            t->err = FM_ERR_IO;
            fm_snprintf(t->error, sizeof t->error, "Wikimedia answered %d", r.status);
            plat_remove_file(t->path);
          }
          net_resp_free(&r);
        }
      }
      break;
    default: break;
  }
  if (t->err != FM_OK && !t->error[0]) fm_strlcpy(t->error, fm_err_str(t->err), sizeof t->error);
  SDL_AtomicSet(&t->done, 1);
  app_wake();
  return 0;
}

static WkTask *task_start(int kind, const char *lang, const char *arg, int num) {
  WkTask *t = (WkTask *)fm_calloc(1, sizeof *t);
  t->kind = kind;
  fm_strlcpy(t->lang, lang, sizeof t->lang);
  fm_strlcpy(t->arg, arg ? arg : "", sizeof t->arg);
  t->num = num;
  t->thr = fm_thread_create(task_main, "wiki", t);
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
       WP_CONTACT };

typedef struct WkRef { char lang[WK_LANG]; char title[256]; char desc[256]; char thumb[512]; } WkRef;
#define WK_SAVED_MAX 500
#define WK_HIST_MAX  60
#define WK_STACK     32

static struct {
  bool open, loaded;
  int page;
  char lang[WK_LANG];
  float zoom;                  /* text size, 0.8 .. 1.6 */
  char field[256];
  bool focus_search;
  FmScroll scroll[WP_CONTACT + 1];
  /* home */
  WkTask *t_feed;
  WkFeed feed;
  bool feed_ok;
  char feed_lang[WK_LANG];
  char feed_error[256];
  /* results */
  WkTask *t_search;
  WkList res;
  char res_query[256];
  char res_error[256];
  /* the article */
  WkTask *t_art;
  WkArticle art;
  bool art_ok;
  char art_error[256];
  char want_lang[WK_LANG], want_title[256];
  int demo_page;               /* --demo-wiki-page: shown once the article is in, -1 = none */
  float *bh;                   /* block heights at lay_w / lay_zoom */
  float lay_w, lay_zoom, lay_total, lay_top;
  float restore_y;             /* back to an article: where it was read */
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
  struct { char lang[WK_LANG]; char title[256]; float y; } stack[WK_STACK];
  int nstack;
  int back_page;               /* a sub-page returns here */
  char contact[300];           /* for Wikimedia's rate limits (Api-User-Agent) */
  char contact_field[300];
  /* lists */
  WkRef *saved, *hist;
  int nsaved, nhist;
} S;

static void save_file(void);
static void contact_card(float x, float *y, float col, bool page);
static void go_back(void);

/* ---- wiki.txt ------------------------------------------------------------------------- */

static bool conf_path(char *out, size_t cap) {
  char dir[FM_PATH_MAX];
  return plat_place(PLACE_CONFIG, dir, sizeof dir) && fm_path_join(out, cap, dir, "wiki.txt");
}

static void clean_field(char *s) {
  for (; *s; s++)
    if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
}

static void ref_set(WkRef *r, const char *lang, const char *title, const char *desc, const char *thumb) {
  memset(r, 0, sizeof *r);
  fm_strlcpy(r->lang, lang, sizeof r->lang);
  fm_strlcpy(r->title, title, sizeof r->title);
  fm_strlcpy(r->desc, desc ? desc : "", sizeof r->desc);
  fm_strlcpy(r->thumb, thumb ? thumb : "", sizeof r->thumb);
  clean_field(r->title);
  clean_field(r->desc);
  clean_field(r->thumb);
}

static void load_file(void) {
  if (S.loaded) return;
  S.loaded = true;
  fm_strlcpy(S.lang, "en", sizeof S.lang);
  S.zoom = 1.0f;
  S.saved = (WkRef *)fm_calloc(WK_SAVED_MAX, sizeof *S.saved);
  S.hist = (WkRef *)fm_calloc(WK_HIST_MAX, sizeof *S.hist);
  char path[FM_PATH_MAX];
  FILE *f = conf_path(path, sizeof path) ? fm_fopen(path, "rb") : NULL;
  if (!f) return;
  char line[1600];
  while (fgets(line, sizeof line, f)) {
    line[strcspn(line, "\r\n")] = 0;
    char *fld[5] = { 0 };
    int n = 0;
    for (char *p = line; p && n < 5;) {
      fld[n++] = p;
      char *tab = strchr(p, '\t');
      if (tab) *tab = 0;
      p = tab ? tab + 1 : NULL;
    }
    if (n >= 2 && !strcmp(fld[0], "L") && wk_lang_ok(fld[1])) fm_strlcpy(S.lang, fld[1], sizeof S.lang);
    else if (n >= 2 && !strcmp(fld[0], "Z")) S.zoom = FM_CLAMP((float)atof(fld[1]), 0.8f, 1.6f);
    else if (n >= 2 && !strcmp(fld[0], "C")) fm_strlcpy(S.contact, fld[1], sizeof S.contact);
    else if (n >= 3 && (!strcmp(fld[0], "S") || !strcmp(fld[0], "H")) && wk_lang_ok(fld[1]) && fld[2][0]) {
      bool sv = fld[0][0] == 'S';
      if (sv && S.nsaved < WK_SAVED_MAX) ref_set(&S.saved[S.nsaved++], fld[1], fld[2], n > 3 ? fld[3] : "", n > 4 ? fld[4] : "");
      if (!sv && S.nhist < WK_HIST_MAX) ref_set(&S.hist[S.nhist++], fld[1], fld[2], n > 3 ? fld[3] : "", n > 4 ? fld[4] : "");
    }
  }
  fclose(f);
  wk_set_contact(S.contact);
  fm_strlcpy(S.contact_field, S.contact, sizeof S.contact_field);
}

static void save_file(void) {
  char path[FM_PATH_MAX], tmp[FM_PATH_MAX];
  if (!conf_path(path, sizeof path)) return;
  fm_snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *f = fm_fopen(tmp, "wb");
  if (!f) return;
  fprintf(f, "# mmcfm Wikipedia: L language, Z text size, S saved, H history (newest first)\n");
  fprintf(f, "L\t%s\nZ\t%.2f\n", S.lang, S.zoom);
  if (S.contact[0]) fprintf(f, "C\t%s\n", S.contact);
  for (int i = 0; i < S.nsaved; i++)
    fprintf(f, "S\t%s\t%s\t%s\t%s\n", S.saved[i].lang, S.saved[i].title, S.saved[i].desc, S.saved[i].thumb);
  for (int i = 0; i < S.nhist; i++)
    fprintf(f, "H\t%s\t%s\t%s\t%s\n", S.hist[i].lang, S.hist[i].title, S.hist[i].desc, S.hist[i].thumb);
  if (fclose(f) == 0) {
    plat_remove_file(path);
    plat_rename(tmp, path);
  }
}

static int ref_find(const WkRef *v, int n, const char *lang, const char *title) {
  for (int i = 0; i < n; i++)
    if (!strcmp(v[i].lang, lang) && !strcmp(v[i].title, title)) return i;
  return -1;
}

/* Puts (lang, title) first in the list v (moving it when there). */
static void ref_front(WkRef *v, int *n, int max, const char *lang, const char *title, const char *desc,
                      const char *thumb) {
  int i = ref_find(v, *n, lang, title);
  if (i < 0) i = *n < max ? (*n)++ : max - 1;
  memmove(&v[1], &v[0], (size_t)i * sizeof *v);
  ref_set(&v[0], lang, title, desc, thumb);
}

static void ref_remove(WkRef *v, int *n, int i) {
  if (i < 0 || i >= *n) return;
  memmove(&v[i], &v[i + 1], (size_t)(*n - i - 1) * sizeof *v);
  (*n)--;
}

int wiki_saved_count(void) {
  load_file();
  return S.nsaved;
}

bool wiki_saved_get(int i, char *lang, size_t lcap, char *title, size_t tcap, char *desc, size_t dcap, char *thumb,
                    size_t thcap) {
  load_file();
  if (i < 0 || i >= S.nsaved) return false;
  fm_strlcpy(lang, S.saved[i].lang, lcap);
  fm_strlcpy(title, S.saved[i].title, tcap);
  fm_strlcpy(desc, S.saved[i].desc, dcap);
  fm_strlcpy(thumb, S.saved[i].thumb, thcap);
  return true;
}

bool wiki_saved_has(const char *lang, const char *title) {
  load_file();
  return ref_find(S.saved, S.nsaved, lang, title) >= 0;
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

static void go(int page) {
  if (page != S.page && (page == WP_MEDIA || page == WP_LANGS || page == WP_RELATED || page == WP_TOC ||
                         page == WP_SAVED || page == WP_HISTORY || page == WP_PICKLANG || page == WP_CONTACT))
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
  S.t_feed = task_start(WT_FEED, S.lang, NULL, 0);
}

static void search_start(const char *q, bool more) {
  if (!q || !q[0]) return;
  if (!more) {
    task_drop(&S.t_search);
    wk_list_free(&S.res);
    S.res.next = -1;
    S.res_error[0] = 0;
    fm_strlcpy(S.res_query, q, sizeof S.res_query);
    S.scroll[WP_RESULTS].y = 0;
    go(WP_RESULTS);
  } else if (S.t_search || S.res.next < 0) {
    return;
  }
  S.t_search = task_start(WT_SEARCH, S.lang, S.res_query, more ? S.res.next : 0);
}

/* Opens an article; push: the one on screen goes on the Back stack. */
static void article_start(const char *lang, const char *title, bool push) {
  if (!title || !title[0]) return;
  if (push && S.art_ok && S.nstack < WK_STACK) {
    fm_strlcpy(S.stack[S.nstack].lang, S.art.lang, sizeof S.stack[0].lang);
    fm_strlcpy(S.stack[S.nstack].title, S.art.title, sizeof S.stack[0].title);
    S.stack[S.nstack++].y = S.scroll[WP_ARTICLE].y;
  }
  task_drop(&S.t_art);
  task_drop(&S.t_media);
  task_drop(&S.t_langs);
  task_drop(&S.t_rel);
  task_drop(&S.t_links);
  fm_strlcpy(S.want_lang, lang && lang[0] ? lang : S.lang, sizeof S.want_lang);
  fm_strlcpy(S.want_title, title, sizeof S.want_title);
  S.art_error[0] = 0;
  S.t_art = task_start(WT_ARTICLE, S.want_lang, title, 0);
  S.page = WP_ARTICLE;
  ui_focus(0);
  ui_redraw();
}

static void random_start(void) {
  if (S.t_random) return;
  S.t_random = task_start(WT_RANDOM, S.lang, NULL, 0);
  ui_toast("Finding a random article\xE2\x80\xA6");
}

static void sub_start(int page) {
  if (!S.art_ok) return;
  if (page == WP_MEDIA && !S.media_for && !S.t_media) {
    S.media_error[0] = 0;
    S.t_media = task_start(WT_MEDIA, S.art.lang, S.art.title, 0);
  }
  if (page == WP_LANGS && !S.langs_for && !S.t_langs) {
    S.langs_error[0] = 0;
    S.t_langs = task_start(WT_LANGS, S.art.lang, S.art.title, 0);
  }
  if (page == WP_RELATED && !S.rel_for && !S.t_rel) {
    S.rel_error[0] = 0;
    S.t_rel = task_start(WT_RELATED, S.art.lang, S.art.title, 0);
    S.t_links = task_start(WT_LINKS, S.art.lang, S.art.title, 0);
  }
  S.scroll[page].y = 0;
  go(page);
}

static void media_open(const WkMedia *m) {
  if (m->type == WM_IMAGE) {
    if (S.t_image) return;
    S.t_image = task_start(WT_IMAGE, S.art.lang, m->file, WM_IMAGE);
    ui_toast("Opening the picture\xE2\x80\xA6");
  } else {
    if (S.t_play) return;
    S.t_play = task_start(WT_PLAY, S.art.lang, m->file, m->type);
    ui_toast(m->type == WM_VIDEO ? "Opening the video\xE2\x80\xA6" : "Opening the sound\xE2\x80\xA6");
  }
}

static void open_web(const char *url) {
  if (!url || !url[0]) return;
  if (SDL_OpenURL(url) != 0 && !plat_open_external(url)) ui_toast("No browser found");
}

static void toggle_saved(void) {
  if (!S.art_ok) return;
  int i = ref_find(S.saved, S.nsaved, S.art.lang, S.art.title);
  if (i >= 0) {
    ref_remove(S.saved, &S.nsaved, i);
    ui_toast("Removed from Saved articles");
  } else {
    ref_front(S.saved, &S.nsaved, WK_SAVED_MAX, S.art.lang, S.art.title, S.art.desc, S.art.lead);
    ui_toast("Saved (also on the home screen's Favorites)");
  }
  save_file();
}

static void go_back(void) {
  switch (S.page) {
    case WP_MEDIA: case WP_LANGS: case WP_RELATED: case WP_TOC: case WP_SAVED: case WP_HISTORY: case WP_PICKLANG:
    case WP_CONTACT:
      go(S.back_page == WP_ARTICLE && !S.art_ok && !S.t_art ? WP_HOME : S.back_page);
      return;
    case WP_ARTICLE:
      if (S.nstack > 0) {
        S.nstack--;
        S.restore_y = S.stack[S.nstack].y;
        article_start(S.stack[S.nstack].lang, S.stack[S.nstack].title, false);
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
  if (task_done(S.t_search)) {
    WkTask *t = S.t_search;
    S.t_search = NULL;
    if (t->err == FM_OK) {
      for (int i = 0; i < t->list.n; i++) {
        if (S.res.n == S.res.cap) {
          S.res.cap = S.res.cap ? S.res.cap * 2 : 32;
          S.res.it = (WkPage *)fm_realloc(S.res.it, (size_t)S.res.cap * sizeof *S.res.it);
        }
        S.res.it[S.res.n++] = t->list.it[i];
      }
      S.res.next = t->list.next;
      if (!S.res.n) fm_strlcpy(S.res_error, t->error[0] ? t->error : "No articles found", sizeof S.res_error);
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
      layout_reset();
      wk_media_free(&S.media);
      wk_langs_free(&S.langs);
      wk_list_free(&S.rel);
      wk_list_free(&S.links);
      S.media_for = S.langs_for = S.rel_for = false;
      S.scroll[WP_ARTICLE].y = S.restore_y;
      S.restore_y = 0;
      if (S.demo_page > 0) {
        int p = S.demo_page;
        S.demo_page = -1;
        if (p == WP_TOC) go(WP_TOC);
        else sub_start(p);
      }
      ref_front(S.hist, &S.nhist, WK_HIST_MAX, S.art.lang, S.art.title, S.art.desc, S.art.lead);
      save_file();
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
    if (t->err == FM_OK) article_start(t->lang, t->title, true);
    else ui_toast("No random article: %s", t->error);
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
      if (dot) *dot = 0;
      if (t->num == WM_VIDEO) {
        video_open_stream(title, t->path, NULL);
      } else {
        FmAudioEntry e;
        memset(&e, 0, sizeof e);
        e.url = t->path;
        e.file = true;
        e.title = title;
        e.artist = "Wikipedia";
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
    SDL_Texture *tex = othumb_get_ex(url, (int)FM_MAX(r.w, r.h) * 2, aspect, wk_headers(), &fade);
    if (tex) {
      gfx_tex_rounded(tex, r, rad, FM_RGBA(255, 255, 255, (u8)(255 * fade)));
      if (fade < 1) ui_animate();
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
    ui_animate();
    y += DP(40);
  }
  if (s && s[0]) {
    float w = FM_MIN(r.w - DP(32), DP(520));
    font_draw_wrap(FONT_REGULAR, ui.m.font, r.x + (r.w - w) * 0.5f, y, w, s, T.text2, true);
  }
}

/* ---- the top bar ---------------------------------------------------------------------------- */

enum { MI_RANDOM = 1, MI_SAVED, MI_HISTORY, MI_LANG, MI_BIGGER, MI_SMALLER, MI_WEB, MI_COPY, MI_RELATED, MI_LANGS,
       MI_CONTACT };
static u32 g_menu_id;

static void more_menu(float x, float y) {
  FmMenuItem m[12];
  int n = 0;
  static char lang_label[80];
  fm_snprintf(lang_label, sizeof lang_label, "Language: %s", lang_name(S.lang));
  m[n++] = (FmMenuItem){ MI_RANDOM, IC_SHUFFLE, "Random article", NULL, 0 };
  m[n++] = (FmMenuItem){ MI_SAVED, IC_STAR, "Saved articles", NULL, 0 };
  m[n++] = (FmMenuItem){ MI_HISTORY, IC_HISTORY, "History", NULL, 0 };
  m[n++] = (FmMenuItem){ MI_LANG, IC_NETWORK, lang_label, NULL, 0 };
  m[n++] = (FmMenuItem){ MI_CONTACT, IC_PERSON, "Wikimedia contact", NULL, 0 };
  if (S.page == WP_ARTICLE && S.art_ok) {
    m[n++] = (FmMenuItem){ MI_RELATED, IC_LINK, "Related articles and links", NULL, 0 };
    m[n++] = (FmMenuItem){ MI_LANGS, IC_NETWORK, "This article in other languages", NULL, 0 };
    m[n++] = (FmMenuItem){ MI_BIGGER, IC_ZOOM_IN, "Bigger text", NULL, 0 };
    m[n++] = (FmMenuItem){ MI_SMALLER, IC_ZOOM_OUT, "Smaller text", NULL, 0 };
    m[n++] = (FmMenuItem){ MI_WEB, IC_OPEN_WITH, "Open in browser", NULL, 0 };
    m[n++] = (FmMenuItem){ MI_COPY, IC_COPY, "Copy link", NULL, 0 };
  }
  ui_menu_open(g_menu_id, x, y, m, n);
}

static void menu_results(void) {
  int r = ui_menu_result(g_menu_id);
  if (r <= 0) return;
  switch (r) {
    case MI_RANDOM: random_start(); break;
    case MI_SAVED: S.scroll[WP_SAVED].y = 0; go(WP_SAVED); break;
    case MI_HISTORY: S.scroll[WP_HISTORY].y = 0; go(WP_HISTORY); break;
    case MI_LANG: S.scroll[WP_PICKLANG].y = 0; go(WP_PICKLANG); break;
    case MI_CONTACT: fm_strlcpy(S.contact_field, S.contact, sizeof S.contact_field); go(WP_CONTACT); break;
    case MI_RELATED: sub_start(WP_RELATED); break;
    case MI_LANGS: sub_start(WP_LANGS); break;
    case MI_BIGGER: S.zoom = FM_MIN(1.6f, S.zoom + 0.1f); layout_reset(); save_file(); break;
    case MI_SMALLER: S.zoom = FM_MAX(0.8f, S.zoom - 0.1f); layout_reset(); save_file(); break;
    case MI_WEB: open_web(S.art.url); break;
    case MI_COPY: ui_clipboard_set(S.art.url); ui_toast("Link copied"); break;
    default: break;
  }
}

static const char *page_name(void) {
  switch (S.page) {
    case WP_RESULTS: return "Search";
    case WP_ARTICLE: return S.art_ok && !S.t_art ? S.art.title : S.want_title;
    case WP_MEDIA: return "Pictures, videos and sounds";
    case WP_LANGS: return "Other languages";
    case WP_RELATED: return "Related articles and links";
    case WP_TOC: return "Contents";
    case WP_SAVED: return "Saved articles";
    case WP_HISTORY: return "History";
    case WP_PICKLANG: return "Wikipedia language";
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
  if (!narrow) {
    FmRect m = rect_center(rect_cut_left(&in, DP(34)), DP(28), DP(28));
    gfx_rrect(m, DP(8), FM_HEX(0xF4F4F2));
    font_draw_center(FONT_BOLD, ui.m.font_title, m, "W", FM_HEX(0x202122));
    rect_cut_left(&in, DP(8));
  }
  b = rect_center(rect_cut_right(&in, bs), bs, bs);
  title_nodrag(b);
  if (ui_icon_btn(ui_idn(base, 2), b, IC_MORE, T.text2, "More")) more_menu(b.x, b.y + b.h);
  bool art = S.page == WP_ARTICLE && S.art_ok && !S.t_art;
  if (art) {
    b = rect_center(rect_cut_right(&in, bs), bs, bs);
    title_nodrag(b);
    bool sv = ref_find(S.saved, S.nsaved, S.art.lang, S.art.title) >= 0;
    if (ui_icon_btn(ui_idn(base, 3), b, sv ? IC_STAR_FILL : IC_STAR, sv ? T.accent : T.text2,
                    sv ? "Saved: remove" : "Save this article"))
      toggle_saved();
    b = rect_center(rect_cut_right(&in, bs), bs, bs);
    title_nodrag(b);
    if (ui_icon_btn(ui_idn(base, 4), b, IC_IMAGE, T.text2, "Pictures, videos and sounds")) sub_start(WP_MEDIA);
    b = rect_center(rect_cut_right(&in, bs), bs, bs);
    title_nodrag(b);
    if (ui_icon_btn(ui_idn(base, 5), b, IC_LIST, T.text2, "Contents")) { S.scroll[WP_TOC].y = 0; go(WP_TOC); }
  }
  if (S.page != WP_HOME && S.page != WP_RESULTS) {
    b = rect_center(rect_cut_right(&in, bs), bs, bs);
    title_nodrag(b);
    if (ui_icon_btn(ui_idn(base, 6), b, IC_SEARCH, T.text2, "Search Wikipedia")) {
      S.focus_search = true;
      go(WP_HOME);
    }
  }
  char sub[160];
  fm_snprintf(sub, sizeof sub, "Wikipedia \xC2\xB7 %s", lang_name(art ? S.art.lang : S.lang));
  float fs = ui.m.font_title * (narrow ? 0.9f : 1.0f);
  float tl = font_line_h(fs), sl = font_line_h(ui.m.font_small);
  float y = in.y + (in.h - tl - sl) * 0.5f;
  font_draw_ellipsis(FONT_BOLD, fs, in.x + DP(2), y, page_name(), in.w - DP(4), T.text);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, in.x + DP(2), y + tl, sub, in.w - DP(4), T.text2);
}

/* The search field; true when it submitted. */
static void search_field(FmRect r) {
  u32 id = ui_id("wk.search");
  if (S.focus_search) { ui_focus(id); S.focus_search = false; }
  gfx_rrect(r, r.h * 0.5f, T.surface2);
  FmRect ic = rect_center(rect_cut_left(&r, r.h), DP(18), DP(18));
  icon_draw(IC_SEARCH, ic, T.text2);
  char hint[80];
  fm_snprintf(hint, sizeof hint, "Search Wikipedia (%s)", lang_name(S.lang));
  int res = ui_textfield(id, rect_inset2(r, 0, DP(2)), S.field, sizeof S.field, hint, 0);
  if (res & UI_TF_SUBMIT) search_start(S.field, false);
}

/* ---- pages ------------------------------------------------------------------------------------ */

static void page_home(FmRect r) {
  FmScroll *sc = &S.scroll[WP_HOME];
  feed_start();
  float col = FM_MIN(r.w - DP(24), DP(860));
  float x = r.x + (r.w - col) * 0.5f;
  /* measured each frame from what is drawn: the content ends at y */
  static float content = 1000;
  ui_scroll(sc, ui_id("wk.home"), r, content);
  gfx_clip_push(r);
  float y = r.y + DP(14) - sc->y;
  float top = y;
  /* quick actions */
  u32 base = ui_id("wk.home.btn");
  float bh = DP(ui.touch_mode ? 40 : 34), bx = x;
  struct { FmIcon ic; const char *label; int act; } kB[] = {
    { IC_SHUFFLE, "Random", 1 }, { IC_STAR, "Saved", 2 }, { IC_HISTORY, "History", 3 }, { IC_NETWORK, lang_name(S.lang), 4 },
  };
  for (int i = 0; i < FM_COUNT(kB); i++) {
    float w = font_width(FONT_REGULAR, ui.m.font_small, kB[i].label, -1) + DP(48);
    if (bx + w > x + col) { bx = x; y += bh + DP(8); }
    if (ui_button(ui_idn(base, (u32)i), FM_RECT(bx, y, w, bh), kB[i].ic, kB[i].label, UI_BTN_TONAL)) {
      if (kB[i].act == 1) random_start();
      else if (kB[i].act == 2) { S.scroll[WP_SAVED].y = 0; go(WP_SAVED); }
      else if (kB[i].act == 3) { S.scroll[WP_HISTORY].y = 0; go(WP_HISTORY); }
      else { S.scroll[WP_PICKLANG].y = 0; go(WP_PICKLANG); }
    }
    bx += w + DP(8);
  }
  y += bh + DP(20);
  if (!S.contact[0]) contact_card(x, &y, col, false);
  if (!S.feed_ok) {
    message(FM_RECT(x, y - DP(20), col, DP(140)), S.t_feed ? "Loading today's Wikipedia\xE2\x80\xA6" : S.feed_error,
            S.t_feed != NULL);
    y += DP(150);
  } else {
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
      font_draw_wrap(FONT_REGULAR, ui.m.font_small, tx, c.y + DP(14) + font_line_h(ui.m.font_title), tw,
                     f->tfa_extract, T.text2, true);
      gfx_clip_pop();
      if (fl & UI_CLICK) article_start(S.lang, f->tfa.title, true);
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
          article_start(S.lang, f->most.it[i].title, true);
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
        S.t_image = task_start(WT_IMAGE, S.lang, f->potd_title, WM_IMAGE);
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
        if (fl & UI_CLICK) article_start(S.lang, d->page.title, true);
        y += h;
      }
      y += DP(16);
    }
  }
  /* recently read */
  if (S.nhist) {
    section_label(x, &y, "Recently read");
    float rh = DP(64);
    int n = FM_MIN(S.nhist, 5);
    for (int i = 0; i < n; i++) {
      FmRect c = { x, y, col, rh };
      if (row(ui_idn(ui_id("wk.home.h"), (u32)i), c, S.hist[i].title, S.hist[i].desc, S.hist[i].thumb, IC_TEXT))
        article_start(S.hist[i].lang, S.hist[i].title, true);
      y += rh;
    }
    y += DP(16);
  }
  gfx_clip_pop();
  content = y + sc->y - top + DP(30);
  ui_scrollbar(sc, r, content);
}

static void list_page(FmRect r, int page, const WkPage *it, int n, const char *lang, u32 base, bool more_at_end) {
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
    if (row(ui_idn(base, (u32)i + 1), c, it[i].title, it[i].desc, it[i].thumb, IC_TEXT)) article_start(lang, it[i].title, true);
  }
  gfx_clip_pop();
  ui_scrollbar(sc, r, content);
  if (more_at_end && sc->y + r.h >= content - rh * 4) search_start(S.res_query, true);
}

static void page_results(FmRect r) {
  if (!S.res.n) {
    message(r, S.t_search ? "Searching\xE2\x80\xA6" : S.res_error, S.t_search != NULL);
    return;
  }
  list_page(r, WP_RESULTS, S.res.it, S.res.n, S.lang, ui_id("wk.res"), S.res.next >= 0);
  if (S.t_search) {                                   /* the next page is coming */
    ui_spinner(FM_RECT(r.x + r.w * 0.5f - DP(12), r.y + r.h - DP(36), DP(24), DP(24)), T.accent);
    ui_animate();
  }
}

/* Article layout: block heights at the current width and text size. */
static float block_font(int kind) {
  return kind == WB_H2 ? ui.m.font_title * 1.25f * S.zoom : kind == WB_H3 ? ui.m.font_title * 1.05f * S.zoom
       : kind == WB_H4 ? txt() * 1.05f : txt();
}

static void layout(float w) {
  if (S.bh && S.lay_w == w && S.lay_zoom == S.zoom) return;
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

static void page_article(FmRect r) {
  if (S.t_art || !S.art_ok) {
    message(r, S.t_art ? "Opening the article\xE2\x80\xA6" : S.art_error, S.t_art != NULL);
    return;
  }
  FmScroll *sc = &S.scroll[WP_ARTICLE];
  float col = FM_MIN(r.w - DP(32), DP(780));
  float x = r.x + (r.w - col) * 0.5f;
  layout(col);
  /* the head: lead picture, title, description */
  float lead_h = 0;
  if (S.art.lead[0]) {
    float ar = S.art.lead_w > 0 && S.art.lead_h > 0 ? (float)S.art.lead_w / (float)S.art.lead_h : 1.5f;
    lead_h = FM_MIN(col / ar, r.h * 0.5f);
  }
  float title_fs = ui.m.font_title * 1.6f * S.zoom;
  float title_h = font_draw_wrap(FONT_BOLD, title_fs, 0, 0, col, S.art.title, T.text, false);
  float desc_h = S.art.desc[0] ? font_draw_wrap(FONT_REGULAR, txt(), 0, 0, col, S.art.desc, T.text2, false) : 0;
  float head = DP(16) + (lead_h > 0 ? lead_h + DP(16) : 0) + title_h + DP(6) + desc_h + DP(18);
  float content = head + S.lay_total + DP(80);
  ui_scroll(sc, ui_id("wk.art"), r, content);
  gfx_clip_push(r);
  float y = r.y - sc->y + DP(16);
  if (lead_h > 0) {
    float ar = S.art.lead_w > 0 && S.art.lead_h > 0 ? (float)S.art.lead_w / (float)S.art.lead_h : 1.5f;
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
  for (int i = 0; i < S.art.nb; i++) {
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
  u32 base = ui_id("wk.art.end");
  float bh = DP(ui.touch_mode ? 40 : 34), bx = x;
  struct { FmIcon ic; const char *label; int page; } kB[] = {
    { IC_IMAGE, "Pictures, videos, sounds", WP_MEDIA }, { IC_LINK, "Related and links", WP_RELATED },
    { IC_NETWORK, "Other languages", WP_LANGS },
  };
  for (int i = 0; i < FM_COUNT(kB); i++) {
    float w = font_width(FONT_REGULAR, ui.m.font_small, kB[i].label, -1) + DP(48);
    if (bx + w > x + col) { bx = x; y += bh + DP(8); }
    if (ui_button(ui_idn(base, (u32)i), FM_RECT(bx, y, w, bh), kB[i].ic, kB[i].label, UI_BTN_TONAL)) sub_start(kB[i].page);
    bx += w + DP(8);
  }
  gfx_clip_pop();
  ui_scrollbar(sc, r, content);
  if (ui_input_ok() && !ui.focus && !ui_menu_is_open()) {
    if (ui_key(SDLK_EQUALS, KMOD_CTRL) || ui_key(SDLK_PLUS, KMOD_CTRL)) { S.zoom = FM_MIN(1.6f, S.zoom + 0.1f); save_file(); }
    if (ui_key(SDLK_MINUS, KMOD_CTRL)) { S.zoom = FM_MAX(0.8f, S.zoom - 0.1f); save_file(); }
    if (ui_key(SDLK_PAGEDOWN, 0) || ui_key(SDLK_SPACE, 0)) sc->y = FM_MIN(sc->y + r.h * 0.9f, FM_MAX(0, content - r.h));
    if (ui_key(SDLK_PAGEUP, 0)) sc->y = FM_MAX(0, sc->y - r.h * 0.9f);
    if (ui_key(SDLK_HOME, 0)) sc->y = 0;
  }
}

static void page_toc(FmRect r) {
  if (!S.art_ok) { go(WP_ARTICLE); return; }
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
    if (i >= 0) {
      float h = S.bh[i];
      if (S.art.b[i].kind == WB_PARA) { at += h; continue; }
    }
    const char *label = i < 0 ? "(Top)" : S.art.b[i].s;
    float indent = i < 0 ? 0 : S.art.b[i].kind == WB_H2 ? 0 : S.art.b[i].kind == WB_H3 ? DP(22) : DP(44);
    FmRect c = { x, y, col, rh };
    int f = ui_hit(ui_idn(base, (u32)(i + 1)), c);
    if (f & UI_HOVER) gfx_rrect(c, DP(8), T.surface2);
    font_draw_ellipsis(i < 0 || S.art.b[i].kind == WB_H2 ? FONT_BOLD : FONT_REGULAR, ui.m.font, x + DP(10) + indent,
                       y + (rh - font_line_h(ui.m.font)) * 0.5f, label, col - DP(20) - indent, T.text);
    if (f & UI_CLICK) {
      S.scroll[WP_ARTICLE].y = i < 0 ? 0 : S.lay_top + at - DP(8);
      S.scroll[WP_ARTICLE].vel = 0;
      go(WP_ARTICLE);
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
    const char *cap = m->caption[0] ? m->caption : m->file + 5;
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
    if (f & UI_CLICK) article_start(l->lang, l->title, true);
  }
  gfx_clip_pop();
  ui_scrollbar(sc, r, content);
}

static void page_related(FmRect r) {
  if (S.t_rel || (!S.rel.n && !S.links.n)) {
    message(r, S.t_rel ? "Finding related articles\xE2\x80\xA6" : S.rel_error[0] ? S.rel_error : "Nothing found",
            S.t_rel != NULL);
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
    section_label(x, &y, "Related articles");
    for (int i = 0; i < S.rel.n; i++, y += rh) {
      FmRect c = { x, y, col, rh };
      if (gfx_visible(c) && row(ui_idn(base, (u32)i), c, S.rel.it[i].title, S.rel.it[i].desc, S.rel.it[i].thumb, IC_TEXT))
        article_start(S.art.lang, S.rel.it[i].title, true);
    }
    y += DP(20);
  }
  if (S.links.n) {
    char lab[64];
    fm_snprintf(lab, sizeof lab, "Linked from this article (%d)", S.links.n);
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
      if (f & UI_CLICK) article_start(S.art.lang, S.links.it[i].title, true);
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
    message(r, saved ? "No saved articles yet: tap the star on an article to keep it here (and on the home screen)."
                     : "Nothing read yet.", false);
    return;
  }
  FmScroll *sc = &S.scroll[page];
  float col = FM_MIN(r.w - DP(16), DP(900)), x = r.x + (r.w - col) * 0.5f, rh = DP(ui.touch_mode ? 72 : 64);
  float content = *n * rh + DP(70);
  ui_scroll(sc, ui_id(saved ? "wk.saved" : "wk.hist"), r, content);
  gfx_clip_push(r);
  u32 base = ui_id(saved ? "wk.saved.row" : "wk.hist.row");
  int remove = -1;
  for (int i = 0; i < *n; i++) {
    FmRect c = { x, r.y + DP(8) - sc->y + i * rh, col, rh };
    if (!gfx_visible(c)) continue;
    FmRect del = { c.x + c.w - DP(44), c.y + (rh - DP(36)) * 0.5f, DP(36), DP(36) };
    FmRect body = c;
    body.w -= DP(48);
    char sub[300];
    fm_snprintf(sub, sizeof sub, "%s%s%s", lang_name(v[i].lang), v[i].desc[0] ? " \xC2\xB7 " : "", v[i].desc);
    if (row(ui_idn(base, (u32)i * 2), body, v[i].title, sub, v[i].thumb, IC_TEXT)) article_start(v[i].lang, v[i].title, true);
    if (ui_icon_btn(ui_idn(base, (u32)i * 2 + 1), del, IC_CLOSE, T.text2, saved ? "Remove from Saved" : "Remove from History"))
      remove = i;
  }
  gfx_clip_pop();
  ui_scrollbar(sc, r, content);
  if (remove >= 0) {
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
  float content = FM_COUNT(kLangs) * rh + DP(40);
  ui_scroll(sc, ui_id("wk.pick"), r, content);
  gfx_clip_push(r);
  u32 base = ui_id("wk.pick.row");
  for (int i = 0; i < FM_COUNT(kLangs); i++) {
    FmRect c = { x, r.y + DP(8) - sc->y + i * rh, col, rh };
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
  article_start(lang && lang[0] ? lang : S.lang, title, S.art_ok);
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
    if (pm > 0) fm_snprintf(s, sizeof s, "Downloading from Wikimedia\xE2\x80\xA6 %d%%", pm / 10);
    else fm_strlcpy(s, "Finding the file\xE2\x80\xA6", sizeof s);
    font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, c.x + DP(14), c.y + DP(8), s, c.w - DP(28), T.text);
    FmRect bar = { c.x + DP(14), c.y + c.h - DP(12), c.w - DP(28), DP(4) };
    gfx_rrect(bar, DP(2), T.surface3);
    bar.w *= FM_CLAMP(pm / 1000.0f, 0.0f, 1.0f);
    if (bar.w > 0) gfx_rrect(bar, DP(2), T.accent);
  }
  if ((S.t_image || S.t_play || S.t_random) && !ui_menu_is_open()) ui_animate();
  if (ui_input_ok() && !ui_menu_is_open()) {
    if (ui_key(SDLK_ESCAPE, 0) || ui_key(SDLK_AC_BACK, 0) || ui_key(SDLK_BACKSPACE, KMOD_ALT)) {
      if (ui.focus) ui_focus(0);
      else go_back();
    }
    if (ui_key(SDLK_f, KMOD_CTRL) || ui_key(SDLK_l, KMOD_CTRL)) { S.focus_search = true; go(WP_HOME); }
  }
}

void wiki_demo_page(const char *page) {
  S.demo_page = !page ? -1 : !strcmp(page, "media") ? WP_MEDIA : !strcmp(page, "toc") ? WP_TOC
              : !strcmp(page, "langs") ? WP_LANGS : !strcmp(page, "related") ? WP_RELATED : -1;
}

void wiki_shutdown(void) {
  task_drop(&S.t_feed);
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
  wk_list_free(&S.res);
  wk_article_free(&S.art);
  wk_media_free(&S.media);
  wk_langs_free(&S.langs);
  wk_list_free(&S.rel);
  wk_list_free(&S.links);
  fm_free(S.bh);
  fm_free(S.saved);
  fm_free(S.hist);
  memset(&S, 0, sizeof S);
}
