/* fasrc.c -- online audio sources: registry, settings snapshot, codec and
** file-name helpers, the stream lookup and downloads (see fasrc.h).
**
** Design decisions:
**   - A page is one allocation: search pages reserve ASRC_MAX_ITEMS, the
**     children of a podcast or album reserve what the parser counted (at
**     most ASRC_MAX_CHILDREN) before adding; parsers stop at the cap, so
**     the realloc path is only a safety net for other callers.
**   - Playability is decided once, here: asrc_stream refuses an AAC / Opus /
**     Vorbis / WMA stream when FFmpeg is missing with a sentence naming the
**     codec and the fix ("This episode uses AAC -- install FFmpeg to play
**     it"), instead of a decoder error after a slow connect. MP3, FLAC and
**     WAV play built in; an unknown codec is tried (the decoder sniffs).
**   - Stations always go through their source's resolve(), even when the
**     search already returned an address: Radio Browser asks apps to count
**     a click when a station is played, and its click call also returns the
**     freshest stream address.
**   - Downloads stream to "<name>.<ms>.download" first; the extension comes
**     from the first bytes (else the Content-Type), so an HTML error page
**     answered with HTTP 200 never lands on disk as "song.mp3". The final
**     "<artist> - <title>.<ext>" is picked and renamed under a lock (the
**     rename fails rather than overwrite), so parallel downloads of two
**     songs with the same name both succeed, as "(2)".
**   - Licensed music (Jamendo, Freesound, archive.org, any CC licence) gets
**     a "<same name>.txt" with title, artist, licence and page, so the credit
**     most licences ask for travels with the file, as the photos do.
*/
#include "fasrc.h"
#include "fasrc_int.h"
#include "fconf.h"
#include "fplat.h"
#include "fsdl.h"
#include "fdec_vid.h"

#ifdef FM_WIN
#  include "fwin.h"
#  ifdef __TINYC__
WINBASEAPI int WINAPI GetLocaleInfoA(DWORD, DWORD, LPSTR, int);   /* missing from tcc's winnls.h */
#  endif
#  ifndef LOCALE_USER_DEFAULT
#    define LOCALE_USER_DEFAULT 0x0400
#  endif
#  ifndef LOCALE_SISO3166CTRYNAME
#    define LOCALE_SISO3166CTRYNAME 0x5A
#  endif
#endif

static void utf8_cut_tail(char *s);

/* ---- registry ----------------------------------------------------------------- */

static const FmAsrc *const kSources[] = {
  &g_asrc_radio, &g_asrc_audius, &g_asrc_archive, &g_asrc_podcasts, &g_asrc_jamendo, &g_asrc_freesound,
};

int asrc_count(void) { return FM_COUNT(kSources); }

const FmAsrc *asrc_at(int i) { return i >= 0 && i < FM_COUNT(kSources) ? kSources[i] : NULL; }

const FmAsrc *asrc_find(const char *key) {
  if (!key) return NULL;
  for (int i = 0; i < FM_COUNT(kSources); i++)
    if (!strcmp(kSources[i]->key, key)) return kSources[i];
  return NULL;
}

/* ---- pages -------------------------------------------------------------------- */

void asrc_page_free(FmAsrcPage *p) {
  if (!p) return;
  fm_free(p->items);
  memset(p, 0, sizeof *p);
}

void asrc_page_reserve(FmAsrcPage *p, int n) {
  if (p->items || n <= 0) return;
  p->cap = n;
  p->items = (FmAsrcItem *)fm_alloc((size_t)n * sizeof *p->items);
}

FmAsrcItem *asrc_page_add(FmAsrcPage *p) {
  if (p->count == p->cap) {
    p->cap = p->cap ? p->cap * 2 : ASRC_MAX_ITEMS;
    p->items = (FmAsrcItem *)fm_realloc(p->items, (size_t)p->cap * sizeof *p->items);
  }
  FmAsrcItem *it = &p->items[p->count++];
  memset(it, 0, sizeof *it);
  return it;
}

