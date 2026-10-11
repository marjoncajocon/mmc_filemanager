/* fvsrc_karaoke.c -- Karaoke: sing-along versions of songs, from YouTube.
**
** Design decisions:
**   - No karaoke service has a public API (KaraFun, Smule, Singa are closed
**     catalogues, checked 2026-10-11); the big karaoke channels (Sing King,
**     KaraFun, Zoom Karaoke, ...) publish on YouTube. So this source is
**     YouTube's own search with "karaoke" added to the words, and playing
**     and downloading are YouTube's: nothing new to keep working.
**   - Results that say karaoke (or instrumental, sing along, minus one) in
**     the title, or come from a known karaoke channel, go first; the order
**     among them is YouTube's.
*/
#include "fvsrc_int.h"

static int score(const FmVsrcItem *it) {
  static const char *const kWords[] = { "karaoke", "sing along", "instrumental", "minus one", "minusone", "backing track",
                                        "videoke" };
  static const char *const kChannels[] = { "sing king", "karafun", "zoom karaoke", "karaoke", "stingray", "ameritz",
                                           "singsong", "lugn", "musisi karaoke", "atomic karaoke", "global karaoke" };
  int s = 0;
  for (int i = 0; i < FM_COUNT(kWords); i++)
    if (fm_stristr(it->title, kWords[i])) { s += 2; break; }
  for (int i = 0; i < FM_COUNT(kChannels); i++)
    if (fm_stristr(it->channel, kChannels[i])) { s += 1; break; }
  return s;
}

static FmErr ka_search(const FmVsrcConf *c, const char *query, const char *token, FmVsrcPage *out,
                       volatile int *cancel) {
  if (!query || !*query) {
    memset(out, 0, sizeof *out);
    fm_strlcpy(out->error, "Type a song to sing", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  char q[300];
  if (fm_stristr(query, "karaoke") || fm_stristr(query, "videoke")) fm_strlcpy(q, query, sizeof q);
  else fm_snprintf(q, sizeof q, "%s karaoke", query);
  FmErr e = g_vsrc_youtube.search(c, q, token, out, cancel);
  if (e != FM_OK || out->count < 2) return e;
  /* karaoke versions first, YouTube's order kept within each score (a stable insertion sort) */
  for (int i = 1; i < out->count; i++) {
    FmVsrcItem x = out->items[i];
    int sx = score(&x), j = i - 1;
    while (j >= 0 && score(&out->items[j]) < sx) {
      out->items[j + 1] = out->items[j];
      j--;
    }
    out->items[j + 1] = x;
  }
  return FM_OK;
}

static FmErr ka_resolve(const FmVsrcConf *c, const FmVsrcItem *item, FmVsrcStream *out, FmVsrcProgress cb, void *user,
                        char *err, size_t errcap, volatile int *cancel) {
  return g_vsrc_youtube.resolve(c, item, out, cb, user, err, errcap, cancel);
}

static FmErr ka_download(const FmVsrcConf *c, const FmVsrcItem *item, const char *dir, char *out_path, size_t cap,
                         FmVsrcProgress cb, void *user, char *err, size_t errcap, volatile int *cancel) {
  if (!g_vsrc_youtube.download) return vsrc_download(&g_vsrc_youtube, c, item, dir, out_path, cap, cb, user, err, errcap, cancel);
  return g_vsrc_youtube.download(c, item, dir, out_path, cap, cb, user, err, errcap, cancel);
}

static FmErr ka_details(const FmVsrcConf *c, FmVsrcItem *item, volatile int *cancel) {
  return g_vsrc_youtube.details ? g_vsrc_youtube.details(c, item, cancel) : FM_OK;
}

const FmVsrc g_vsrc_karaoke = {
  "karaoke", "Karaoke", IC_MIC, VSRC_SEARCH | VSRC_DIRECT,
  ka_search, ka_resolve, ka_download, ka_details,
  "Sing along: karaoke versions of songs from YouTube's karaoke channels (Sing King, KaraFun ...)",
};
