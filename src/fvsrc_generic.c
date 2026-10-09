/* fvsrc_generic.c -- the "Any site" source without yt-dlp: finds the video
** on a pasted page natively, like yt-dlp's generic extractor (see
** fvsrc_generic.h). Android cannot run yt-dlp; everywhere else this answers
** the plain cases at once and leaves the rest to yt-dlp.
**
** Design decisions:
**   - One GET does both jobs. The head callback sees the Content-Type and
**     the first 512 bytes are sniffed (EBML, ftyp, #EXTM3U, ID3, OggS, TS),
**     so a link straight to a media file costs its headers and first bytes;
**     the transfer is stopped there and the player streams the URL itself.
**     A page is read into memory up to GEN_MAX_PAGE and the rest dropped:
**     scanning a cut page beats failing, and players sit early in pages.
**   - The HTML scanner is one forward pass over tags (no DOM, no library):
**     comments, <script>/<style> bodies and unclosed quotes are skipped or
**     bounded by the buffer end, attributes are entity-decoded, every step
**     advances, so malformed or huge pages cost O(n) and nothing more.
**   - Places are trusted in yt-dlp's order: JSON-LD VideoObject, Open Graph
**     and microdata meta tags, twitter:player:stream, <video>/<source>,
**     <link rel=preload as=video>, and last plain media URLs seen anywhere
**     in the text (JS strings too, "https:\/\/..." forms decoded). Page text
**     is full of ad and preview clips, so those raw URLs only count when no
**     declared candidate plays.
**   - Playability is decided from the container (extension, declared type,
**     or a header probe for declared URLs without either) plus codec hints
**     ("codecs=" in type, "vp9"/"h264" in the URL): the built-in decoder
**     plays VP9/Opus WebM; MP4 and HLS need the system decoders (os_mp4) or
**     FFmpeg; everything else needs FFmpeg. q[] lists the unplayable ones
**     too, marked, so the player can say what is missing.
**   - A page that only names a player page (og:video of type text/html,
**     JSON-LD embedUrl, twitter:player, an <iframe>) gets that one page read
**     as well; never deeper, and never YouTube (its own source handles it).
*/
#include "fvsrc_generic.h"
#include "fvsrc_int.h"
#include "fjson.h"
#include "fnet.h"

#define GEN_MAX_CAND  40           /* candidates kept per page */
#define GEN_RAW_MAX   24           /* of which plain URLs from the text */
#define GEN_SNIFF     512          /* bytes looked at before deciding page or media */
#define GEN_PROBE_MAX 3            /* header probes for declared URLs of unknown type */
#define GEN_ATTR_MAX  4096         /* longest attribute value read (longer URLs are dropped anyway) */
#define GEN_MAX_ATTRS 32

enum { GT_RAW = 1, GT_PRELOAD = 3, GT_TAG = 4, GT_META = 5, GT_LD = 6 };   /* trust */

typedef struct Cand {
  char url[VSRC_QURL];
  char mime[64];          /* declared type ("video/mp4; codecs=...") */
  char hint[48];          /* label/res attribute text, for the height */
  int trust;              /* GT_* */
  int w, h;               /* declared size, 0 = unknown */
  int kind;               /* GEN_*, 0 until finalize() */
  bool audio;             /* came from <audio> or is an audio type */
  bool in_video;          /* came from <video> (an .ogg there is Ogg video) */
  bool named;             /* named by meta data, which may point at a player page */
  bool probe;             /* named, no media extension: the headers decide (resolve only) */
  bool dead;
} Cand;

typedef struct Scan {
  Cand c[GEN_MAX_CAND];
  int n, nraw;
  int og_group, tw_group; /* first candidate of the current og:video / twitter group, -1 = none */
  char base[VSRC_QURL];   /* <base href>, raw */
  char embed[VSRC_QURL];  /* a player page to read when nothing else is found */
  int embed_p;
  char title[256], thumb[VSRC_QURL];
  int title_p, thumb_p, dur_p;
  double dur;
} Scan;

/* ---- small text helpers ------------------------------------------------------- */