FmAsrcItem *asrc_item_new(FmAsrcPage *p, const char *source, int kind) {
  FmAsrcItem *it = asrc_page_add(p);
  fm_strlcpy(it->source, source, sizeof it->source);
  it->kind = kind;
  it->plays = -1;
  return it;
}

/* ---- settings ----------------------------------------------------------------- */

/* keys are pasted: drop surrounding blanks */
static void copy_key(char *out, const char *k, size_t cap) {
  while (*k == ' ' || *k == '\t') k++;
  fm_strlcpy(out, k, cap);
  size_t n = strlen(out);
  while (n && (u8)out[n - 1] <= ' ') out[--n] = 0;
}

static void user_region(char *out, size_t cap) {
  char buf[16];
  buf[0] = 0;
#ifdef FM_WIN
  if (!GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_SISO3166CTRYNAME, buf, (int)sizeof buf)) buf[0] = 0;
#else
  const char *l = getenv("LC_ALL");
  if (!l || !*l) l = getenv("LC_MESSAGES");
  if (!l || !*l) l = getenv("LANG");
  const char *u = l ? strchr(l, '_') : NULL;
  if (u) fm_snprintf(buf, sizeof buf, "%.2s", u + 1);
#endif
  if (strlen(buf) == 2 && buf[0] >= 'A' && buf[0] <= 'Z' && buf[1] >= 'A' && buf[1] <= 'Z')
    fm_strlcpy(out, buf, cap);
  else if (cap)
    out[0] = 0;
}

void asrc_conf_snapshot(FmAsrcConf *c) {
  memset(c, 0, sizeof *c);
  copy_key(c->key_jamendo, conf.key_jamendo, sizeof c->key_jamendo);
  copy_key(c->key_freesound, conf.key_freesound, sizeof c->key_freesound);
  char dir[FM_PATH_MAX];
  if (plat_place(PLACE_CACHE, dir, sizeof dir) || plat_place(PLACE_TEMP, dir, sizeof dir))
    fm_path_join(c->cache_dir, sizeof c->cache_dir, dir, "online-audio");
  if (conf.online_dl_dir[0]) fm_strlcpy(c->download_dir, conf.online_dl_dir, sizeof c->download_dir);
  else if (!plat_place(PLACE_DOWNLOADS, c->download_dir, sizeof c->download_dir))
    plat_place(PLACE_HOME, c->download_dir, sizeof c->download_dir);
  c->have_ffmpeg = ff_available();
  c->safe_search = conf.online_safe;
  user_region(c->country, sizeof c->country);
}

/* ---- network ------------------------------------------------------------------ */

bool asrc_need_net(char *err, size_t cap) {
  if (net_available()) return true;
#if defined(FM_ANDROID) || defined(FM_WEB)
  fm_strlcpy(err, "Online audio is not available on this platform yet", cap);
#else
  fm_snprintf(err, cap, "No internet support on this system (%s)", net_backend());
#endif
  return false;
}

FmErr asrc_http_get(const char *url, const char *headers, size_t max, FmNetResp *r, char *err, size_t errcap,
                    volatile int *cancel) {
  memset(r, 0, sizeof *r);
  if (!asrc_need_net(err, errcap)) return FM_ERR_UNSUPPORTED;
  FmErr e = net_get(url, headers, max, r, cancel);
  if (e == FM_ERR_CANCEL) { fm_strlcpy(err, "Cancelled", errcap); net_resp_free(r); return e; }
  if (e != FM_OK) {
    if (e == FM_ERR_FULL || e == FM_ERR_NOMEM)
      fm_strlcpy(err, "The reply was too big to read", errcap);
    else if (strstr(r->error, "12175") || fm_stristr(r->error, "certificate") || fm_stristr(r->error, "SSL"))
      /* WinHTTP 12175 = SECURE_FAILURE: seen when a firewall re-signs HTTPS with its own CA */
      fm_snprintf(err, errcap, "No secure connection to the site \xE2\x80\x94 a firewall or antivirus may be "
                  "intercepting HTTPS (%s)", r->error);
    else
      fm_snprintf(err, errcap, "Network error \xE2\x80\x94 check the internet connection (%s)",
                  r->error[0] ? r->error : fm_err_str(e));
    net_resp_free(r);
    return e;
  }
  return FM_OK;
}

