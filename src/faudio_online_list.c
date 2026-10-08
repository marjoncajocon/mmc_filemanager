/* faudio_online_list.c -- the lists of the online audio view: station tiles,
** track rows and cover cards, with artwork and the "now playing" marks.
**
** Design decisions:
**   - One list routine, three layouts picked from what the page holds:
**     stations get dense tiles (logo, name, country and tags, a format
**     badge and a LIVE dot), tracks and episodes get rows (artwork, title,
**     artist, length, heart, download, menu), podcasts and albums get
**     cover cards. A mixed page (the library) uses rows, which show every
**     kind well.
**   - Station logos are often missing or broken: a letter avatar on a
**     colour taken from the name stands in, and stays under the logo while
**     it fades in. Logos are fitted on a light tile (many are wide or
**     transparent); covers are cropped square by the thumbnail loader.
**   - Only visible rows are drawn and only they ask for artwork, so a
**     300-episode feed costs what fits on screen.
**   - The playing row shows moving bars only while sound is really coming
**     out; they are redrawn about 14 times a second through a wake timer,
**     not every frame. Paused, the bars stand still and nothing redraws.
**   - Buttons inside a row are hit-tested after the row, so they win the
**     click (the immediate-mode UI gives a press to the last widget).
*/
#include "faudio_online_int.h"
#include "fview_int.h"
#include "fdec_vid.h"

#define BADGE_FS 0.86f

/* ---- small pieces ------------------------------------------------------------------- */

