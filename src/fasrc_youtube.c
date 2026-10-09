/* fasrc_youtube.c -- YouTube as music: search YouTube and play only the
** sound, in the music player (cover = the video's thumbnail).
**
** Design decisions:
**   - Search and stream links come from the built-in YouTube client
**     (fvsrc_innertube.c): no key, no yt-dlp, the same code the video
**     gallery uses. Search results keep their order and paging ("i:<n>").
**   - The stream is the Opus-in-WebM track (itags 251/250/249): the built-in
**     decoder plays it on every system (the music player falls back to the
**     video decoders, fdec_aud.c), so no FFmpeg is needed. Its codec is
**     reported as "WEBM", which the FFmpeg check lets through.
**   - The links expire after some hours, so items carry no url: resolve()
**     asks YouTube at play time (one request, ~0.1-0.3 s).
*/
#include "fasrc_int.h"
#include "fvsrc.h"
#include "fvsrc_int.h"

/* The video gallery's settings, as far as search and audio picking need them. */
static void vconf(const FmAsrcConf *c, FmVsrcConf *vc) {
  memset(vc, 0, sizeof *vc);
  vc->max_height = 144;                 /* the picture is not used: the smallest */
  fm_strlcpy(vc->region, c->country, sizeof vc->region);
  vc->safe_search = c->safe_search;
}

static FmErr yt_search(const FmAsrcConf *c, const char *query, const char *page_token, FmAsrcPage *out,
                       volatile int *cancel) {
  FmVsrcConf vc;
  vconf(c, &vc);
  FmVsrcPage vp;
  memset(&vp, 0, sizeof vp);
  FmErr e = vsrc_innertube_search(&vc, query, page_token, &vp, cancel);
  fm_strlcpy(out->error, vp.error, sizeof out->error);
  fm_strlcpy(out->next, vp.next, sizeof out->next);
  for (int i = 0; i < vp.count; i++) {
    const FmVsrcItem *v = &vp.items[i];
    if (v->live) continue;                         /* live streams have no sound-only file */
    FmAsrcItem *it = asrc_page_add(out);
    if (!it) break;
    fm_strlcpy(it->source, "youtube", sizeof it->source);
    it->kind = AITEM_TRACK;
    fm_strlcpy(it->id, v->id, sizeof it->id);
    fm_strlcpy(it->title, v->title, sizeof it->title);
    fm_strlcpy(it->artist, v->channel, sizeof it->artist);
    fm_strlcpy(it->art, v->thumb, sizeof it->art);
    fm_strlcpy(it->page, v->page, sizeof it->page);
    fm_strlcpy(it->codec, "WEBM", sizeof it->codec);
    it->duration = v->duration;
    it->plays = v->views;
    fm_strlcpy(it->published, v->published, sizeof it->published);
  }
  vsrc_page_free(&vp);
  return e;
}

static FmErr yt_resolve(const FmAsrcConf *c, const FmAsrcItem *item, FmAsrcStream *out, char *err, size_t errcap,
                        volatile int *cancel) {
  memset(out, 0, sizeof *out);
  char id[16];
  if (!vsrc_innertube_id(item->id, id, sizeof id) && !vsrc_innertube_id(item->page, id, sizeof id)) {
    fm_strlcpy(err, "Not a YouTube video", errcap);
    return FM_ERR_NOT_FOUND;
  }
  FmVsrcConf vc;
  vconf(c, &vc);
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
  FmErr e = FM_ERR_UNSUPPORTED;
  /* YouTube now and then refuses a session's links (403 on the first byte,
  ** see fvsrc_innertube.c): one byte of the sound link says so, and a new
  ** session is asked once */
  for (int round = 0; round < 2; round++) {
    vc.fresh = round > 0;
    e = vsrc_innertube_resolve(&vc, id, st, err, errcap, cancel);
    if (e != FM_OK) break;
    e = FM_ERR_UNSUPPORTED;
    for (int i = 0; i < st->nq; i++)
      if (st->q[i].audio_only && st->q[i].playable && st->q[i].url[0]) {
        fm_strlcpy(out->url, st->q[i].url, sizeof out->url);
        fm_strlcpy(out->codec, "WEBM", sizeof out->codec);
        e = FM_OK;
        break;
      }
    if (e != FM_OK) { fm_strlcpy(err, "YouTube listed no sound-only stream for this video", errcap); break; }
    FmNetResp r;
    memset(&r, 0, sizeof r);
    FmErr pe = net_get(out->url, "Range: bytes=0-0\r\n", 4096, &r, cancel);
    int status = r.status;
    net_resp_free(&r);
    if (pe == FM_ERR_CANCEL) { e = pe; break; }
    if (status != 403) break;
    fm_log("youtube audio: link refused (403), asking again with a new session");
    if (round == 1) {
      fm_strlcpy(err, "YouTube refused this video's sound for now; try again in a minute", errcap);
      e = FM_ERR_ACCESS;
    }
  }
  fm_free(st);
  return e;
}

/* Paste a link: that one video. */
static FmErr yt_search_or_link(const FmAsrcConf *c, const char *query, const char *page_token, FmAsrcPage *out,
                               volatile int *cancel) {
  char id[16];
  if (query && strstr(query, "://") && vsrc_innertube_id(query, id, sizeof id)) {
    FmVsrcItem v;
    char err[256];
    FmErr e = vsrc_innertube_item(id, &v, err, sizeof err, cancel);
    if (e != FM_OK) { fm_strlcpy(out->error, err, sizeof out->error); return e; }
    FmAsrcItem *it = asrc_page_add(out);
    if (it) {
      fm_strlcpy(it->source, "youtube", sizeof it->source);
      it->kind = AITEM_TRACK;
      fm_strlcpy(it->id, v.id, sizeof it->id);
      fm_strlcpy(it->title, v.title, sizeof it->title);
      fm_strlcpy(it->artist, v.channel, sizeof it->artist);
      fm_strlcpy(it->art, v.thumb, sizeof it->art);
      fm_strlcpy(it->page, v.page, sizeof it->page);
      fm_strlcpy(it->codec, "WEBM", sizeof it->codec);
      it->duration = v.duration;
      it->plays = v.views;
    }
    return FM_OK;
  }
  return yt_search(c, query, page_token, out, cancel);
}

const FmAsrc g_asrc_youtube = {
  "youtube", "YouTube", IC_PLAY_BADGE, ASRC_SEARCH,
  yt_search_or_link, NULL, NULL, yt_resolve, NULL,
  "Any song on YouTube, sound only \xC2\xB7 no key needed",
};