/* ---- text --------------------------------------------------------------------- */

void asrc_text(const char *s, char *out, size_t cap) {
  char tmp[1200];
  if (!cap) return;
  vsrc_html_unescape(s ? s : "", tmp, sizeof tmp);
  size_t o = 0;
  bool blank = true;
  for (const char *p = tmp; *p && o + 1 < cap; p++) {
    if ((u8)*p <= ' ') {
      if (!blank) { out[o++] = ' '; blank = true; }
    } else {
      out[o++] = *p;
      blank = false;
    }
  }
  while (o > 0 && out[o - 1] == ' ') o--;
  out[o] = 0;
  utf8_cut_tail(out);
}

/* ---- codecs ------------------------------------------------------------------- */

static bool mime_is(const char *mime, const char *want) {
  size_t n = strlen(want);
  return !fm_strnicmp(mime, want, n) && (mime[n] == 0 || mime[n] == ';' || mime[n] == ' ');
}

static const struct { const char *mime, *codec, *ext; } kMimes[] = {
  { "audio/mpeg", "MP3", ".mp3" },   { "audio/mp3", "MP3", ".mp3" },      { "audio/mpeg3", "MP3", ".mp3" },
  { "audio/x-mpeg", "MP3", ".mp3" }, { "audio/x-mp3", "MP3", ".mp3" },    { "audio/flac", "FLAC", ".flac" },
  { "audio/x-flac", "FLAC", ".flac" }, { "audio/ogg", "OGG", ".ogg" },    { "application/ogg", "OGG", ".ogg" },
  { "audio/vorbis", "OGG", ".ogg" }, { "audio/opus", "OPUS", ".opus" },  { "audio/wav", "WAV", ".wav" },
  { "audio/x-wav", "WAV", ".wav" },  { "audio/wave", "WAV", ".wav" },     { "audio/vnd.wave", "WAV", ".wav" },
  { "audio/mp4", "AAC", ".m4a" },    { "audio/x-m4a", "AAC", ".m4a" },    { "audio/m4a", "AAC", ".m4a" },
  { "audio/aac", "AAC", ".aac" },    { "audio/aacp", "AAC", ".aac" },     { "audio/x-aac", "AAC", ".aac" },
  { "audio/x-ms-wma", "WMA", ".wma" }, { "audio/webm", "OPUS", ".webm" }, { "audio/x-aiff", "AIFF", ".aif" },
  { "audio/aiff", "AIFF", ".aif" },
};

void asrc_codec_from_mime(const char *mime, char *out, size_t cap) {
  if (cap) out[0] = 0;
  if (!mime) return;
  for (int i = 0; i < FM_COUNT(kMimes); i++)
    if (mime_is(mime, kMimes[i].mime)) { fm_strlcpy(out, kMimes[i].codec, cap); return; }
}

const char *asrc_mime_ext(const char *type) {
  if (!type) return "";
  for (int i = 0; i < FM_COUNT(kMimes); i++)
    if (mime_is(type, kMimes[i].mime)) return kMimes[i].ext;
  return "";
}

void asrc_codec_from_ext(const char *name, char *out, size_t cap) {
  static const struct { const char *ext, *codec; } kExt[] = {
    { ".mp3", "MP3" }, { ".mp2", "MP3" }, { ".flac", "FLAC" }, { ".ogg", "OGG" }, { ".oga", "OGG" },
    { ".opus", "OPUS" }, { ".wav", "WAV" }, { ".m4a", "AAC" }, { ".m4b", "AAC" }, { ".mp4", "AAC" },
    { ".aac", "AAC" }, { ".wma", "WMA" }, { ".aif", "AIFF" }, { ".aiff", "AIFF" },
  };
  char ext[16];
  if (cap) out[0] = 0;
  psrc_url_ext(name, ext, sizeof ext);
  if (!ext[0]) {
    const char *x = name ? fm_path_ext(name) : "";
    fm_strlcpy(ext, x, sizeof ext);
  }
  for (int i = 0; i < FM_COUNT(kExt); i++)
    if (!fm_stricmp(ext, kExt[i].ext)) { fm_strlcpy(out, kExt[i].codec, cap); return; }
}