void ao_fmt_dur(double sec, char *out, size_t cap) {
  if (sec <= 0) { if (cap) out[0] = 0; return; }
  long s = (long)(sec + 0.5);
  if (s >= 3600) fm_snprintf(out, cap, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
  else fm_snprintf(out, cap, "%ld:%02ld", s / 60, s % 60);
}

void ao_badge_text(const FmAsrcItem *it, char *out, size_t cap) {
  if (it->codec[0] && it->bitrate > 0) fm_snprintf(out, cap, "%s %d", it->codec, it->bitrate);
  else if (it->codec[0]) fm_strlcpy(out, it->codec, cap);
  else if (it->bitrate > 0) fm_snprintf(out, cap, "%dk", it->bitrate);
  else if (cap) out[0] = 0;
}

static Uint32 SDLCALL tick_cb(Uint32 interval, void *t) {
  FM_UNUSED(interval);
  SDL_AtomicSet((SDL_atomic_t *)t, 0);
  app_wake();
  return 0;
}

void ao_tick(SDL_atomic_t *t, u32 ms) {
  if (SDL_AtomicCAS(t, 0, 1) && !SDL_AddTimer(ms ? ms : 1, tick_cb, t)) SDL_AtomicSet(t, 0);
}

static SDL_atomic_t g_tick_bars, g_tick_spin, g_tick_shimmer;

float ao_shimmer(int k) {
  return 0.5f + 0.5f * sinf((float)(ui.now % 100000) / 1600.0f * 6.2831853f - (float)k * 0.45f);
}

static u32 name_hash(const char *s) {
  u32 h = 2166136261u;
  for (; *s; s++) { h ^= (u8)*s; h *= 16777619u; }
  return h;
}

void ao_avatar(FmRect r, const char *name, float rad, bool round) {
  u32 h = name_hash(name);
  FmColor a = kAccents[h % UI_ACCENTS], b = kAccents[(h / UI_ACCENTS + 2) % UI_ACCENTS];
  FmColor top = col_mix(a, AO_WHITE, T.dark ? 0.05f : 0.12f), bot = col_mix(b, FM_HEX(0x101018), 0.3f);
  if (round) {
    gfx_circle(r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w * 0.5f, a);
  } else {
    gfx_rrect_vgrad(r, rad, top, bot);
  }
  /* the first letter or digit of the name, skipping "The", quotes and symbols */
  const char *p = name;
  if (!fm_strnicmp(p, "the ", 4)) p += 4;
  u32 cp = '?';
  while (*p) {
    u32 c;
    int n = utf8_decode(p, &c);
    if (n <= 0) break;
    if (c >= 0x80 || (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z')) { cp = c; break; }
    p += n;
  }
  if (cp >= 'a' && cp <= 'z') cp -= 32;
  char s[8];
  int n = utf8_encode(cp, s);
  s[n] = 0;
  font_draw_center(FONT_BOLD, r.h * 0.44f, r, s, AO_WHITE);
}

void ao_art(FmRect r, const FmAsrcItem *it, float rad) {
  /* whole pixels: crisp pictures and texture coordinates inside 0..1 */
  float x1 = floorf(r.x + r.w + 0.5f), y1 = floorf(r.y + r.h + 0.5f);
  r.x = floorf(r.x + 0.5f);
  r.y = floorf(r.y + 0.5f);
  r.w = x1 - r.x;
  r.h = y1 - r.y;
  bool station = it->kind == AITEM_STATION;
  float fade = 1;
  SDL_Texture *tex = it->art[0] ? othumb_get_ex(it->art, (int)r.w, station ? 0.0f : 1.0f, NULL, &fade) : NULL;
  if (!tex || fade < 1) ao_avatar(r, it->title[0] ? it->title : it->id, rad, false);
  if (!tex) return;
  FmColor tint = col_alpha(AO_WHITE, fade);
  if (station) {
    int tw = 1, th = 1;
    SDL_QueryTexture(tex, NULL, NULL, &tw, &th);
    gfx_rrect(r, rad, col_alpha(FM_HEX(0xF7F8FB), fade));
    float k = FM_MIN(r.w * 0.82f / (float)FM_MAX(1, tw), r.h * 0.82f / (float)FM_MAX(1, th));
    FmRect d = rect_center(r, floorf((float)tw * k), floorf((float)th * k));
    d.x = floorf(d.x);
    d.y = floorf(d.y);
    gfx_tex(tex, NULL, d, tint);
  } else {
    gfx_tex_rounded(tex, r, rad, tint);
  }
  if (fade < 1) ui_animate();
}

void ao_bars(FmRect r, FmColor c, bool moving) {
  float t = (float)(ui.now % 100000) / 1000.0f;
  float bw = r.w / 5.0f, gap = bw * 0.5f;
  float x = r.x + (r.w - (bw * 3 + gap * 2)) * 0.5f;
  static const float kPh[3] = { 0.0f, 1.7f, 3.1f }, kSp[3] = { 7.3f, 9.1f, 6.1f }, kRest[3] = { 0.45f, 0.75f, 0.3f };
  for (int i = 0; i < 3; i++) {
    float h = moving ? 0.3f + 0.7f * (0.5f + 0.5f * sinf(t * kSp[i] + kPh[i])) : kRest[i];
    float bh = FM_MAX(bw, r.h * h);
    gfx_rrect(FM_RECT(x, r.y + r.h - bh, bw, bh), bw * 0.5f, c);
    x += bw + gap;
  }
  if (moving) ao_tick(&g_tick_bars, 110);     /* about 9 a second: alive, and cheap */
}

/* A turning arc: a stream is connecting or buffering. */
static void spinner_arc(float cx, float cy, float rad, FmColor c) {
  float a = (float)(ui.now % 1000) / 1000.0f * 6.2831853f;
  gfx_arc(cx, cy, rad, DP(2.2f), a, a + 1.9f, c);
  ao_tick(&g_tick_spin, 40);
}

/* The playing mark over artwork: a scrim with bars, or a spinner. */
static void now_mark(FmRect art, float rad, int st) {
  gfx_rrect(art, rad, FM_RGBA(0, 0, 0, 120));
  float s = FM_MIN(art.w, art.h);
  if (st == AUDIO_BUFFERING) spinner_arc(art.x + art.w * 0.5f, art.y + art.h * 0.5f, s * 0.22f, AO_WHITE);
  else if (st == AUDIO_FAILED) icon_draw(IC_WARN, rect_center(art, s * 0.4f, s * 0.4f), AO_WHITE);
  else ao_bars(rect_center(art, s * 0.42f, s * 0.36f), AO_WHITE, st == AUDIO_PLAYING);
}

static float badge(float x, float y, float h, const char *t, FmColor bg, FmColor fg) {
  float fs = ui.m.font_small * BADGE_FS;
  float w = font_width(FONT_BOLD, fs, t, -1) + DP(10);
  FmRect b = { x, y, w, h };
  gfx_rrect(b, DP(5), bg);
  font_draw_center(FONT_BOLD, fs, b, t, fg);
  return w;
}

static float live_badge(float x, float y, float h) {
  float fs = ui.m.font_small * BADGE_FS;
  float w = font_width(FONT_BOLD, fs, "LIVE", -1) + DP(20);
  FmRect b = { x, y, w, h };
  gfx_rrect(b, DP(5), col_alpha(T.danger, T.dark ? 0.25f : 0.14f));
  gfx_circle(b.x + DP(7), b.y + h * 0.5f, DP(3), T.danger);
  font_draw(FONT_BOLD, fs, b.x + DP(13), b.y + (h - font_line_h(fs)) * 0.5f, "LIVE", -1, T.danger);
  return w;
}

static void join_dot(char *out, size_t cap, const char *part) {
  if (!part || !part[0]) return;
  if (out[0]) fm_strlcat(out, "  \xC2\xB7  ", cap);
  fm_strlcat(out, part, cap);
}

/* The first two tags of a station: "jazz, smooth jazz" */
static void two_tags(const char *tags, char *out, size_t cap) {
  fm_strlcpy(out, tags, cap);
  int commas = 0;
  for (char *p = out; *p; p++)
    if (*p == ',' && ++commas == 2) { *p = 0; break; }
}

static void short_date(const char *iso, char *out, size_t cap) {
  static const char *const kMon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  int y = 0, m = 0, d = 0;
  if (sscanf(iso, "%d-%d-%d", &y, &m, &d) == 3 && m >= 1 && m <= 12) fm_snprintf(out, cap, "%s %d, %d", kMon[m - 1], d, y);
  else fm_strlcpy(out, iso, cap);
}

/* A codec the built-in decoders cannot play over HTTP, with no FFmpeg. */
static bool needs_ffmpeg(const char *codec) {
  static const char *const kBuiltin[] = { "MP3", "MP2", "FLAC", "WAV" };
  if (!codec[0]) return false;
  for (int i = 0; i < FM_COUNT(kBuiltin); i++)
    if (!fm_stricmp(codec, kBuiltin[i])) return false;
  return !ff_available();
}

int ao_layout_for(const AoList *l) {
  if (l->n == 0) return AL_ROWS;
  int st = 0, co = 0;
  for (int i = 0; i < l->n; i++) {
    st += l->items[i].kind == AITEM_STATION;
    co += aitem_container(&l->items[i]);
  }
  if (st == l->n) return AL_STATIONS;
  if (co == l->n) return AL_COVERS;
  return AL_ROWS;
}

/* ---- geometry ------------------------------------------------------------------------- */

typedef struct Geo {
  int layout, cols;
  float pad, gap, cw, ch;       /* cell width and height */
} Geo;

static Geo geometry(FmRect view, int layout) {
  Geo g;
  g.layout = layout;
  g.pad = DP(view.w < DP(520) ? 10 : 16);
  float avail = view.w - g.pad * 2;
  switch (layout) {
    case AL_STATIONS:
      g.gap = DP(10);
      g.cols = FM_CLAMP((int)((avail + g.gap) / (DP(280) + g.gap)), 1, 5);
      g.ch = DP(ui.touch_mode ? 84 : 78);
      break;
    case AL_COVERS:
      g.gap = DP(16);
      g.cols = FM_CLAMP((int)((avail + g.gap) / (DP(ui.touch_mode ? 150 : 168) + g.gap)), 2, 8);
      break;
    default:
      g.gap = 0;
      g.cols = 1;
      g.ch = DP(ui.touch_mode ? 72 : 64);
      break;
  }
  g.cw = (avail - g.gap * (float)(g.cols - 1)) / (float)g.cols;
  if (layout == AL_COVERS)
    g.ch = g.cw + DP(10) + font_line_h(ui.m.font) * 2 + font_line_h(ui.m.font_small) + DP(14);
  return g;
}

static FmRect cell(const Geo *g, FmRect view, float top, int i) {
  int r = i / g->cols, c = i % g->cols;
  float rgap = g->layout == AL_ROWS ? 0 : g->gap;
  return FM_RECT(view.x + g->pad + (float)c * (g->cw + g->gap), top + (float)r * (g->ch + rgap), g->cw, g->ch);
}

static float rows_h(const Geo *g, int n) {
  int rows = (n + g->cols - 1) / g->cols;
  float rgap = g->layout == AL_ROWS ? 0 : g->gap;
  return rows > 0 ? (float)rows * (g->ch + rgap) - rgap : 0;
}

/* ---- skeletons ------------------------------------------------------------------------ */

static void skeleton(FmRect c, const Geo *g, int k) {
  float w = ao_shimmer(k);
  FmColor a = col_mix(T.surface2, T.text, (T.dark ? 0.05f : 0.04f) + 0.04f * w);
  FmColor b = col_mix(T.surface2, T.text, (T.dark ? 0.04f : 0.03f) + 0.03f * w);
  float bh = DP(11);
  if (g->layout == AL_COVERS) {
    gfx_rrect(FM_RECT(c.x, c.y, c.w, c.w), DP(14), a);
    float y = c.y + c.w + DP(12);
    gfx_rrect(FM_RECT(c.x, y, c.w * 0.86f, bh), bh * 0.5f, b);
    gfx_rrect(FM_RECT(c.x, y + font_line_h(ui.m.font) + DP(2), c.w * 0.55f, DP(9)), DP(4.5f), b);
    return;
  }
  if (g->layout == AL_STATIONS) gfx_rrect(c, DP(14), col_mix(T.surface2, T.surface, 0.4f));
  float s = c.h - DP(g->layout == AL_STATIONS ? 22 : 16);
  FmRect art = { c.x + DP(g->layout == AL_STATIONS ? 11 : 8), c.y + (c.h - s) * 0.5f, s, s };
  gfx_rrect(art, DP(10), a);
  float x = art.x + s + DP(14), tw = c.x + c.w - x - DP(16);
  gfx_rrect(FM_RECT(x, c.y + c.h * 0.5f - DP(16), tw * 0.7f, bh), bh * 0.5f, b);
  gfx_rrect(FM_RECT(x, c.y + c.h * 0.5f + DP(5), tw * 0.42f, DP(9)), DP(4.5f), b);
}

/* ---- one station tile ------------------------------------------------------------------ */

typedef struct Ctx {
  AoListRes *res;
  const AoListOpts *o;
  u32 id;
  bool fav;
  int st;                       /* AUDIO_* when it is the one playing, else AUDIO_IDLE */
} Ctx;

static bool heart_btn(u32 id, FmRect b, bool fav) {
  return ui_icon_btn(id, b, fav ? IC_HEART_FILL : IC_HEART, fav ? T.accent : T.text3,
                     fav ? "Remove from favorites" : "Favorite");
}

static void station_tile(FmRect c, const FmAsrcItem *it, int i, Ctx *x) {
  int f = ui_hit(x->id, c);
  bool on = x->st != AUDIO_IDLE;
  float hv = ui.touch_mode ? 0 : ui_anim(ui_idn(x->id, 1), (f & UI_HOVER) ? 1.0f : 0.0f, 14.0f);
  gfx_rrect(c, DP(14), on ? col_mix(T.surface2, T.accent, T.dark ? 0.18f : 0.1f) : T.surface2);
  if (hv > 0.01f) gfx_rrect(c, DP(14), col_alpha(T.hover, hv));
  if (f & UI_HELD) gfx_rrect(c, DP(14), T.press);
  if (on) gfx_rrect_line(c, DP(14), DP(1.5f), col_alpha(T.accent, 0.8f));
  float s = c.h - DP(22);
  FmRect art = { c.x + DP(11), c.y + DP(11), s, s };
  ao_art(art, it, DP(10));
  if (on) now_mark(art, DP(10), x->st);
  else if (hv > 0.01f) {
    gfx_rrect(art, DP(10), FM_RGBA(0, 0, 0, (int)(110 * hv)));
    float is = s * 0.42f;
    icon_draw(IC_PLAY, rect_center(art, is, is), col_alpha(AO_WHITE, hv));
  }
  /* the LIVE dot on the logo's corner */
  gfx_circle(art.x + art.w - DP(1), art.y + DP(1), DP(5.5f), T.surface2);
  gfx_circle(art.x + art.w - DP(1), art.y + DP(1), DP(3.8f), T.danger);
  float bs = FM_MIN(DP(40), c.h * 0.5f);
  FmRect hb = { c.x + c.w - bs - DP(4), c.y + (c.h - bs) * 0.5f, bs, bs };
  float tx = art.x + s + DP(12), tw = hb.x - tx - DP(2);
  float lh = font_line_h(ui.m.font), ls = font_line_h(ui.m.font_small);
  float bh = font_line_h(ui.m.font_small * BADGE_FS) + DP(3);
  float ty = c.y + (c.h - lh - ls - bh - DP(6)) * 0.5f;
  font_draw_ellipsis(FONT_BOLD, ui.m.font, tx, ty, it->title[0] ? it->title : "Station", tw, on ? T.accent : T.text);
  char meta[256], tags[160];
  meta[0] = 0;
  two_tags(it->album, tags, sizeof tags);
  join_dot(meta, sizeof meta, it->artist);
  join_dot(meta, sizeof meta, tags);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, tx, ty + lh, meta[0] ? meta : "Live radio", tw, T.text2);
  float by = ty + lh + ls + DP(6);
  char b[32];
  ao_badge_text(it, b, sizeof b);
  float bx = tx;
  bx += live_badge(bx, by, bh) + DP(6);
  if (needs_ffmpeg(it->codec)) {
    /* said up front: this one stays silent without FFmpeg */
    char nb[48];
    fm_snprintf(nb, sizeof nb, "%s \xC2\xB7 needs FFmpeg", it->codec);
    if (bx + DP(40) < tx + tw) badge(bx, by, bh, nb, col_alpha(T.warn, T.dark ? 0.25f : 0.16f), T.warn);
  } else if (b[0] && bx + DP(40) < tx + tw) {
    badge(bx, by, bh, b, T.surface3, T.text2);
  }
  if (heart_btn(ui_idn(x->id, 2), hb, x->fav)) x->res->fav = i;
  if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
  if (f & UI_CLICK) {
    if (on && (x->st == AUDIO_PLAYING || x->st == AUDIO_PAUSED)) x->res->toggle = true;
    else x->res->play = i;
  } else if ((f & UI_RCLICK) || (f & UI_LONG)) {
    x->res->menu = i;
    x->res->menu_x = ui.mx;
    x->res->menu_y = ui.my;
  }
}

/* ---- one track / episode / container row ---------------------------------------------------- */

static void track_row(FmRect c, const FmAsrcItem *it, int i, Ctx *x, bool last) {
  int f = ui_hit(x->id, c);
  bool on = x->st != AUDIO_IDLE;
  bool cont = aitem_container(it);
  FmRect bg = rect_inset2(c, DP(4), DP(2));
  if (on) gfx_rrect(bg, DP(12), col_mix(T.surface, T.accent, T.dark ? 0.14f : 0.08f));
  else if (f & UI_HOVER) gfx_rrect(bg, DP(12), T.hover);
  if (f & UI_HELD) gfx_rrect(bg, DP(12), T.press);
  float s = c.h - DP(16);
  FmRect art = { c.x + DP(10), c.y + DP(8), s, s };
  ao_art(art, it, DP(8));
  if (on) now_mark(art, DP(8), x->st);
  else if ((f & UI_HOVER) && !cont) {
    gfx_rrect(art, DP(8), FM_RGBA(0, 0, 0, 110));
    icon_draw(IC_PLAY, rect_center(art, s * 0.42f, s * 0.42f), AO_WHITE);
  }
  /* buttons from the right: menu, download, heart */
  float bs = FM_MIN(DP(40), c.h - DP(16));
  FmRect in = { art.x + s + DP(12), c.y, c.x + c.w - (art.x + s + DP(12)) - DP(6), c.h };
  bool wide = c.w >= DP(560);
  FmRect mb = rect_center(rect_cut_right(&in, bs), bs, bs);
  if (ui_icon_btn(ui_idn(x->id, 3), mb, IC_MORE, T.text2, "More")) {
    x->res->menu = i;
    x->res->menu_x = mb.x + mb.w;
    x->res->menu_y = mb.y + mb.h;
  }
  if (cont) {
    float is = DP(18);
    FmRect cr = rect_cut_right(&in, DP(28));
    icon_draw(IC_CHEVRON_RIGHT, rect_center(cr, is, is), T.text3);
  } else {
    if (wide && it->kind == AITEM_TRACK) {
      FmRect db = rect_center(rect_cut_right(&in, bs), bs, bs);
      bool has = adl_has(it);
      if (ui_icon_btn(ui_idn(x->id, 4), db, has ? IC_CHECK : IC_DOWNLOAD, has ? T.success : T.text2,
                      has ? "Downloaded" : "Download"))
        x->res->dl = i;
    }
    FmRect hb = rect_center(rect_cut_right(&in, bs), bs, bs);
    if (heart_btn(ui_idn(x->id, 2), hb, x->fav)) x->res->fav = i;
  }
  /* length (or LIVE) column */
  char d[32];
  d[0] = 0;
  bool col = false;              /* the length has its own column (else it joins the line below) */
  if (it->kind == AITEM_STATION) {
    float bh = font_line_h(ui.m.font_small * BADGE_FS) + DP(3);
    float lw = font_width(FONT_BOLD, ui.m.font_small * BADGE_FS, "LIVE", -1) + DP(20);
    FmRect lr = rect_cut_right(&in, lw + DP(12));
    live_badge(lr.x, c.y + (c.h - bh) * 0.5f, bh);
  } else if (!cont) {
    ao_fmt_dur(it->duration, d, sizeof d);
    col = d[0] && in.w > DP(160) && (wide || !x->o->episodes);
    if (col) {
      float dw = font_width(FONT_REGULAR, ui.m.font_small, d, -1) + DP(14);
      FmRect dr = rect_cut_right(&in, dw);
      ui_label(dr, d, FONT_REGULAR, ui.m.font_small, T.text3, UI_RIGHT);
      rect_cut_right(&in, DP(6));
    }
  }
  /* title and the line under it */
  char meta[400], tmp[64];
  meta[0] = 0;
  if (cont) {
    join_dot(meta, sizeof meta, it->kind == AITEM_PODCAST ? "Podcast" : "Album");
    join_dot(meta, sizeof meta, it->artist);
  } else if (it->kind == AITEM_STATION) {
    char tags[160];
    two_tags(it->album, tags, sizeof tags);
    join_dot(meta, sizeof meta, it->artist);
    join_dot(meta, sizeof meta, tags);
  } else if (x->o->episodes) {
    if (it->published[0]) { short_date(it->published, tmp, sizeof tmp); join_dot(meta, sizeof meta, tmp); }
    if (!col) { ao_fmt_dur(it->duration, tmp, sizeof tmp); join_dot(meta, sizeof meta, tmp); }
  } else {
    join_dot(meta, sizeof meta, it->artist);
    join_dot(meta, sizeof meta, it->album);
  }
  float lh = font_line_h(ui.m.font), ls = font_line_h(ui.m.font_small);
  float ty = c.y + (c.h - lh - ls) * 0.5f;
  font_draw_ellipsis(on ? FONT_BOLD : FONT_REGULAR, ui.m.font, in.x, ty, it->title[0] ? it->title : "Untitled", in.w,
                     on ? T.accent : T.text);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, in.x, ty + lh, meta, in.w, T.text2);
  if (!last) ui_divider(in.x, c.x + c.w - DP(10), c.y + c.h - DP(0.5f));
  if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
  if (f & UI_CLICK) {
    if (cont) x->res->open = i;
    else if (on && (x->st == AUDIO_PLAYING || x->st == AUDIO_PAUSED)) x->res->toggle = true;
    else x->res->play = i;
  } else if ((f & UI_RCLICK) || (f & UI_LONG)) {
    x->res->menu = i;
    x->res->menu_x = ui.mx;
    x->res->menu_y = ui.my;
  }
}

/* ---- one cover card ------------------------------------------------------------------------ */

static void cover_card(FmRect c, const FmAsrcItem *it, int i, Ctx *x) {
  FmRect hit = rect_inset(c, -DP(6));
  int f = ui_hit(x->id, hit);
  float hv = ui.touch_mode ? 0 : ui_anim(ui_idn(x->id, 1), (f & UI_HOVER) ? 1.0f : 0.0f, 14.0f);
  if (hv > 0.01f) gfx_rrect(hit, DP(18), col_alpha(T.hover, hv));
  if (f & UI_HELD) gfx_rrect(hit, DP(18), T.press);
  FmRect art = { c.x, c.y, c.w, c.w };
  gfx_shadow(art, DP(14), DP(10), col_alpha(T.shadow, 0.5f + 0.5f * hv));
  ao_art(art, it, DP(14));
  if (x->fav) {
    float d = DP(26);
    FmRect b = { art.x + art.w - d - DP(8), art.y + DP(8), d, d };
    gfx_circle(b.x + d * 0.5f, b.y + d * 0.5f, d * 0.5f, FM_RGBA(0, 0, 0, 140));
    icon_draw(it->kind == AITEM_PODCAST ? IC_RSS : IC_HEART_FILL, rect_inset(b, DP(6)), AO_WHITE);
  }
  float y = art.y + art.h + DP(10);
  int lines = ao_text_lines(FONT_BOLD, ui.m.font, c.x, y, c.w, it->title[0] ? it->title : "Untitled", 2, T.text, true);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, c.x, y + font_line_h(ui.m.font) * (float)lines + DP(2),
                     it->artist[0] ? it->artist : ao_src_name(it->source), c.w, T.text2);
  if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
  if (f & UI_CLICK) x->res->open = i;
  else if ((f & UI_RCLICK) || (f & UI_LONG)) {
    x->res->menu = i;
    x->res->menu_x = ui.mx;
    x->res->menu_y = ui.my;
  }
}