static bool is_digit(char ch) { return ch >= '0' && ch <= '9'; }
static bool is_alpha(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }
static bool is_alnum(char ch) { return is_digit(ch) || is_alpha(ch); }
static bool is_space(char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f'; }
static char to_lower(char ch) { return ch >= 'A' && ch <= 'Z' ? (char)(ch + 32) : ch; }

static int hex_val(char ch) {
  return is_digit(ch) ? ch - '0' : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1;
}

static bool hex_n(const char *s, int n, u32 *v) {
  *v = 0;
  for (int i = 0; i < n; i++) {
    int d = hex_val(s[i]);
    if (d < 0) return false;
    *v = *v * 16 + (u32)d;
  }
  return true;
}

/* case-insensitive search for needle (ASCII) in [p, end) */
static const char *find_ci(const char *p, const char *end, const char *needle) {
  size_t n = strlen(needle);
  for (; p && (size_t)(end - p) >= n; p++) {
    p = (const char *)memchr(p, needle[0], (size_t)(end - p));
    if (!p || (size_t)(end - p) < n) return NULL;
    if (!fm_strnicmp(p, needle, n)) return p;
  }
  return NULL;
}

/* whitespace collapsed and trimmed, in place */
static void squeeze(char *s) {
  size_t o = 0;
  for (size_t i = 0; s[i]; i++) {
    char ch = is_space(s[i]) ? ' ' : s[i];
    if (ch == ' ' && (o == 0 || s[o - 1] == ' ')) continue;
    s[o++] = ch;
  }
  while (o && s[o - 1] == ' ') o--;
  s[o] = 0;
}

void vsrc_generic_js_unescape(const char *s, size_t len, char *out, size_t cap) {
  size_t o = 0, i = 0;
  if (!cap) return;
  while (s && i < len && s[i] && o + 1 < cap) {
    char ch = s[i];
    if (ch != '\\' || i + 1 >= len) { out[o++] = ch; i++; continue; }
    char e = s[i + 1];
    u32 cp;
    if ((e == 'u' && i + 5 < len && hex_n(s + i + 2, 4, &cp)) || (e == 'x' && i + 3 < len && hex_n(s + i + 2, 2, &cp))) {
      i += e == 'u' ? 6 : 4;
      u32 lo;
      if (cp >= 0xD800 && cp <= 0xDBFF && i + 5 < len && s[i] == '\\' && s[i + 1] == 'u' &&
          hex_n(s + i + 2, 4, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        i += 6;
      }
      if (!cp || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
      char u[4];
      int n = utf8_encode(cp, u);
      if (o + (size_t)n + 1 > cap) break;
      memcpy(out + o, u, (size_t)n);
      o += (size_t)n;
      continue;
    }
    out[o++] = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
    i += 2;
  }
  out[o] = 0;
}

/* ---- URLs ------------------------------------------------------------------------ */

static bool is_http(const char *s) { return !fm_strnicmp(s, "http://", 7) || !fm_strnicmp(s, "https://", 8); }

/* removes "." and ".." segments from an absolute path ("/a/./b/../c" -> "/a/c") */
static void remove_dots(char *path) {
  char *out = path, *in = path;
  while (*in) {
    size_t n = strcspn(in + 1, "/") + 1;      /* "/seg" */
    if (n == 2 && in[1] == '.') {
      in += 2;
      if (!*in) *out++ = '/';
      continue;
    }
    if (n == 3 && in[1] == '.' && in[2] == '.') {
      while (out > path && *--out != '/') {}
      in += 3;
      if (!*in) *out++ = '/';
      continue;
    }
    memmove(out, in, n);
    out += n;
    in += n;
  }
  if (out == path) *out++ = '/';
  *out = 0;
}

bool vsrc_generic_join(const char *base, const char *ref, char *out, size_t cap) {
  char tmp[VSRC_QURL * 2];
  if (!cap) return false;
  out[0] = 0;
  if (!ref) return false;
  while (is_space(*ref)) ref++;
  size_t rn = strlen(ref);
  while (rn && is_space(ref[rn - 1])) rn--;
  if (!rn || rn >= VSRC_QURL) return false;

  /* scheme? */
  size_t sc = 0;
  while (sc < rn && (is_alnum(ref[sc]) || ref[sc] == '+' || ref[sc] == '-' || ref[sc] == '.')) sc++;
  bool has_scheme = sc > 0 && sc < rn && ref[sc] == ':' && is_alpha(ref[0]);
  if (has_scheme) {
    if (!is_http(ref)) return false;               /* javascript:, data:, blob:, mailto: ... */
    fm_snprintf(tmp, sizeof tmp, "%.*s", (int)rn, ref);
  } else {
    if (!base || !is_http(base)) return false;
    const char *auth = strstr(base, "://") + 3;
    size_t an = strcspn(auth, "/?#");
    size_t pre = (size_t)(auth - base) + an;       /* "https://host" */
    const char *path = base + pre;
    size_t pn = strcspn(path, "?#");               /* base path */
    size_t qn = strcspn(path, "#");                /* base path + query */
    if (rn >= 2 && ref[0] == '/' && ref[1] == '/')
      fm_snprintf(tmp, sizeof tmp, "%.*s%.*s", (int)(auth - 2 - base), base, (int)rn, ref);
    else if (ref[0] == '/')
      fm_snprintf(tmp, sizeof tmp, "%.*s%.*s", (int)pre, base, (int)rn, ref);
    else if (ref[0] == '?')
      fm_snprintf(tmp, sizeof tmp, "%.*s%.*s%.*s", (int)pre, base, (int)pn, path, (int)rn, ref);
    else if (ref[0] == '#')
      fm_snprintf(tmp, sizeof tmp, "%.*s%.*s", (int)pre, base, (int)qn, path);
    else {
      size_t dir = pn;
      while (dir > 0 && path[dir - 1] != '/') dir--;
      fm_snprintf(tmp, sizeof tmp, "%.*s%s%.*s%.*s", (int)pre, base, dir ? "" : "/", (int)dir, path, (int)rn,
                  ref);
    }
  }
  /* drop the fragment, lower-case the scheme, clean the path */
  tmp[strcspn(tmp, "#")] = 0;
  char *a = strstr(tmp, "://");
  if (!a) return false;
  a += 3;
  for (char *x = tmp; x < a; x++) *x = to_lower(*x);
  char *p = a + strcspn(a, "/?");
  if (*p == '/') {
    char q[VSRC_QURL * 2];
    size_t pl = strcspn(p, "?");
    fm_strlcpy(q, p + pl, sizeof q);
    p[pl] = 0;
    remove_dots(p);
    fm_strlcat(tmp, q, sizeof tmp);
  }
  /* spaces, controls and non-ASCII bytes percent-encoded */
  size_t o = 0;
  for (const char *s = tmp; *s; s++) {
    u8 ch = (u8)*s;
    if (ch <= ' ' || ch >= 0x7F || ch == '"' || ch == '<' || ch == '>') {
      if (o + 4 >= cap) return false;
      o += (size_t)fm_snprintf(out + o, cap - o, "%%%02X", ch);
    } else {
      if (o + 2 >= cap) return false;
      out[o++] = (char)ch;
    }
  }
  out[o] = 0;
  return o > 0 && vsrc_url_ok(out) && o < VSRC_QURL;
}

static const struct { const char *ext; int kind; } kExt[] = {
  { "mp4", GEN_MP4 }, { "m4v", GEN_MP4 }, { "mov", GEN_MP4 }, { "webm", GEN_WEBM }, { "mkv", GEN_MKV },
  { "m3u8", GEN_HLS }, { "ts", GEN_TS }, { "ogv", GEN_OGV }, { "mp3", GEN_MP3 }, { "m4a", GEN_M4A },
  { "ogg", GEN_OGG }, { "oga", GEN_OGG }, { "opus", GEN_OPUS },
};

static const struct { const char *mime; int kind; } kMime[] = {
  { "video/mp4", GEN_MP4 }, { "video/x-m4v", GEN_MP4 }, { "video/quicktime", GEN_MP4 },
  { "video/webm", GEN_WEBM }, { "audio/webm", GEN_WEBM }, { "video/x-matroska", GEN_MKV },
  { "application/vnd.apple.mpegurl", GEN_HLS }, { "application/x-mpegurl", GEN_HLS },
  { "audio/mpegurl", GEN_HLS }, { "audio/x-mpegurl", GEN_HLS }, { "video/mp2t", GEN_TS },
  { "video/ogg", GEN_OGV }, { "audio/mpeg", GEN_MP3 }, { "audio/mp3", GEN_MP3 }, { "audio/mp4", GEN_M4A },
  { "audio/x-m4a", GEN_M4A }, { "audio/aac", GEN_M4A }, { "audio/ogg", GEN_OGG }, { "audio/opus", GEN_OPUS },
};

static int ext_kind(const char *url) {
  const char *p = strstr(url, "://");
  if (p) p += 3 + strcspn(p + 3, "/?#");           /* past the host */
  else if (url[0] == '/' && url[1] == '/') p = url + 2 + strcspn(url + 2, "/?#");
  else p = url;                                    /* relative */
  size_t n = strcspn(p, "?#");
  const char *dot = NULL;
  for (size_t i = 0; i < n; i++) {
    if (p[i] == '/') dot = NULL;
    else if (p[i] == '.') dot = p + i;
  }
  if (!dot) return GEN_NONE;
  size_t el = (size_t)(p + n - dot - 1);
  for (int i = 0; i < FM_COUNT(kExt); i++)
    if (strlen(kExt[i].ext) == el && !fm_strnicmp(dot + 1, kExt[i].ext, el)) return kExt[i].kind;
  return GEN_NONE;
}

static int mime_kind(const char *mime) {
  if (!mime) return GEN_NONE;
  while (is_space(*mime)) mime++;
  size_t n = strcspn(mime, "; \t");
  if (!n) return GEN_NONE;
  for (int i = 0; i < FM_COUNT(kMime); i++)
    if (strlen(kMime[i].mime) == n && !fm_strnicmp(mime, kMime[i].mime, n)) return kMime[i].kind;
  if (!fm_strnicmp(mime, "video/", 6)) return GEN_VIDEO;
  if (!fm_strnicmp(mime, "audio/", 6)) return GEN_AUDIO;
  return GEN_NONE;
}

int vsrc_generic_kind(const char *url, const char *mime) {
  int k = url ? ext_kind(url) : GEN_NONE;
  return k ? k : mime_kind(mime);
}

static bool kind_audio(int k) { return k >= GEN_MP3 && k <= GEN_AUDIO; }

static int sniff_kind(const u8 *p, size_t n) {
  if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { p += 3; n -= 3; }
  if (n >= 4 && p[0] == 0x1A && p[1] == 0x45 && p[2] == 0xDF && p[3] == 0xA3) {
    for (size_t i = 4; i + 4 <= FM_MIN(n, (size_t)64); i++)
      if (!memcmp(p + i, "webm", 4)) return GEN_WEBM;
    return GEN_MKV;
  }
  if (n >= 12 && !memcmp(p + 4, "ftyp", 4)) return !memcmp(p + 8, "M4A ", 4) ? GEN_M4A : GEN_MP4;
  if (n >= 7 && !memcmp(p, "#EXTM3U", 7)) return GEN_HLS;
  if (n >= 3 && !memcmp(p, "ID3", 3)) return GEN_MP3;
  if (n >= 4 && !memcmp(p, "OggS", 4)) {
    for (size_t i = 4; i + 8 <= n; i++) {
      if (!memcmp(p + i, "theora", 6)) return GEN_OGV;
      if (!memcmp(p + i, "OpusHead", 8)) return GEN_OPUS;
    }
    return GEN_OGG;
  }
  if (n >= 189 && p[0] == 0x47 && p[188] == 0x47) return GEN_TS;
  if (n >= 2 && p[0] == 0xFF && (p[1] & 0xE0) == 0xE0 && (p[1] & 0x06)) return GEN_MP3;
  return GEN_NONE;
}

static bool looks_html(const u8 *p, size_t n) {
  size_t i = 0;
  if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) i = 3;
  while (i < n && is_space((char)p[i])) i++;
  return i < n && p[i] == '<';
}

static const int kWeakHeights[] = { 240, 360, 480, 540, 576, 720, 1080, 1440, 2160 };

int vsrc_generic_height_hint(const char *s) {
  int weak = 0;
  if (!s) return 0;
  for (const char *p = s; *p;) {
    if (!is_digit(*p)) { p++; continue; }
    const char *st = p;
    int v = 0, nd = 0;
    while (is_digit(*p)) { if (nd < 6) v = v * 10 + (*p - '0'); nd++; p++; }
    bool left = st == s || !is_alnum(st[-1]);
    if (nd > 5) continue;
    if ((*p == 'x' || *p == 'X') && is_digit(p[1]) && left) {          /* "1280x720" */
      const char *q = p + 1;
      int h = 0, hd = 0;
      while (is_digit(*q)) { if (hd < 6) h = h * 10 + (*q - '0'); hd++; q++; }
      if (hd <= 5 && !is_alnum(*q) && v >= 100 && h >= 100) return FM_MIN(v, h);
    }
    if ((*p == 'p' || *p == 'P') && !is_alpha(p[1]) && left && v >= 100 && v <= 4320) return v;
    if ((*p == 'k' || *p == 'K') && !is_alnum(p[1]) && left && v == 4) return 2160;
    if (!weak && left && !is_alnum(*p))
      for (int i = 0; i < FM_COUNT(kWeakHeights); i++)
        if (v == kWeakHeights[i]) weak = v;
  }
  return weak;
}

static const char *host_of(const char *url, char *out, size_t cap) {
  const char *a = strstr(url, "://");
  a = a ? a + 3 : url;
  size_t n = strcspn(a, "/?#:");
  fm_snprintf(out, cap, "%.*s", (int)FM_MIN(n, cap - 1), a);
  return out;
}

/* the URL without its scheme, so http and https twins compare equal */
static const char *no_scheme(const char *url) {
  const char *a = strstr(url, "://");
  return a ? a : url;
}

/* same host and path (scheme, query and fragment ignored) */
static bool same_path(const char *a, const char *b) {
  a = no_scheme(a);
  b = no_scheme(b);
  size_t n = strcspn(a, "?#");
  return n == strcspn(b, "?#") && !strncmp(a, b, n);
}

static bool is_youtube(const char *url) {
  char h[128];
  host_of(url, h, sizeof h);
  return fm_stristr(h, "youtube.com") || fm_stristr(h, "youtu.be") || fm_stristr(h, "youtube-nocookie.com");
}

/* ---- candidates ------------------------------------------------------------------ */

static Cand *add_cand(Scan *s, const char *url, int trust, const char *mime) {
  if (!url || !*url || strlen(url) >= VSRC_QURL) return NULL;
  if (trust == GT_RAW && s->nraw >= GEN_RAW_MAX) return NULL;
  for (int i = 0; i < s->n; i++)                        /* the same text again: keep the best trust */
    if (!strcmp(s->c[i].url, url)) {
      if (trust > s->c[i].trust) s->c[i].trust = trust;
      if (mime && *mime && !s->c[i].mime[0]) fm_strlcpy(s->c[i].mime, mime, sizeof s->c[i].mime);
      return &s->c[i];
    }
  if (s->n >= GEN_MAX_CAND) return NULL;
  Cand *c = &s->c[s->n++];
  memset(c, 0, sizeof *c);
  fm_strlcpy(c->url, url, sizeof c->url);
  if (mime) fm_strlcpy(c->mime, mime, sizeof c->mime);
  c->trust = trust;
  if (trust == GT_RAW) s->nraw++;
  return c;
}

static void set_embed(Scan *s, const char *url, int prio) {
  if (url && *url && prio > s->embed_p && strlen(url) < sizeof s->embed) {
    fm_strlcpy(s->embed, url, sizeof s->embed);
    s->embed_p = prio;
  }
}

/* entity-decoded, whitespace-squeezed text into dst when prio beats the current one */
static void put_text(char *dst, size_t cap, int *cur, int prio, const char *val) {
  if (!val || !*val || prio <= *cur) return;
  char t[1024];
  vsrc_html_unescape(val, t, sizeof t);
  squeeze(t);
  if (!t[0]) return;
  fm_strlcpy(dst, t, cap);
  *cur = prio;
}

static void put_duration(Scan *s, int prio, const char *v) {
  if (!v || prio <= s->dur_p) return;
  while (is_space(*v)) v++;
  double d = 0;
  if (*v == 'P' || *v == 'p') d = vsrc_iso_duration(v);
  else if (is_digit(*v)) {
    d = atof(v);
    if (strchr(v, ':')) d = vsrc_clock_seconds(v);
  }
  if (d > 0 && d < 1e7) { s->dur = d; s->dur_p = prio; }
}

/* ---- JSON-LD ----------------------------------------------------------------------- */

static bool ld_is_video(const FmJsonNode *t) {
  if (!t) return false;
  if (t->type == JSON_ARR) {
    for (const FmJsonNode *e = json_first(t); e; e = json_next(e))
      if (ld_is_video(e)) return true;
    return false;
  }
  const char *s = json_str(t, "");
  size_t n = strlen(s);
  return n >= 11 && !strcmp(s + n - 11, "VideoObject");
}

static int ld_int(const FmJsonNode *n) {
  if (n && n->type == JSON_OBJ) n = json_get(n, "value");          /* QuantitativeValue */
  if (!n) return 0;
  if (n->type == JSON_STR) return atoi(json_str(n, "0"));
  double v = json_num(n, 0);
  return v > 0 && v < 100000 ? (int)v : 0;
}

static const char *ld_url(const FmJsonNode *n) {
  if (n && n->type == JSON_ARR) n = json_first(n);
  if (n && n->type == JSON_OBJ) {
    const FmJsonNode *u = json_get(n, "url");
    n = u ? u : json_get(n, "contentUrl");
  }
  return json_str(n, "");
}

/* JSON-LD URLs often keep HTML escapes ("&amp;"): decoded into out */
static const char *ld_unescape(const char *s, char *out, size_t cap) {
  vsrc_html_unescape(s, out, cap);
  return out;
}

static void ld_video(Scan *s, const FmJsonNode *v) {
  char cub[VSRC_QURL], eub[VSRC_QURL], thb[VSRC_QURL];
  const char *cu = ld_unescape(ld_url(json_get(v, "contentUrl")), cub, sizeof cub);
  const char *fmt = vsrc_jstr(json_get(v, "encodingFormat"), "");
  Cand *c = *cu ? add_cand(s, cu, GT_LD, strchr(fmt, '/') ? fmt : NULL) : NULL;
  if (c) {
    c->named = true;
    c->w = ld_int(json_get(v, "width"));
    c->h = ld_int(json_get(v, "height"));
  }
  const char *eu = ld_unescape(ld_url(json_get(v, "embedUrl")), eub, sizeof eub);
  if (*eu) {                          /* usually a player page, even when it ends in ".webm" */
    Cand *e = vsrc_generic_kind(eu, NULL) ? add_cand(s, eu, GT_META, NULL) : NULL;
    if (e) e->named = true;
    else set_embed(s, eu, 5);
  }
  const char *th = ld_url(json_get(v, "thumbnailUrl"));
  if (!*th) th = ld_url(json_get(v, "thumbnail"));
  if (!*th) th = ld_url(json_get(v, "image"));
  th = ld_unescape(th, thb, sizeof thb);
  if (*th && s->thumb_p < 4 && strlen(th) < sizeof s->thumb) { fm_strlcpy(s->thumb, th, sizeof s->thumb); s->thumb_p = 4; }
  put_text(s->title, sizeof s->title, &s->title_p, 4, vsrc_jstr(json_get(v, "name"), ""));
  put_duration(s, 4, vsrc_jstr(json_get(v, "duration"), NULL));
}

static void ld_walk(Scan *s, const FmJsonNode *n, int depth) {
  if (!n || depth > 12) return;
  if (n->type == JSON_OBJ && ld_is_video(json_get(n, "@type"))) ld_video(s, n);
  if (n->type != JSON_OBJ && n->type != JSON_ARR) return;
  for (const FmJsonNode *e = json_first(n); e; e = json_next(e))
    if (e->type == JSON_OBJ || e->type == JSON_ARR) ld_walk(s, e, depth + 1);
}

static void scan_ld(Scan *s, const char *p, size_t n) {
  while (n && is_space(*p)) { p++; n--; }
  if (n >= 4 && !memcmp(p, "<!--", 4)) { p += 4; n -= 4; }
  FmJson j;
  if (json_parse(&j, p, n) != FM_OK) return;
  ld_walk(s, json_root(&j), 0);
  json_free(&j);
}

/* ---- the tag scanner --------------------------------------------------------------- */

typedef struct Tag {
  char name[16];
  bool close;
  int na;
  struct { const char *k; size_t kn; const char *v; size_t vn; } a[GEN_MAX_ATTRS];
} Tag;

/* p points at '<'; returns the position after the tag's '>' (or end) */
static const char *read_tag(const char *p, const char *end, Tag *t) {
  size_t n = 0;
  t->na = 0;
  t->close = false;
  p++;
  if (p < end && *p == '/') { t->close = true; p++; }
  while (p < end && (is_alnum(*p) || *p == '-' || *p == ':')) {
    if (n + 1 < sizeof t->name) t->name[n++] = to_lower(*p);
    p++;
  }
  t->name[n] = 0;
  for (;;) {
    while (p < end && (is_space(*p) || *p == '/')) p++;
    if (p >= end) return end;
    if (*p == '>') return p + 1;
    const char *k = p;
    while (p < end && !is_space(*p) && *p != '=' && *p != '>' && *p != '/') p++;
    size_t kn = (size_t)(p - k);
    if (!kn) { p++; continue; }                     /* a stray '=' */
    while (p < end && is_space(*p)) p++;
    const char *v = NULL;
    size_t vn = 0;
    if (p < end && *p == '=') {
      p++;
      while (p < end && is_space(*p)) p++;
      if (p < end && (*p == '"' || *p == '\'')) {
        char q = *p++;
        const char *e = (const char *)memchr(p, q, (size_t)(end - p));
        if (!e) e = end;
        v = p;
        vn = (size_t)(e - p);
        p = e < end ? e + 1 : end;
      } else {
        v = p;
        while (p < end && !is_space(*p) && *p != '>') p++;
        vn = (size_t)(p - v);
      }
    }
    if (t->na < GEN_MAX_ATTRS) {
      t->a[t->na].k = k;
      t->a[t->na].kn = kn;
      t->a[t->na].v = v;
      t->a[t->na].vn = vn;
      t->na++;
    }
  }
}

/* the decoded, trimmed value of attribute key ("" when missing); false when missing */
static bool tag_val(const Tag *t, const char *key, char *out, size_t cap) {
  size_t kl = strlen(key);
  out[0] = 0;
  for (int i = 0; i < t->na; i++) {
    if (t->a[i].kn != kl || fm_strnicmp(t->a[i].k, key, kl)) continue;
    char raw[GEN_ATTR_MAX];
    size_t n = FM_MIN(t->a[i].vn, sizeof raw - 1);
    if (t->a[i].v) memcpy(raw, t->a[i].v, n);
    raw[n] = 0;
    vsrc_html_unescape(raw, out, cap);
    char *s = out;
    while (is_space(*s)) s++;
    size_t m = strlen(s);
    while (m && is_space(s[m - 1])) m--;
    memmove(out, s, m);
    out[m] = 0;
    return true;
  }
  return false;
}

static void scan_meta(Scan *s, const char *key, const char *val) {
  char k[64];
  fm_strlcpy(k, key, sizeof k);
  for (char *p = k; *p; p++) *p = to_lower(*p);
  if (!*val) return;
  if (!strcmp(k, "og:video") || !strcmp(k, "og:video:url") || !strcmp(k, "og:video:secure_url")) {
    /* og:video starts a group; :url / :secure_url and :type/:width/:height belong to it */
    bool start = !strcmp(k, "og:video") || s->og_group < 0;
    Cand *c = add_cand(s, val, GT_META, NULL);
    if (c) c->named = true;
    if (start && c) s->og_group = (int)(c - s->c);
  } else if (!strncmp(k, "og:video:", 9) && s->og_group >= 0) {
    for (int i = s->og_group; i < s->n; i++) {
      if (!strcmp(k + 9, "type")) fm_strlcpy(s->c[i].mime, val, sizeof s->c[i].mime);
      else if (!strcmp(k + 9, "width")) s->c[i].w = atoi(val);
      else if (!strcmp(k + 9, "height")) s->c[i].h = atoi(val);
    }
    if (!strcmp(k + 9, "duration")) put_duration(s, 2, val);
  } else if (!strcmp(k, "twitter:player:stream")) {
    Cand *c = add_cand(s, val, GT_META - 1, NULL);
    if (c) { c->named = true; s->tw_group = (int)(c - s->c); }
  } else if (!strcmp(k, "twitter:player:stream:content_type") && s->tw_group >= 0) {
    for (int i = s->tw_group; i < s->n; i++) fm_strlcpy(s->c[i].mime, val, sizeof s->c[i].mime);
  } else if (!strcmp(k, "twitter:player")) {
    set_embed(s, val, 2);
  } else if (!strcmp(k, "contenturl")) {                          /* microdata */
    Cand *c = add_cand(s, val, GT_META, NULL);
    if (c) c->named = true;
  } else if (!strcmp(k, "embedurl")) {
    Cand *c = vsrc_generic_kind(val, NULL) ? add_cand(s, val, GT_META, NULL) : NULL;
    if (c) c->named = true;
    else set_embed(s, val, 4);
  } else if (!strcmp(k, "og:title")) {
    put_text(s->title, sizeof s->title, &s->title_p, 3, val);
  } else if (!strcmp(k, "twitter:title")) {
    put_text(s->title, sizeof s->title, &s->title_p, 2, val);
  } else if (!strcmp(k, "thumbnailurl") || !strcmp(k, "og:image") || !strcmp(k, "og:image:url") ||
             !strcmp(k, "og:image:secure_url") || !strcmp(k, "twitter:image") ||
             !strcmp(k, "twitter:image:src")) {
    int p = k[0] == 't' && k[1] == 'h' ? 4 : k[0] == 'o' ? 3 : 2;
    if (p > s->thumb_p && strlen(val) < sizeof s->thumb) { fm_strlcpy(s->thumb, val, sizeof s->thumb); s->thumb_p = p; }
  } else if (!strcmp(k, "duration")) {
    put_duration(s, 3, val);
  } else if (!strcmp(k, "video:duration")) {
    put_duration(s, 2, val);
  }
}

/* "label res size ..." attribute text of a <source>, for the height hint */
static void source_hint(const Tag *t, char *out, size_t cap) {
  static const char *const kKeys[] = { "label", "res", "size", "data-res", "data-quality", "quality",
                                       "title", "data-label", "data-transcodekey" };
  char v[64];
  out[0] = 0;
  for (int i = 0; i < FM_COUNT(kKeys); i++)
    if (tag_val(t, kKeys[i], v, sizeof v) && v[0]) {
      fm_strlcat(out, v, cap);
      fm_strlcat(out, " ", cap);
    }
}

static void scan_tags(Scan *s, const char *html, size_t len) {
  const char *p = html, *end = html + len;
  char media = 0;                     /* inside <video> 'v' or <audio> 'a' */
  char *v = (char *)fm_alloc(GEN_ATTR_MAX), *w = (char *)fm_alloc(256);
  while (p < end) {
    p = (const char *)memchr(p, '<', (size_t)(end - p));
    if (!p) break;
    if (end - p >= 4 && !memcmp(p, "<!--", 4)) {
      const char *e = find_ci(p + 4, end, "-->");
      p = e ? e + 3 : end;
      continue;
    }
    if (p + 1 >= end || !(is_alpha(p[1]) || p[1] == '/')) { p++; continue; }
    Tag t;
    p = read_tag(p, end, &t);
    const char *nm = t.name;
    if (t.close) {
      if (!strcmp(nm, "video") || !strcmp(nm, "audio")) media = 0;
      continue;
    }
    if (!strcmp(nm, "script") || !strcmp(nm, "style") || !strcmp(nm, "title") || !strcmp(nm, "textarea")) {
      char closer[16];
      fm_snprintf(closer, sizeof closer, "</%s", nm);
      const char *e = find_ci(p, end, closer);
      if (!e) e = end;
      if (nm[1] == 'c' && tag_val(&t, "type", w, 256) && fm_stristr(w, "ld+json")) scan_ld(s, p, (size_t)(e - p));
      if (nm[0] == 't' && nm[1] == 'i' && s->title_p < 1) {
        size_t n = FM_MIN((size_t)(e - p), (size_t)1023);
        char raw[1024];
        memcpy(raw, p, n);
        raw[n] = 0;
        put_text(s->title, sizeof s->title, &s->title_p, 1, raw);
      }
      p = e;
      continue;
    }
    if (!strcmp(nm, "base")) {
      if (!s->base[0] && tag_val(&t, "href", v, GEN_ATTR_MAX)) fm_strlcpy(s->base, v, sizeof s->base);
    } else if (!strcmp(nm, "meta")) {
      if (!tag_val(&t, "property", w, 256) || !w[0])
        if (!tag_val(&t, "name", w, 256) || !w[0]) tag_val(&t, "itemprop", w, 256);
      if (w[0] && tag_val(&t, "content", v, GEN_ATTR_MAX)) scan_meta(s, w, v);
    } else if (!strcmp(nm, "video") || !strcmp(nm, "audio")) {
      media = nm[0];
      if ((tag_val(&t, "src", v, GEN_ATTR_MAX) && v[0]) || tag_val(&t, "data-src", v, GEN_ATTR_MAX)) {
        Cand *c = add_cand(s, v, GT_TAG, NULL);
        if (c && media == 'a') c->audio = true;
        if (c && media == 'v') c->in_video = true;
      }
      if (tag_val(&t, "poster", v, GEN_ATTR_MAX) && v[0] && s->thumb_p < 1 && strlen(v) < sizeof s->thumb) {
        fm_strlcpy(s->thumb, v, sizeof s->thumb);
        s->thumb_p = 1;
      }
    } else if (!strcmp(nm, "source")) {
      if (!((tag_val(&t, "src", v, GEN_ATTR_MAX) && v[0]) || tag_val(&t, "data-src", v, GEN_ATTR_MAX)) || !v[0])
        continue;
      tag_val(&t, "type", w, 256);
      if (!media && !vsrc_generic_kind(v, w)) continue;            /* <picture> sources are images */
      Cand *c = add_cand(s, v, GT_TAG, w);
      if (!c) continue;
      if (media == 'a' || !fm_strnicmp(w, "audio/", 6)) c->audio = true;
      if (media == 'v') c->in_video = true;
      source_hint(&t, c->hint, sizeof c->hint);
      if (tag_val(&t, "data-height", w, 256) || tag_val(&t, "height", w, 256)) c->h = atoi(w);
      if (tag_val(&t, "data-width", w, 256) || tag_val(&t, "width", w, 256)) c->w = atoi(w);
    } else if (!strcmp(nm, "link")) {
      if (!tag_val(&t, "href", v, GEN_ATTR_MAX) || !v[0]) continue;
      if (tag_val(&t, "itemprop", w, 256) && w[0]) { scan_meta(s, w, v); continue; }
      tag_val(&t, "rel", w, 256);
      if (fm_stristr(w, "video_src")) { add_cand(s, v, GT_META - 1, NULL); continue; }
      if (fm_stristr(w, "preload") || fm_stristr(w, "prefetch")) {
        tag_val(&t, "as", w, 256);
        if (!fm_stricmp(w, "video") || !fm_stricmp(w, "audio")) add_cand(s, v, GT_PRELOAD, NULL);
      }
    } else if (!strcmp(nm, "iframe") || !strcmp(nm, "embed")) {
      if (!((tag_val(&t, "src", v, GEN_ATTR_MAX) && v[0]) || tag_val(&t, "data-src", v, GEN_ATTR_MAX)) || !v[0])
        continue;
      if (vsrc_generic_kind(v, NULL)) add_cand(s, v, GT_TAG, NULL);
      else if (nm[0] == 'i' && (fm_stristr(v, "embed") || fm_stristr(v, "player") || fm_stristr(v, "video")))
        set_embed(s, v, 1);
    }
  }
  fm_free(v);
  fm_free(w);
}

/* "http" URLs anywhere in the text, JS escapes decoded; only media ones are kept */
static void scan_raw(Scan *s, const char *html, size_t len) {
  const char *p = html, *end = html + len;
  char *u = (char *)fm_alloc(VSRC_QURL * 2);
  while (p < end && s->nraw < GEN_RAW_MAX) {
    p = (const char *)memchr(p, 'h', (size_t)(end - p));
    if (!p) break;
    if (end - p < 12 || memcmp(p, "http", 4)) { p++; continue; }
    const char *q = p + 4;
    if (*q == 's') q++;
    if (*q != ':') { p++; continue; }
    /* the token: up to a quote, space, bracket, or a backslash that is not \/ \u \x */
    const char *r = p;
    while (r < end && (size_t)(r - p) < VSRC_QURL * 2 - 1) {
      char ch = *r;
      if ((u8)ch <= ' ' || ch == '"' || ch == '\'' || ch == '<' || ch == '>' || ch == '`' || ch == '(' ||
          ch == ')' || ch == '{' || ch == '}' || ch == '|' || ch == '^')
        break;
      if (ch == '\\' && !(r + 1 < end && (r[1] == '/' || r[1] == 'u' || r[1] == 'x'))) break;
      r++;
    }
    size_t tn = (size_t)(r - p);
    const char *start = p;
    p = r > p ? r : p + 1;
    if (tn >= VSRC_QURL * 2 - 1) continue;
    vsrc_generic_js_unescape(start, tn, u, VSRC_QURL * 2);
    /* escapes may have hidden a terminator; HTML entities end or join it */
    u[strcspn(u, "\"'<> \t\r\n\\`")] = 0;
    for (char *a = strchr(u, '&'); a; a = strchr(a + 1, '&')) {
      if (!strncmp(a, "&amp;", 5)) memmove(a + 1, a + 5, strlen(a + 5) + 1);
      else if (!strncmp(a, "&quot;", 6) || !strncmp(a, "&#34;", 5) || !fm_strnicmp(a, "&#x22;", 6) ||
               !strncmp(a, "&lt;", 4) || !strncmp(a, "&gt;", 4) || !strncmp(a, "&#39;", 5) ||
               !strncmp(a, "&apos;", 6)) {
        *a = 0;
        break;
      }
    }
    if (!is_http(u) || strlen(u) >= VSRC_QURL) continue;
    int k = ext_kind(u);
    if (k && k != GEN_TS) add_cand(s, u, GT_RAW, NULL);   /* ".ts" in page text is mostly TypeScript */
  }
  fm_free(u);
}

/* Absolute URLs, kinds, sizes; drops what cannot be a video; merges http/https twins. */
static void finalize(Scan *s, const char *page_url) {
  char base[VSRC_QURL], abs[VSRC_QURL];
  if (!(s->base[0] && vsrc_generic_join(page_url, s->base, base, sizeof base)))
    fm_strlcpy(base, page_url, sizeof base);
  for (int i = 0; i < s->n; i++) {
    Cand *c = &s->c[i];
    if (!vsrc_generic_join(base, c->url, abs, sizeof abs)) { c->dead = true; continue; }
    fm_strlcpy(c->url, abs, sizeof c->url);
    if (fm_stristr(c->mime, "html")) { set_embed(s, c->url, c->trust >= GT_LD ? 5 : 4); c->dead = true; continue; }
    if (fm_stristr(c->mime, "flash")) { c->dead = true; continue; }
    {                                                /* encrypted (DRM) streams never play */
      const char *path = strstr(c->url, "://");
      path = path ? path + 3 : c->url;
      size_t pn = strcspn(path, "?#");
      const char *d = find_ci(path, path + pn, "/drm/");
      if (d) { c->dead = true; continue; }
    }
    if (!c->kind) c->kind = vsrc_generic_kind(c->url, c->mime);
    if (!c->kind) {
      if (c->trust < GT_TAG) { c->dead = true; continue; }
      c->kind = GEN_UNKNOWN;                         /* declared, container unknown: probed later */
    }
    if ((c->kind == GEN_OGG || c->kind == GEN_OPUS) && (c->in_video || mime_kind(c->mime) == GEN_OGV))
      c->kind = GEN_OGV;
    /* named without a media extension, or the page's own path again (Commons'
    ** og:video is ".../File:X.webm?embedplayer=true", a player page) */
    c->probe = c->named && c->kind != GEN_UNKNOWN && (!ext_kind(c->url) || same_path(c->url, page_url));
    if (kind_audio(c->kind) || (c->kind != GEN_HLS && !fm_strnicmp(c->mime, "audio/", 6))) c->audio = true;
    if (c->w > 0 && c->h > 0) c->h = FM_MIN(c->w, c->h);
    if (c->h <= 0 || c->h > 8640) {
      c->h = vsrc_generic_height_hint(c->hint);
      if (!c->h) c->h = vsrc_generic_height_hint(c->url);
    }
    if (c->audio) c->h = 0;
  }
  /* twins (same URL, or http vs https): the better one stays */
  for (int i = 0; i < s->n; i++) {
    Cand *a = &s->c[i];
    if (a->dead) continue;
    for (int j = i + 1; j < s->n; j++) {
      Cand *b = &s->c[j];
      if (b->dead || strcmp(no_scheme(a->url), no_scheme(b->url))) continue;
      if (b->trust > a->trust) a->trust = b->trust;
      if (!a->mime[0]) fm_strlcpy(a->mime, b->mime, sizeof a->mime);
      if (!a->h) a->h = b->h;
      if (a->kind == GEN_UNKNOWN && b->kind != GEN_UNKNOWN) a->kind = b->kind;
      if (!fm_strnicmp(b->url, "https:", 6)) fm_strlcpy(a->url, b->url, sizeof a->url);
      b->dead = true;
    }
  }
  if (s->thumb[0] && vsrc_generic_join(base, s->thumb, abs, sizeof abs)) fm_strlcpy(s->thumb, abs, sizeof s->thumb);
  else s->thumb[0] = 0;
  if (s->embed[0] && vsrc_generic_join(base, s->embed, abs, sizeof abs)) fm_strlcpy(s->embed, abs, sizeof s->embed);
  else s->embed[0] = 0;
  if (s->embed[0] && !strcmp(no_scheme(s->embed), no_scheme(page_url))) s->embed[0] = 0;
}

static Scan *scan_page(const char *html, size_t len, const char *page_url) {
  Scan *s = (Scan *)fm_calloc(1, sizeof *s);
  s->og_group = s->tw_group = -1;
  if (html && len) {
    scan_tags(s, html, len);
    scan_raw(s, html, len);
  }
  finalize(s, page_url);
  return s;
}

/* ---- choosing ----------------------------------------------------------------------- */

static const char *codec_hint(const char *s) {
  static const struct { const char *pat, *name; } kC[] = {
    { "avc1", "H.264" }, { "avc3", "H.264" }, { "h264", "H.264" }, { "x264", "H.264" }, { "vp09", "VP9" },
    { "vp9", "VP9" }, { "vp08", "VP8" }, { "vp8", "VP8" }, { "av01", "AV1" }, { "hev1", "H.265" },
    { "hvc1", "H.265" }, { "hevc", "H.265" }, { "h265", "H.265" },
  };
  for (int i = 0; s && i < FM_COUNT(kC); i++)
    if (fm_stristr(s, kC[i].pat)) return kC[i].name;
  return NULL;
}

static const char *cand_codec(const Cand *c) {
  const char *cs = fm_stristr(c->mime, "codecs");
  const char *h = cs ? codec_hint(cs) : NULL;
  if (!h && c->kind != GEN_HLS) {                  /* a playlist's URL says nothing about its streams */
    const char *path = strstr(c->url, "://");
    path = path ? path + 3 + strcspn(path + 3, "/?#") : c->url;  /* not the host, not the query */
    char pt[VSRC_QURL];
    fm_snprintf(pt, sizeof pt, "%.*s", (int)strcspn(path, "?#"), path);
    h = codec_hint(pt);
  }
  if (h) return h;
  switch (c->kind) {
  case GEN_MP4: return "MP4";
  case GEN_WEBM: return "WebM";
  case GEN_MKV: return "MKV";
  case GEN_HLS: return "HLS";
  case GEN_TS: return "MPEG-TS";
  case GEN_OGV: case GEN_OGG: return "Ogg";
  case GEN_MP3: return "MP3";
  case GEN_M4A: return "AAC";
  case GEN_OPUS: return "Opus";
  default: return "?";
  }
}

/* Does it stream with the decoders c describes? (HLS: the streaming core
** reads playlists as of 2026-10-09; the segments are H.264/AAC, so the same
** decoders as MP4.) */
static bool cand_plays(const Cand *c, const FmVsrcConf *cf) {
  if (c->kind == GEN_UNKNOWN || c->kind == GEN_NONE) return false;
  if (cf->have_ffmpeg_libs) return true;
  const char *codec = cand_codec(c);
  switch (c->kind) {
  case GEN_WEBM:                       /* MediaCodec (os_dash) also does VP8 */
    return strcmp(codec, "AV1") && strcmp(codec, "H.264") && (cf->os_dash || strcmp(codec, "VP8"));
  case GEN_MP4: case GEN_HLS: case GEN_MP3: case GEN_M4A: return cf->os_mp4;
  case GEN_MKV: case GEN_TS: case GEN_OGG: case GEN_OPUS:
    return cf->os_dash;                /* Android's extractors read these; elsewhere FFmpeg */
  default: return false;               /* other types: FFmpeg */
  }
}

/* better first: pictures before sound, bigger, playable, more trusted, page order */
static bool cand_before(const Cand *a, const Cand *b, bool pa, bool pb) {
  if (a->audio != b->audio) return !a->audio;
  if (a->h != b->h) return a->h > b->h;
  if (pa != pb) return pa;
  if (a->trust != b->trust) return a->trust > b->trust;
  return a < b;
}

static FmErr build_stream(Scan *s, const FmVsrcConf *cf, FmVsrcStream *out, char *err, size_t errcap) {
  int ord[GEN_MAX_CAND], n = 0;
  bool play[GEN_MAX_CAND];
  bool declared_plays = false;
  out->nq = 0;
  out->cur = -1;
  for (int i = 0; i < s->n; i++) {
    play[i] = !s->c[i].dead && cand_plays(&s->c[i], cf);
    if (play[i] && s->c[i].trust >= GT_PRELOAD) declared_plays = true;
  }
  for (int i = 0; i < s->n; i++)
    if (!s->c[i].dead && (s->c[i].trust >= GT_PRELOAD || !declared_plays)) ord[n++] = i;
  for (int a = 1; a < n; a++)
    for (int b = a; b > 0 && cand_before(&s->c[ord[b]], &s->c[ord[b - 1]], play[ord[b]], play[ord[b - 1]]); b--) {
      int t = ord[b];
      ord[b] = ord[b - 1];
      ord[b - 1] = t;
    }
  if (n > VSRC_QMAX) n = VSRC_QMAX;
  for (int i = 0; i < n; i++) {
    const Cand *c = &s->c[ord[i]];
    FmVsrcQuality *q = &out->q[out->nq++];
    memset(q, 0, sizeof *q);
    const char *codec = cand_codec(c);
    char lb[16];
    if (c->audio) fm_strlcpy(lb, "Audio only", sizeof lb);
    else if (c->h > 0) fm_snprintf(lb, sizeof lb, "%dp", c->h);
    else fm_strlcpy(lb, c->kind == GEN_HLS ? "Auto" : "Video", sizeof lb);
    fm_strlcpy(q->label, lb, sizeof q->label);
    for (int k = 0, dup = 2; k < out->nq - 1; k++)              /* same label twice: tell them apart */
      if (!strcmp(out->q[k].label, q->label)) {
        if (dup == 2 && strcmp(codec, "?")) fm_snprintf(q->label, sizeof q->label, "%s %s", lb, codec);
        else fm_snprintf(q->label, sizeof q->label, "%s %d", lb, dup);
        dup++;
        k = -1;
      }
    q->height = c->h;
    fm_strlcpy(q->codec, codec, sizeof q->codec);
    q->playable = play[ord[i]];
    q->needs_ffmpeg = !q->playable && !cf->have_ffmpeg_libs && c->kind != GEN_UNKNOWN;
    q->muxed = !c->audio;
    q->audio_only = c->audio;
    fm_strlcpy(q->url, c->url, sizeof q->url);
  }
  /* the default: the biggest playable picture within max_height, else sound */
  int H = cf->max_height >= 144 && cf->max_height <= 4320 ? cf->max_height : 720;
  int pick = -1, last = -1, snd = -1;
  for (int i = 0; i < out->nq; i++) {
    if (!out->q[i].playable) continue;
    if (out->q[i].audio_only) { if (snd < 0) snd = i; continue; }
    last = i;
    if (pick < 0 && out->q[i].height <= H) pick = i;
  }
  if (pick < 0) pick = last >= 0 ? last : snd;
  if (pick >= 0) {
    const Cand *c = &s->c[ord[pick]];
    out->cur = pick;
    fm_strlcpy(out->video, c->url, sizeof out->video);
    out->audio[0] = 0;
    out->headers[0] = 0;
    out->local = false;
    out->height = c->h;
    out->width = c->w > 0 && c->w != c->h ? c->w : 0;
  }
  if (s->dur > 0) out->duration = s->dur;
  if (pick >= 0) return FM_OK;
  if (!out->nq) {
    fm_strlcpy(err, "No video found on this page", errcap);
    return FM_ERR_NOT_FOUND;
  }
  const FmVsrcQuality *q = &out->q[0];
  if (s->c[ord[0]].kind == GEN_UNKNOWN)
    fm_strlcpy(err, "The page names a video in a format this app could not identify", errcap);
  else if (!cf->have_ffmpeg_libs && (s->c[ord[0]].kind == GEN_MP4 || s->c[ord[0]].kind == GEN_HLS))
    fm_snprintf(err, errcap, "The video on this page is %s; it plays with the system video decoders or FFmpeg, "
                "neither is available here", q->codec);
  else fm_snprintf(err, errcap, "The video on this page is %s; it plays once the FFmpeg libraries are installed",
                   q->codec);
  return FM_ERR_UNSUPPORTED;
}

static void fill_item(const Scan *s, const char *page_url, FmVsrcItem *item, bool keep) {
  u32 h = 2166136261u;
  for (const char *p = page_url; *p; p++) h = (h ^ (u8)*p) * 16777619u;
  if (!keep || !item->id[0]) fm_snprintf(item->id, sizeof item->id, "g%08x", h);
  if (!item->page[0]) fm_strlcpy(item->page, page_url, sizeof item->page);
  if (s->title[0] && (!keep || !item->title[0])) fm_strlcpy(item->title, s->title, sizeof item->title);
  if (s->thumb[0] && strlen(s->thumb) < sizeof item->thumb && (!keep || !item->thumb[0]))
    fm_strlcpy(item->thumb, s->thumb, sizeof item->thumb);
  if (s->dur > 0 && (!keep || item->duration <= 0)) item->duration = s->dur;
}

/* a title from the URL when the page has none: the file name, else the host */
static void url_title(const char *url, char *out, size_t cap) {
  char host[128], seg[256];
  host_of(url, host, sizeof host);
  const char *path = strstr(url, "://");
  path = path ? path + 3 + strcspn(path + 3, "/?#") : url;
  size_t pn = strcspn(path, "?#");
  while (pn && path[pn - 1] == '/') pn--;
  size_t st = pn;
  while (st && path[st - 1] != '/') st--;
  size_t o = 0;
  for (size_t i = st; i < pn && o + 1 < sizeof seg; i++) {        /* percent-decoded */
    u32 v;
    if (path[i] == '%' && i + 2 < pn && hex_n(path + i + 1, 2, &v) && v >= 0x20) { seg[o++] = (char)v; i += 2; }
    else seg[o++] = path[i] == '_' ? ' ' : path[i];
  }
  seg[o] = 0;
  fm_strlcpy(out, seg[0] ? seg : host, cap);
}

FmErr vsrc_generic_parse(const char *html, size_t len, const char *page_url, const FmVsrcConf *c,
                         FmVsrcItem *item, FmVsrcStream *out, char *embed, size_t embedcap) {
  char err[256];
  if (embedcap) embed[0] = 0;
  memset(out, 0, sizeof *out);
  out->cur = -1;
  Scan *s = scan_page(html, len, page_url ? page_url : "");
  if (item) {
    memset(item, 0, sizeof *item);
    item->views = -1;
    if (!s->title[0] && page_url) url_title(page_url, s->title, sizeof s->title);
    fill_item(s, page_url ? page_url : "", item, false);
  }
  FmErr e = build_stream(s, c, out, err, sizeof err);
  if (embedcap && s->embed[0]) fm_strlcpy(embed, s->embed, embedcap);
  fm_free(s);
  return e;
}

/* ---- network ------------------------------------------------------------------------ */

typedef struct Fetch {
  const char *url;
  u8 *buf;
  size_t len, cap, max;
  int status;
  char type[96];
  int kind;               /* media kind of the reply; GEN_NONE = a page */
  bool decided, stopped;
} Fetch;

static void fetch_head(void *user, const FmNetResp *r) {
  Fetch *f = (Fetch *)user;
  f->status = r->status;
  fm_strlcpy(f->type, r->type, sizeof f->type);
}

static void fetch_decide(Fetch *f) {
  bool html = looks_html(f->buf, f->len) || fm_stristr(f->type, "html") || fm_stristr(f->type, "xml");
  int k = sniff_kind(f->buf, f->len);
  if (!k && !html) k = mime_kind(f->type);
  if (!k && !html) k = ext_kind(f->url);
  f->kind = k;
  f->decided = true;
}

static bool fetch_data(void *user, const u8 *p, size_t n) {
  Fetch *f = (Fetch *)user;
  if (f->status >= 400) { f->stopped = true; return false; }
  size_t take = FM_MIN(n, f->max - f->len);
  if (f->len + take + 1 > f->cap) {
    size_t nc = f->cap ? f->cap * 2 : 65536;
    while (nc < f->len + take + 1) nc *= 2;
    if (nc > f->max + 1) nc = f->max + 1;
    f->buf = (u8 *)fm_realloc(f->buf, nc);
    f->cap = nc;
  }
  memcpy(f->buf + f->len, p, take);
  f->len += take;
  f->buf[f->len] = 0;
  if (!f->decided && f->len >= GEN_SNIFF) {
    fetch_decide(f);
    if (f->kind || f->max <= GEN_SNIFF * 8) { f->stopped = true; return false; }   /* media, or a probe */
  }
  if (f->len >= f->max) { f->stopped = true; return false; }
  return true;
}

/* GET url; a page body is kept up to `max` bytes, media stops after the sniff */
static FmErr gen_fetch(const char *url, size_t max, Fetch *f, char *err, size_t errcap, volatile int *cancel) {
  static const char kHdr[] = "Accept: text/html,application/xhtml+xml,*/*;q=0.8\r\nAccept-Language: en-US,en;q=0.8\r\n";
  memset(f, 0, sizeof *f);
  f->url = url;
  f->max = max;
  FmNetResp r;
  memset(&r, 0, sizeof r);
  FmErr e = net_get_stream(url, kHdr, fetch_head, fetch_data, f, &r, cancel);
  if (cancel && *cancel) e = FM_ERR_CANCEL;
  else if (f->stopped) e = FM_OK;
  if (!f->status) f->status = r.status;
  if (!f->type[0]) fm_strlcpy(f->type, r.type, sizeof f->type);
  if (e == FM_ERR_CANCEL) fm_strlcpy(err, "Cancelled", errcap);
  else if (e != FM_OK) fm_snprintf(err, errcap, "Network error: %s", r.error[0] ? r.error : fm_err_str(e));
  net_resp_free(&r);
  if (e == FM_OK && (f->status < 200 || f->status >= 400)) {
    char host[128];
    vsrc_http_error(host_of(url, host, sizeof host), f->status, err, errcap);
    e = FM_ERR_IO;
  }
  if (e != FM_OK) {
    fm_free(f->buf);
    f->buf = NULL;
    return e;
  }
  if (!f->decided) fetch_decide(f);
  return FM_OK;
}

/* Declared URLs without a media extension (unknown type, or a meta tag's
** type that may describe the player page rather than the URL): their
** headers and first bytes tell. Measured: Commons' og:video is a player
** page declared "video/webm". */
static void probe_unknown(Scan *s, volatile int *cancel) {
  int done = 0;
  for (int i = 0; i < s->n && done < GEN_PROBE_MAX; i++) {
    Cand *c = &s->c[i];
    if (c->dead || (c->kind != GEN_UNKNOWN && !c->probe)) continue;
    if (cancel && *cancel) return;
    done++;
    Fetch f;
    char err[160];
    if (gen_fetch(c->url, GEN_SNIFF, &f, err, sizeof err, cancel) != FM_OK) {
      if (c->kind == GEN_UNKNOWN) c->dead = true;               /* a typed one keeps its declared type */
      continue;
    }
    if (f.kind) {
      c->kind = f.kind;
      if (kind_audio(f.kind) || (f.kind != GEN_HLS && !fm_strnicmp(f.type, "audio/", 6))) { c->audio = true; c->h = 0; }
    } else {
      if (looks_html(f.buf, f.len) || fm_stristr(f.type, "html")) set_embed(s, c->url, c->trust >= GT_LD ? 5 : 4);
      c->dead = true;
    }
    fm_free(f.buf);
  }
}

/* resolve and probe: one page, plus one player page it points to */
static FmErr gen_run(const FmVsrcConf *cf, const char *page_url, FmVsrcItem *item, FmVsrcStream *out, char *err,
                     size_t errcap, volatile int *cancel) {
  FmVsrcConf def;
  if (!cf) {
    memset(&def, 0, sizeof def);
    def.max_height = 720;
    cf = &def;
  }
  memset(out, 0, sizeof *out);
  out->cur = -1;
  char url[VSRC_QURL];
  const char *u = page_url ? page_url : "";
  while (is_space(*u)) u++;
  if (*u && !is_http(u) && strchr(u, '.') && !strchr(u, ' ') && !strstr(u, "://"))
    fm_snprintf(url, sizeof url, "https://%s", u);                /* "www.site.com/..." without the scheme */
  else fm_strlcpy(url, u, sizeof url);
  size_t n = strlen(url);
  while (n && is_space(url[n - 1])) url[--n] = 0;
  if (!vsrc_url_ok(url)) {
    fm_strlcpy(err, "Paste a link that starts with http:// or https://", errcap);
    return FM_ERR_UNSUPPORTED;
  }
  if (!vsrc_need_net(err, errcap)) return FM_ERR_UNSUPPORTED;
  char first[VSRC_QURL];
  fm_strlcpy(first, url, sizeof first);
  FmErr e = FM_ERR_NOT_FOUND;
  for (int depth = 0; depth < 2; depth++) {
    Fetch f;
    e = gen_fetch(url, GEN_MAX_PAGE, &f, err, errcap, cancel);
    if (e != FM_OK) return e;
    Scan *s;
    if (f.kind) {                                    /* a link straight to the media */
      s = (Scan *)fm_calloc(1, sizeof *s);
      Cand *c = add_cand(s, url, GT_LD, NULL);
      if (c) {
        c->kind = f.kind;
        c->audio = kind_audio(f.kind) || (f.kind != GEN_HLS && !fm_strnicmp(f.type, "audio/", 6));
        if (!fm_strnicmp(f.type, "video/", 6) || !fm_strnicmp(f.type, "audio/", 6))
          fm_strlcpy(c->mime, f.type, sizeof c->mime);
      }
      finalize(s, url);
      if (depth == 0) url_title(url, s->title, sizeof s->title);
    } else {
      s = scan_page((const char *)f.buf, f.len, url);
      if (!s->title[0] && depth == 0) url_title(url, s->title, sizeof s->title);
      probe_unknown(s, cancel);
    }
    fm_free(f.buf);
    if (cancel && *cancel) {
      fm_free(s);
      fm_strlcpy(err, "Cancelled", errcap);
      return FM_ERR_CANCEL;
    }
    if (item) fill_item(s, first, item, depth > 0);
    double dur = out->duration;
    e = build_stream(s, cf, out, err, errcap);
    if (e == FM_OK && errcap) err[0] = 0;                        /* the first page's "No video found" */
    if (out->duration <= 0 && dur > 0) out->duration = dur;
    bool again = e == FM_ERR_NOT_FOUND && depth == 0 && s->embed[0] && !is_youtube(s->embed);
    if (again) fm_strlcpy(url, s->embed, sizeof url);
    fm_free(s);
    if (!again) break;
  }
  return e;
}

FmErr vsrc_generic_resolve(const FmVsrcConf *c, const char *page_url, FmVsrcStream *out, char *err,
                           size_t errcap, volatile int *cancel) {
  if (errcap) err[0] = 0;
  return gen_run(c, page_url, NULL, out, err, errcap, cancel);
}

FmErr vsrc_generic_probe(const FmVsrcConf *c, const char *page_url, FmVsrcItem *item, char *err, size_t errcap,
                         volatile int *cancel) {
  if (errcap) err[0] = 0;
  memset(item, 0, sizeof *item);
  item->views = -1;
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
  FmErr e = gen_run(c, page_url, item, st, err, errcap, cancel);
  if (e == FM_ERR_UNSUPPORTED && st->nq > 0) e = FM_OK;        /* a video, just not playable here */
  fm_free(st);
  return e;
}