void asrc_codec_norm(const char *codec, char *out, size_t cap) {
  if (cap) out[0] = 0;
  if (!codec || !*codec || !fm_stricmp(codec, "UNKNOWN")) return;
  if (!fm_strnicmp(codec, "AAC", 3)) fm_strlcpy(out, "AAC", cap);           /* "AAC+", "AAC+,H.264" */
  else if (!fm_stricmp(codec, "MP3") || !fm_stricmp(codec, "MPEG")) fm_strlcpy(out, "MP3", cap);
  else if (!fm_stricmp(codec, "OGG") || !fm_stricmp(codec, "VORBIS")) fm_strlcpy(out, "OGG", cap);
  else if (!fm_stricmp(codec, "FLAC")) fm_strlcpy(out, "FLAC", cap);
  else if (!fm_stricmp(codec, "OPUS")) fm_strlcpy(out, "OPUS", cap);
  else if (!fm_stricmp(codec, "WMA")) fm_strlcpy(out, "WMA", cap);
  else {
    size_t o = 0;                                  /* keep it short and printable */
    for (const char *s = codec; *s && o + 1 < cap && o < 8; s++)
      if ((*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z'))
        out[o++] = (char)(*s >= 'a' && *s <= 'z' ? *s - 'a' + 'A' : *s);
    out[o] = 0;
  }
}

bool asrc_codec_builtin(const char *codec) {
  return !codec || !*codec || !fm_stricmp(codec, "MP3") || !fm_stricmp(codec, "FLAC") ||
         !fm_stricmp(codec, "WAV");
}

bool asrc_codec_blocked(const FmAsrcConf *c, const FmAsrcItem *item, const char *codec, char *err, size_t cap) {
  if (c->have_ffmpeg || asrc_codec_builtin(codec)) return false;
  const char *what = !item ? "This stream" : item->kind == AITEM_STATION ? "This station"
                   : !strcmp(item->source, "podcasts") ? "This episode" : "This track";
  fm_snprintf(err, cap, "%s uses %s \xE2\x80\x94 install FFmpeg to play it", what, codec);
  return true;
}

/* "http://creativecommons.org/licenses/by-nc-sa/3.0/" -> "CC BY-NC-SA 3.0" */
void asrc_cc_license(const char *url, char *out, size_t cap) {
  if (cap) out[0] = 0;
  if (!url || !*url) return;
  const char *p = strstr(url, "creativecommons.org/");
  if (!p) {                                        /* already a name ("Attribution", "CC BY 4.0") */
    if (strncmp(url, "http", 4)) psrc_copy(out, url, cap);
    return;
  }
  p += 20;
  if (!strncmp(p, "publicdomain/zero", 17)) { fm_strlcpy(out, "CC0 1.0", cap); return; }
  if (!strncmp(p, "publicdomain/mark", 17)) { fm_strlcpy(out, "Public domain mark", cap); return; }
  if (!strncmp(p, "publicdomain", 12)) { fm_strlcpy(out, "Public domain", cap); return; }
  if (strncmp(p, "licenses/", 9)) return;
  p += 9;
  char code[24], ver[8];
  size_t o = 0;
  for (; *p && *p != '/' && o + 1 < sizeof code; p++)
    code[o++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 'a' + 'A' : *p);
  code[o] = 0;
  o = 0;
  if (*p == '/') p++;
  for (; *p && *p != '/' && o + 1 < sizeof ver && ((*p >= '0' && *p <= '9') || *p == '.'); p++) ver[o++] = *p;
  ver[o] = 0;
  if (!code[0]) return;
  if (ver[0]) fm_snprintf(out, cap, "CC %s %s", code, ver);
  else fm_snprintf(out, cap, "CC %s", code);
}

/* ---- dates and durations ------------------------------------------------------ */

void asrc_rfc822_date(const char *s, char *out, size_t cap) {
  static const char kMon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  if (!cap) return;
  out[0] = 0;
  if (!s) return;
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
  vsrc_iso_date(s, out, cap);                      /* some feeds write ISO 8601 */
  if (out[0]) return;
  const char *comma = strchr(s, ',');
  if (comma && comma - s <= 10) s = comma + 1;     /* "Wed," / "Wednesday," */
  while (*s == ' ') s++;
  int d = 0, nd = 0;
  while (*s >= '0' && *s <= '9' && nd < 2) { d = d * 10 + (*s++ - '0'); nd++; }
  if (!nd || *s != ' ') return;
  s++;
  int m = -1;
  for (int i = 0; i < 12; i++)
    if (!fm_strnicmp(s, kMon + i * 3, 3)) m = i + 1;
  if (m < 0) return;
  s += 3;
  while (*s && *s != ' ') s++;                     /* "Sept", "October" */
  while (*s == ' ') s++;
  int y = 0, ny = 0;
  while (*s >= '0' && *s <= '9' && ny < 4) { y = y * 10 + (*s++ - '0'); ny++; }
  if (ny == 2) y += y < 70 ? 2000 : 1900;
  else if (ny != 4) return;
  if (d < 1 || d > 31 || y < 1900 || y > 2200) return;
  fm_snprintf(out, cap, "%04d-%02d-%02d", y, m, d);
}

double asrc_duration(const char *s) {
  char buf[32];
  if (!s) return 0;
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
  fm_strlcpy(buf, s, sizeof buf);
  size_t n = strlen(buf);
  while (n && (u8)buf[n - 1] <= ' ') buf[--n] = 0;
  double v = vsrc_clock_seconds(buf);
  return v > 0 && v < 1e7 ? v : 0;
}

/* ---- file names and credits --------------------------------------------------- */

/* Drops an incomplete UTF-8 sequence at the end (left by truncation). */
static void utf8_cut_tail(char *s) {
  size_t n = strlen(s), i = n;
  int back = 0;
  while (i > 0 && back < 4 && ((u8)s[i - 1] & 0xC0) == 0x80) { i--; back++; }
  if (i == 0) { if (back) s[0] = 0; return; }
  u8 lead = (u8)s[i - 1];
  int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
  if (lead >= 0x80 && need != back + 1) s[i - 1] = 0;
}

static void safe_part(const char *s, char *out, size_t max) {
  size_t o = 0;
  if (!max) return;
  for (; s && *s && o + 1 < max; s++) {
    u8 c = (u8)*s;
    if (c < 0x20 || c == 0x7F || strchr("<>:\"/\\|?*", c)) c = ' ';
    if (c == ' ' && (o == 0 || out[o - 1] == ' ')) continue;
    if (o == 0 && c == '.') continue;                  /* no hidden files on Unix */
    out[o++] = (char)c;
  }
  out[o] = 0;
  utf8_cut_tail(out);
  o = strlen(out);
  while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '.')) o--;
  out[o] = 0;
}