/* ---- the list ------------------------------------------------------------------------------ */

AoListRes ao_list_draw(AoList *l, FmRect view, const AoListOpts *o) {
  AoListRes res;
  memset(&res, 0, sizeof res);
  res.play = res.open = res.menu = res.fav = res.dl = -1;
  Geo g = geometry(view, o->layout);
  float top_pad = DP(o->layout == AL_ROWS ? 6 : 12);
  int n = o->fixed ? 0 : l->n;
  float content = top_pad + o->header + rows_h(&g, n + o->skeleton) + o->footer;
  if (!o->fixed) ui_scroll(&l->scroll, l->id, view, content);
  float sy = o->fixed ? 0 : l->scroll.y;
  float top = view.y + top_pad + o->header - sy;
  res.header_y = view.y + top_pad - sy;
  res.footer_y = top + rows_h(&g, n + o->skeleton);
  gfx_clip_push(view);
  for (int i = 0; i < n; i++) {
    FmRect c = cell(&g, view, top, i);
    if (c.y > view.y + view.h) break;
    if (c.y + c.h + DP(8) < view.y) continue;
    const FmAsrcItem *it = &l->items[i];
    char key[192];
    aitem_key(it, key, sizeof key);
    Ctx x;
    x.res = &res;
    x.o = o;
    x.id = ui_idn(l->id, (u32)i + 1);
    x.st = o->play_key && o->play_key[0] && !strcmp(o->play_key, key) ? o->play_state : AUDIO_IDLE;
    x.fav = alib_has_key(aitem_container(it) && it->kind == AITEM_PODCAST ? ALIB_SUBS : ALIB_FAV, key);
    if (o->layout == AL_STATIONS) station_tile(c, it, i, &x);
    else if (o->layout == AL_COVERS) cover_card(c, it, i, &x);
    else track_row(c, it, i, &x, i == n - 1 && o->skeleton == 0);
  }
  for (int k = 0; k < o->skeleton; k++) {
    FmRect c = cell(&g, view, top, n + k);
    if (c.y > view.y + view.h) break;
    skeleton(c, &g, k);
  }
  gfx_clip_pop();
  if (!o->fixed) ui_scrollbar(&l->scroll, view, content);
  if (o->skeleton > 0) ao_tick(&g_tick_shimmer, 33);            /* the shimmer, only while loading */
  res.near_end = !o->fixed && l->n > 0 && content - sy - view.h < view.h * 1.5f;
  return res;
}