void asrc_file_base(const FmAsrcItem *item, char *out, size_t max) {
  char t[110], a[60];
  if (!max) return;
  safe_part(item ? item->artist : "", a, sizeof a);
  safe_part(item ? item->title : "", t, sizeof t);
  if (t[0] && a[0]) fm_snprintf(out, max, "%s - %s", a, t);
  else fm_strlcpy(out, t[0] ? t : a[0] ? a : "audio", max);
  utf8_cut_tail(out);
  size_t o = strlen(out);
  while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '.')) out[--o] = 0;
  if (!out[0]) fm_strlcpy(out, "audio", max);
  /* DOS device names are still reserved in every folder on Windows */
  static const char *const kDev[] = { "CON", "PRN", "AUX", "NUL" };
  size_t stem = strcspn(out, ". ");
  bool dev = false;
  for (int i = 0; i < FM_COUNT(kDev); i++)
    if (stem == 3 && !fm_strnicmp(out, kDev[i], 3)) dev = true;
  if (stem == 4 && (!fm_strnicmp(out, "COM", 3) || !fm_strnicmp(out, "LPT", 3)) && out[3] >= '0' &&
      out[3] <= '9')
    dev = true;
  if (dev && strlen(out) + 2 < max) {
    memmove(out + 1, out, strlen(out) + 1);
    out[0] = '_';
  }
}

const char *asrc_sniff_ext(const u8 *h, size_t n) {
  if (!h) return "";
  if (n >= 3 && !memcmp(h, "ID3", 3)) return ".mp3";
  if (n >= 4 && !memcmp(h, "fLaC", 4)) return ".flac";
  if (n >= 4 && !memcmp(h, "OggS", 4)) {
    for (size_t i = 4; i + 8 <= n && i < 96; i++)
      if (!memcmp(h + i, "OpusHead", 8)) return ".opus";
    return ".ogg";
  }
  if (n >= 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) return ".wav";
  if (n >= 12 && !memcmp(h, "FORM", 4) && (!memcmp(h + 8, "AIFF", 4) || !memcmp(h + 8, "AIFC", 4))) return ".aif";
  if (n >= 12 && !memcmp(h + 4, "ftyp", 4)) return ".m4a";
  if (n >= 8 && !memcmp(h, "\x30\x26\xB2\x75\x8E\x66\xCF\x11", 8)) return ".wma";
  if (n >= 3 && h[0] == 0xFF && (h[1] & 0xE0) == 0xE0) {
    if ((h[1] & 0x06) == 0) return (h[1] & 0xF6) == 0xF0 ? ".aac" : "";   /* ADTS */
    if ((h[2] & 0xF0) != 0xF0 && (h[1] & 0x18) != 0x08) return ".mp3";   /* MPEG audio frame */
  }
  return "";
}

bool asrc_wants_credit(const FmAsrcItem *item) {
  if (!item) return false;
  if (!strcmp(item->source, "jamendo") || !strcmp(item->source, "freesound") || !strcmp(item->source, "archive"))
    return true;
  return item->license[0] && fm_strnicmp(item->license, "All rights", 10) != 0;
}

void asrc_credit(const FmAsrcItem *item, const char *site, const char *file_url, char *out, size_t cap) {
  char date[16];
  vsrc_unix_date(plat_time_unix(), date, sizeof date);
  fm_snprintf(out, cap,
              "Title: %s\nArtist: %s\nAlbum: %s\nLicence: %s\nSource: %s\nPage: %s\nFile: %s\nSaved: %s by mmcfm\n"
              "\nCheck the licence on the page before reusing this recording; most need a credit line.\n",
              item->title[0] ? item->title : "(untitled)", item->artist[0] ? item->artist : "(unknown)",
              item->album[0] ? item->album : "-", item->license[0] ? item->license : "(see the page)",
              site ? site : item->source, item->page[0] ? item->page : "-",
              file_url && *file_url ? file_url : item->url[0] ? item->url : "-", date);
}

/* ---- streams ------------------------------------------------------------------ */

/* the stream without the FFmpeg check (downloads save any codec) */
static FmErr find_stream(const FmAsrcConf *c, const FmAsrcItem *item, FmAsrcStream *out, char *err, size_t errcap,
                         volatile int *cancel) {
  memset(out, 0, sizeof *out);
  if (errcap) err[0] = 0;
  if (!c || !item) { fm_strlcpy(err, "Nothing to play", errcap); return FM_ERR_NOT_FOUND; }
  if (item->kind == AITEM_PODCAST || item->kind == AITEM_ALBUM) {
    fm_strlcpy(err, item->kind == AITEM_PODCAST ? "Open the podcast to pick an episode"
                                                : "Open the album to pick a track", errcap);
    return FM_ERR_UNSUPPORTED;
  }
  const FmAsrc *src = asrc_find(item->source);
  FmErr e;
  if (src && src->resolve && (item->kind == AITEM_STATION || !item->url[0])) {
    e = src->resolve(c, item, out, err, errcap, cancel);
    if (e != FM_OK && !err[0]) fm_snprintf(err, errcap, "Could not find the stream (%s)", fm_err_str(e));
  } else if (item->url[0]) {
    if (!psrc_url_copy(item->url, out->url, sizeof out->url)) {
      fm_strlcpy(err, "This item has no usable address", errcap);
      return FM_ERR_NOT_FOUND;
    }
    out->live = item->kind == AITEM_STATION;
    e = FM_OK;
  } else {
    fm_strlcpy(err, "This item has no stream", errcap);
    return FM_ERR_NOT_FOUND;
  }
  if (e == FM_OK) {
    if (!out->codec[0]) fm_strlcpy(out->codec, item->codec, sizeof out->codec);
    if (!out->codec[0]) asrc_codec_from_ext(out->url, out->codec, sizeof out->codec);
    if (item->kind == AITEM_STATION) out->live = true;
  }
  return e;
}

FmErr asrc_stream(const FmAsrcConf *c, const FmAsrcItem *item, FmAsrcStream *out, char *err, size_t errcap,
                  volatile int *cancel) {
  FmErr e = find_stream(c, item, out, err, errcap, cancel);
  if (e != FM_OK) return e;
  if (asrc_codec_blocked(c, item, out->codec, err, errcap)) return FM_ERR_UNSUPPORTED;
  return FM_OK;
}

/* ---- downloads ---------------------------------------------------------------- */

static SDL_SpinLock g_dest_lock;

/* Picks "<base><ext>" or "<base> (N)<ext>" with no "<same>.txt" next to it
** and renames tmp there under the lock; txt gets the credit file's path. */
static FmErr place_file(const char *dir, const char *base, const char *ext, const char *tmp, char *dst, char *txt,
                        size_t cap) {
  char name[300], a[320], b[320];
  FmErr e = FM_ERR_EXISTS;
  SDL_AtomicLock(&g_dest_lock);
  for (int i = 1; i < 1000; i++) {
    if (i == 1) fm_snprintf(name, sizeof name, "%s", base);
    else fm_snprintf(name, sizeof name, "%s (%d)", base, i);
    fm_snprintf(a, sizeof a, "%s%s", name, ext);
    fm_snprintf(b, sizeof b, "%s.txt", name);
    if (!fm_path_join(dst, cap, dir, a) || !fm_path_join(txt, cap, dir, b)) { e = FM_ERR_IO; break; }
    if (plat_exists(dst) || plat_exists(txt)) continue;
    e = plat_rename(tmp, dst);                     /* fails rather than overwrite */
    if (e == FM_OK || e != FM_ERR_EXISTS) break;
  }
  SDL_AtomicUnlock(&g_dest_lock);
  return e;
}

static bool write_text(const char *path, const char *text) {
  FILE *f = fm_fopen(path, "wb");
  if (!f) return false;
  size_t n = strlen(text);
  bool ok = fwrite(text, 1, n, f) == n;
  return fclose(f) == 0 && ok;
}

/* the extension from the first bytes; *markup = an HTML / JSON / XML page */
static const char *file_sniff(const char *path, bool *markup) {
  u8 head[128];
  *markup = false;
  FILE *f = fm_fopen(path, "rb");
  if (!f) return "";
  size_t n = fread(head, 1, sizeof head, f);
  fclose(f);
  size_t i = 0;
  if (n >= 3 && !memcmp(head, "\xEF\xBB\xBF", 3)) i = 3;
  while (i < n && (head[i] == ' ' || head[i] == '\t' || head[i] == '\r' || head[i] == '\n')) i++;
  *markup = i < n && (head[i] == '<' || head[i] == '{' || head[i] == '[');
  return asrc_sniff_ext(head, n);
}

FmErr asrc_download(const FmAsrcConf *c, const FmAsrcItem *item, const char *dir, char *out, size_t cap,
                    bool (*progress)(void *user, u64 done, u64 total), void *user, char *err, size_t errcap,
                    volatile int *cancel) {
  if (cap) out[0] = 0;
  if (errcap) err[0] = 0;
  if (!c || !item) { fm_strlcpy(err, "Nothing to download", errcap); return FM_ERR_NOT_FOUND; }
  if (item->kind == AITEM_STATION) {
    fm_strlcpy(err, "Live radio cannot be downloaded", errcap);
    return FM_ERR_UNSUPPORTED;
  }
  if (!dir || !*dir) dir = c->download_dir;
  if (!*dir) { fm_strlcpy(err, "No download folder", errcap); return FM_ERR_NOT_FOUND; }
  const FmAsrc *src = asrc_find(item->source);
  const char *site = src ? src->name : "The site";
  FmAsrcStream st;
  FmErr e = find_stream(c, item, &st, err, errcap, cancel);
  if (e != FM_OK) return e;
  if (st.live) { fm_strlcpy(err, "Live streams cannot be downloaded", errcap); return FM_ERR_UNSUPPORTED; }
  if (!asrc_need_net(err, errcap)) return FM_ERR_UNSUPPORTED;
  if (plat_mkdirs(dir) != FM_OK && !plat_is_dir(dir)) {
    fm_snprintf(err, errcap, "Cannot create the folder %s", dir);
    return FM_ERR_IO;
  }
  char base[200], name[300], tmp[FM_PATH_MAX], dst[FM_PATH_MAX], txt[FM_PATH_MAX];
  asrc_file_base(item, base, 150);
  fm_snprintf(name, sizeof name, "%s.%llx.download", base, (unsigned long long)plat_now_ms());
  if (!fm_path_join(tmp, sizeof tmp, dir, name)) {
    fm_strlcpy(err, "The folder path is too long", errcap);
    return FM_ERR_IO;
  }

  FmNetResp r;
  for (int attempt = 0; attempt < 2; attempt++) {
    memset(&r, 0, sizeof r);
    e = net_download(st.url, st.headers[0] ? st.headers : NULL, tmp, progress, user, &r, cancel);
    /* a connect failure (no HTTP status) is tried once more: Audius redirects
    ** each request to some content node, and one node's name failed to
    ** resolve on 2026-10-08 while the next request worked */
    if (e == FM_OK || e == FM_ERR_CANCEL || r.status) break;
  }
  if (e == FM_ERR_CANCEL) { plat_remove_file(tmp); fm_strlcpy(err, "Cancelled", errcap); return e; }
  if (e != FM_OK) {
    plat_remove_file(tmp);
    if (r.status && (r.status < 200 || r.status >= 300)) {
      psrc_http_error(site, r.status, false, err, errcap);
      return r.status == 404 || r.status == 410 ? FM_ERR_NOT_FOUND : FM_ERR_IO;
    }
    fm_snprintf(err, errcap, "Download failed \xE2\x80\x94 %s", r.error[0] ? r.error : fm_err_str(e));
    return e;
  }
  bool markup;
  const char *ext = file_sniff(tmp, &markup);
  if (!*ext && !markup) ext = asrc_mime_ext(r.type);    /* raw AAC/MP3 without a header the sniffer knows */
  if (!*ext) {
    plat_remove_file(tmp);
    fm_snprintf(err, errcap, "%s sent something that is not audio (%s)", site, r.type[0] ? r.type : "unknown type");
    return FM_ERR_FORMAT;
  }
  e = place_file(dir, base, ext, tmp, dst, txt, sizeof dst);
  if (e != FM_OK) {
    plat_remove_file(tmp);
    fm_snprintf(err, errcap, "Could not save: %s",
                e == FM_ERR_EXISTS ? "too many files with this name" : fm_err_str(e));
    return e;
  }
  if (asrc_wants_credit(item)) {
    char text[3600];
    asrc_credit(item, site, st.url, text, sizeof text);
    write_text(txt, text);                         /* the music itself is what matters */
  }
  fm_strlcpy(out, dst, cap);
  return FM_OK;
}
