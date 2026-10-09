/* fdec_vid_soft.c -- the built-in video backend: VP9 + Opus in WebM, decoded
** in portable C on every target (tcc included).
**
** The last backend fdec_vid.c tries, after pl_mpeg, FFmpeg and the OS
** decoders, so Linux, macOS, FreeBSD, the web build and the tcc build can
** still play WebM, which is what online sources stream (YouTube's VP9 and
** Opus formats).
**
** Design decisions:
**   - Demuxing is nestegg (vendor/nestegg), VP9 is libvpx's decoder built for
**     generic C (vendor/libvpx), Opus is libopus float (vendor/opus). All
**     three are permissive and need no assembler or run-time CPU detection.
**   - Input is a local file (fm_fopen) or an http(s) URL read through
**     FmNetStream, whose lazy range seeks fit nestegg's io callbacks: the
**     header, the Cues and the target cluster are separate small reads.
**   - Only profile-0 (8-bit 4:2:0) VP9 is accepted; 10-bit and 4:4:4 are
**     refused at open (the first key frame's header says so). Frames are
**     handed out in libvpx's own buffers, valid until the next decode.
**   - libvpx runs with min(CPUs, 4) threads, row-based multithreading and
**     the loop-filter optimisation (single threaded on tcc and the web).
**   - Opus is decoded at 48 kHz through the multistream API (which covers
**     mono and stereo too), with the OpusHead from CodecPrivate. Samples
**     before time 0 (CodecDelay / pre-skip), DiscardPadding at the end of a
**     packet and, after a seek, the samples before the target (80 ms of
**     SeekPreRoll are decoded and thrown away) are dropped by time.
**   - A seek goes to the Cues entry at or before t, then frames before t are
**     decoded but not handed out, as the FFmpeg backend does. Files without
**     Cues are reopened and decoded forward.
**   - Everything is bounded: packet sizes, lacing counts, channel counts,
**     the read-ahead at open and the loops in decode.
*/
#include "fdec_vid_int.h"
#include "fplat.h"
#include "fnetstream.h"

#include "nestegg/nestegg.h"
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"
#include "opus_multistream.h"

#include <math.h>

#define SOFT_AHEAD    64               /* packets read ahead at open (fps, key frame) */
#define SOFT_MAX_PKT  (16 << 20)       /* bytes in one frame */
#define SOFT_MAX_LACE 64               /* frames in one block */
#define SOFT_OPUS_MAX 5760             /* samples per channel in one Opus packet (120 ms) */
#define SOFT_WIN      (256 << 10)      /* io window */
#define SOFT_KEEP     (64 << 10)       /* history kept when the window slides */

/* The source behind a window of recent bytes. nestegg rewinds a few KB
** whenever its own 8 KB buffer runs short; served from here, that never
** reaches the source, where a step back would be a new HTTP request. */
typedef struct SoftIo {
  FILE *f;
  FmNetStream *ns;
  u8 *win;                             /* SOFT_WIN bytes */
  i64 win_pos;                         /* source offset of win[0] */
  int win_len;
  i64 cur;                             /* logical position (nestegg's) */
  i64 src;                             /* where the source is; -1 unknown */
} SoftIo;

typedef struct SoftVid {
  SoftIo io;
  nestegg *ne;
  int flags;
  /* tracks; -1 = not used */
  int vtrack, atrack;
  /* VP9 */
  vpx_codec_ctx_t vpx;
  bool vpx_ok, need_key;
  int vpx_errors;                      /* consecutive decode errors */
  double v_skip;                       /* frames before this time are not handed out */
  /* Opus */
  OpusMSDecoder *opus;
  int channels;                        /* coded channels */
  int family;                          /* channel mapping family (0 or 1) */
  double a_delay;                      /* CodecDelay (pre-skip) in seconds */
  double a_preroll;                    /* SeekPreRoll in seconds */
  double a_skip;                       /* samples before this time are dropped */
  float *dec;                          /* decoded, coded channels */
  int dec_cap;                         /* floats */
  float *out;                          /* handed out, <= 2 channels */
  int out_cap;                         /* floats */
  /* packets read ahead at open, handed out before reading more */
  nestegg_packet *ahead[SOFT_AHEAD];
  int ahead_n, ahead_pos;
  bool ended;
} SoftVid;

/* ---- io --------------------------------------------------------------------- */

static bool is_url(const char *p) { return !fm_strnicmp(p, "http://", 7) || !fm_strnicmp(p, "https://", 8); }

static size_t src_read(SoftIo *io, void *buf, size_t n) {
  size_t r = io->ns ? ns_read(io->ns, buf, n) : fread(buf, 1, n, io->f);
  if (io->src >= 0) io->src += (i64)r;
  return r;
}

static bool src_seek(SoftIo *io, i64 pos) {
  if (io->src == pos) return true;
  bool ok = io->ns ? ns_seek(io->ns, pos) : fm_fseek64(io->f, pos, SEEK_SET) == 0;
  io->src = ok ? pos : -1;
  return ok;
}

static int64_t io_read(void *buf, size_t n, void *ud) {
  SoftIo *io = (SoftIo *)ud;
  u8 *out = (u8 *)buf;
  size_t done = 0;
  if (!n) return 0;
  for (int guard = 0; guard < 4 && done < n; guard++) {
    i64 end = io->win_pos + io->win_len;
    if (io->cur >= io->win_pos && io->cur < end) {       /* in the window */
      size_t k = (size_t)FM_MIN((i64)(n - done), end - io->cur);
      memcpy(out + done, io->win + (io->cur - io->win_pos), k);
      io->cur += (i64)k;
      done += k;
      continue;
    }
    if (io->cur != end) {                                   /* a jump: start a new window */
      io->win_pos = io->cur;
      io->win_len = 0;
      end = io->cur;
    }
    if (!src_seek(io, end)) return done ? (int64_t)done : -1;
    if (n - done >= SOFT_KEEP) {                            /* big frames go straight through */
      size_t r = src_read(io, out + done, n - done);
      if (!r) break;
      io->cur += (i64)r;
      done += r;
      io->win_pos = io->cur;                                /* (the window restarts after them) */
      io->win_len = 0;
      continue;
    }
    if (io->win_len > SOFT_WIN - SOFT_KEEP) {               /* slide, keeping some history */
      int drop = io->win_len - SOFT_KEEP;
      memmove(io->win, io->win + drop, (size_t)SOFT_KEEP);
      io->win_pos += drop;
      io->win_len = SOFT_KEEP;
    }
    size_t r = src_read(io, io->win + io->win_len, (size_t)(SOFT_WIN - io->win_len));
    if (!r) break;
    io->win_len += (int)r;
  }
  return (int64_t)done;
}

static int64_t io_tell(void *ud) {
  return ((SoftIo *)ud)->cur;
}

/* Only moves the logical position; the source follows on the next read. */
static int io_seek(int64_t off, int whence, void *ud) {
  SoftIo *io = (SoftIo *)ud;
  i64 pos = off;
  if (whence == NESTEGG_SEEK_CUR) pos += io->cur;
  else if (whence == NESTEGG_SEEK_END) {
    i64 sz = -1;
    if (io->ns) sz = ns_size(io->ns);
    else if (fm_fseek64(io->f, 0, SEEK_END) == 0) { sz = fm_ftell64(io->f); io->src = sz; }
    if (sz < 0) return -1;
    pos += sz;
  }
  if (pos < 0) return -1;
  if (io->ns && pos != io->cur && !(pos >= io->win_pos && pos <= io->win_pos + io->win_len) &&
      !ns_seekable(io->ns) && pos < ns_tell(io->ns))
    return -1;                                              /* cannot go back on this stream */
  io->cur = pos;
  return 0;
}

static bool io_open(SoftIo *io, const char *path) {
  u8 magic[4];
  io->win = (u8 *)fm_alloc(SOFT_WIN);
  if (is_url(path)) {
    char err[160];
    io->ns = ns_open(path, NULL, err, sizeof err);
    if (!io->ns) { fm_log("video (built-in): %.200s: %s", path, err); vid_note_net_error(err); return false; }
    if (ns_peek(io->ns, magic, 4) != 4) return false;
  } else {
    io->f = fm_fopen(path, "rb");
    if (!io->f) return false;
    if (fread(magic, 1, 4, io->f) != 4 || fm_fseek64(io->f, 0, SEEK_SET) != 0) return false;
  }
  io->src = io->cur = io->win_pos = 0;   /* ns_peek does not consume */
  /* EBML header: anything else is not ours (and costs nothing to refuse) */
  return magic[0] == 0x1A && magic[1] == 0x45 && magic[2] == 0xDF && magic[3] == 0xA3;
}

static void io_close(SoftIo *io) {
  if (io->ns) ns_close(io->ns);
  if (io->f) fclose(io->f);
  fm_free(io->win);
  memset(io, 0, sizeof *io);
}

static bool ne_start(SoftVid *s) {
  nestegg_io nio = { io_read, io_seek, io_tell, &s->io };
  return nestegg_init(&s->ne, nio, NULL, -1) == 0 && s->ne;
}

/* ---- packets ------------------------------------------------------------------ */

static void ahead_clear(SoftVid *s) {
  for (int i = s->ahead_pos; i < s->ahead_n; i++) nestegg_free_packet(s->ahead[i]);
  s->ahead_n = s->ahead_pos = 0;
}

/* Next packet of a track we use, or NULL at the end (or on a read error). */
static nestegg_packet *next_packet(SoftVid *s) {
  if (s->ahead_pos < s->ahead_n) return s->ahead[s->ahead_pos++];
  for (int guard = 0; guard < 100000 && !s->ended; guard++) {
    nestegg_packet *p = NULL;
    int r = nestegg_read_packet(s->ne, &p);
    if (r <= 0 || !p) { s->ended = true; return NULL; }
    unsigned tr;
    if (nestegg_packet_track(p, &tr) == 0 && ((int)tr == s->vtrack || (int)tr == s->atrack)) return p;
    nestegg_free_packet(p);
  }
  return NULL;
}

static int pkt_track(nestegg_packet *p) {
  unsigned tr;
  return nestegg_packet_track(p, &tr) == 0 ? (int)tr : -1;
}

static double pkt_time(nestegg_packet *p) {
  uint64_t ts = 0;
  return nestegg_packet_tstamp(p, &ts) == 0 ? (double)ts / 1e9 : 0;
}

/* ---- VP9 ---------------------------------------------------------------------- */

/* Frame header bits: frame_marker(2) profile_low profile_high [reserved]
** show_existing_frame frame_type (0 = key frame). */
static int vp9_profile(const u8 *d, size_t n) {
  if (n < 1 || (d[0] >> 6) != 2) return -1;
  return ((d[0] >> 5) & 1) | (((d[0] >> 4) & 1) << 1);
}

static bool vp9_is_key(const u8 *d, size_t n) {
  int prof = vp9_profile(d, n);
  if (prof < 0) return false;
  int at = prof == 3 ? 5 : 4;              /* show_existing_frame, counted from the top bit */
  u32 b = (u32)d[0] << 8 | (n > 1 ? d[1] : 0);
  bool show_existing = (b >> (15 - at)) & 1;
  bool inter = (b >> (14 - at)) & 1;
  return !show_existing && !inter;
}

static bool vpx_start(SoftVid *s) {
  vpx_codec_dec_cfg_t cfg;
  memset(&cfg, 0, sizeof cfg);
#if !defined(__TINYC__) && !defined(FM_WEB)
  int n = plat_cpu_count();
  cfg.threads = (unsigned)FM_CLAMP(n, 1, 4);
#else
  cfg.threads = 1;
#endif
  if (vpx_codec_dec_init(&s->vpx, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK) return false;
  s->vpx_ok = true;
  if (cfg.threads > 1) vpx_codec_control(&s->vpx, VP9D_SET_ROW_MT, 1);
  vpx_codec_control(&s->vpx, VP9D_SET_LOOP_FILTER_OPT, 1);
  return true;
}

/* Decodes the frames of one block; true with vf filled when a picture is out. */
static bool video_packet(SoftVid *s, nestegg_packet *p, FmVidFrame *vf) {
  unsigned cnt = 0;
  if (nestegg_packet_count(p, &cnt) != 0 || cnt > SOFT_MAX_LACE) return false;
  if (s->need_key) {
    int k = nestegg_packet_has_keyframe(p);
    u8 *d0 = NULL;
    size_t n0 = 0;
    if (k == NESTEGG_PACKET_HAS_KEYFRAME_FALSE) return false;
    if (cnt < 1 || nestegg_packet_data(p, 0, &d0, &n0) != 0 || !vp9_is_key(d0, n0)) return false;
    s->need_key = false;
  }
  vpx_image_t *img = NULL;
  for (unsigned i = 0; i < cnt; i++) {
    u8 *d = NULL;
    size_t n = 0;
    if (nestegg_packet_data(p, i, &d, &n) != 0 || !d || !n || n > SOFT_MAX_PKT) continue;
    if (vpx_codec_decode(&s->vpx, d, (unsigned)n, NULL, 0) != VPX_CODEC_OK) {
      if (++s->vpx_errors == 1) fm_log("video (built-in): VP9: %s", vpx_codec_error_detail(&s->vpx) ? vpx_codec_error_detail(&s->vpx) : vpx_codec_error(&s->vpx));
      continue;
    }
    s->vpx_errors = 0;
    vpx_codec_iter_t it = NULL;
    vpx_image_t *im;
    while ((im = vpx_codec_get_frame(&s->vpx, &it)) != NULL) img = im;
  }
  if (!img) return false;
  if (img->fmt != VPX_IMG_FMT_I420 || img->d_w < 1 || img->d_h < 1 || img->d_w > 16384 || img->d_h > 16384 ||
      !img->planes[0] || !img->planes[1] || !img->planes[2]) {
    s->vpx_errors = 1000;                /* not 8-bit 4:2:0: give up on the picture */
    return false;
  }
  double t = pkt_time(p);
  if (t < s->v_skip - 0.001) return false;
  vf->t = t;
  vf->w = (int)img->d_w;
  vf->h = (int)img->d_h;
  for (int i = 0; i < 3; i++) {
    vf->plane[i] = img->planes[i];
    vf->stride[i] = img->stride[i];
  }
  return true;
}

/* ---- Opus --------------------------------------------------------------------- */

static bool opus_start(SoftVid *s, unsigned track) {
  unsigned items = 0;
  u8 *head = NULL;
  size_t n = 0;
  if (nestegg_track_codec_data_count(s->ne, track, &items) != 0 || items < 1 ||
      nestegg_track_codec_data(s->ne, track, 0, &head, &n) != 0 || !head || n < 19 ||
      memcmp(head, "OpusHead", 8) != 0)
    return false;
  int ch = head[9];
  int preskip = head[10] | head[11] << 8;
  int gain = (i16)(head[16] | head[17] << 8);
  int family = head[18];
  int streams = 1, coupled = ch == 2;
  u8 map[255] = { 0, 1 };
  if (ch < 1) return false;
  if (family == 0) {
    if (ch > 2) return false;
  } else if (family == 1) {
    if (ch > 8 || n < (size_t)21 + ch) return false;
    streams = head[19];
    coupled = head[20];
    memcpy(map, head + 21, (size_t)ch);
    if (streams < 1 || coupled > streams || streams + coupled > 255) return false;
  } else {
    return false;
  }
  int err = 0;
  s->opus = opus_multistream_decoder_create(48000, ch, streams, coupled, map, &err);
  if (!s->opus || err != OPUS_OK) { s->opus = NULL; return false; }
  if (gain) opus_multistream_decoder_ctl(s->opus, OPUS_SET_GAIN(gain));
  s->channels = ch;
  s->family = family;
  nestegg_audio_params ap;
  memset(&ap, 0, sizeof ap);
  nestegg_track_audio_params(s->ne, track, &ap);
  s->a_delay = ap.codec_delay > 0 && ap.codec_delay < 1000000000ull ? (double)ap.codec_delay / 1e9 : preskip / 48000.0;
  s->a_preroll = ap.seek_preroll > 0 && ap.seek_preroll < 1000000000ull ? (double)ap.seek_preroll / 1e9 : 0.08;
  return true;
}

/* Left/right weights per coded channel (Vorbis order for family 1). */
static void opus_weights(int ch, float *wl, float *wr) {
  static const float k = 0.7071f;
  for (int c = 0; c < ch; c++) { wl[c] = 0; wr[c] = 0; }
  switch (ch) {
    case 1: wl[0] = 1; break;
    case 2: wl[0] = 1; wr[1] = 1; break;
    case 3: wl[0] = 1; wl[1] = wr[1] = k; wr[2] = 1; break;                 /* L C R */
    case 4: wl[0] = 1; wr[1] = 1; wl[2] = k; wr[3] = k; break;              /* FL FR RL RR */
    case 5: case 6:                                                          /* FL C FR RL RR (LFE) */
      wl[0] = 1; wl[1] = wr[1] = k; wr[2] = 1; wl[3] = k; wr[4] = k; break;
    case 7:                                                                  /* FL C FR SL SR RC LFE */
      wl[0] = 1; wl[1] = wr[1] = k; wr[2] = 1; wl[3] = k; wr[4] = k; wl[5] = wr[5] = 0.5f; break;
    default:                                                                 /* FL C FR SL SR RL RR LFE */
      wl[0] = 1; wl[1] = wr[1] = k; wr[2] = 1; wl[3] = k; wr[4] = k; wl[5] = k; wr[6] = k; break;
  }
}

static bool audio_packet(SoftVid *s, nestegg_packet *p, FmVidPcm *pc) {
  unsigned cnt = 0;
  if (nestegg_packet_count(p, &cnt) != 0 || cnt < 1 || cnt > SOFT_MAX_LACE) return false;
  double t0 = pkt_time(p) - s->a_delay;
  /* far before the target (after a seek): not even decoded */
  if (t0 + 0.125 * cnt < s->a_skip - s->a_preroll) return false;
  int ch = s->channels, total = 0;
  for (unsigned i = 0; i < cnt; i++) {
    u8 *d = NULL;
    size_t n = 0;
    if (nestegg_packet_data(p, i, &d, &n) != 0 || !d || !n || n > 65536) continue;
    int need = (total + SOFT_OPUS_MAX) * ch;
    if (need > s->dec_cap) {
      s->dec_cap = need;
      s->dec = (float *)fm_realloc(s->dec, (size_t)need * sizeof(float));
    }
    int got = opus_multistream_decode_float(s->opus, d, (opus_int32)n, s->dec + (size_t)total * ch, SOFT_OPUS_MAX, 0);
    if (got > 0) total += got;
  }
  if (total <= 0) return false;
  i64 pad = 0;
  if (nestegg_packet_discard_padding(p, &pad) == 0 && pad > 0) {
    i64 drop = (i64)((double)pad * 48000.0 / 1e9 + 0.5);
    total = drop >= total ? 0 : total - (int)drop;
  }
  int first = 0;
  if (t0 < s->a_skip) {
    double d = (s->a_skip - t0) * 48000.0;
    first = d >= total ? total : (int)ceil(d - 1e-6);
  }
  int frames = total - first;
  if (frames <= 0) return false;
  int oc = ch >= 2 ? 2 : 1;
  if (frames * oc > s->out_cap) {
    s->out_cap = frames * oc;
    fm_free(s->out);
    s->out = (float *)fm_alloc((size_t)s->out_cap * sizeof(float));
  }
  const float *src = s->dec + (size_t)first * ch;
  if (ch <= 2) {
    memcpy(s->out, src, (size_t)frames * ch * sizeof(float));
  } else {
    float wl[8], wr[8], sl = 0, sr = 0;
    opus_weights(ch, wl, wr);
    for (int c = 0; c < ch; c++) { sl += wl[c]; sr += wr[c]; }
    float kl = sl > 0 ? 1.0f / sl : 1, kr = sr > 0 ? 1.0f / sr : 1;
    for (int i = 0; i < frames; i++) {
      float l = 0, r = 0;
      for (int c = 0; c < ch; c++) { l += src[c] * wl[c]; r += src[c] * wr[c]; }
      s->out[2 * i] = FM_CLAMP(l * kl * 1.4142f, -1.0f, 1.0f);
      s->out[2 * i + 1] = FM_CLAMP(r * kr * 1.4142f, -1.0f, 1.0f);
      src += ch;
    }
  }
  pc->t = t0 + first / 48000.0;
  pc->frames = frames;
  pc->channels = oc;
  pc->rate = 48000;
  pc->pcm = s->out;
  return true;
}

/* ---- backend ------------------------------------------------------------------- */

static void soft_close(void *st) {
  SoftVid *s = (SoftVid *)st;
  if (!s) return;
  ahead_clear(s);
  if (s->ne) nestegg_destroy(s->ne);
  if (s->vpx_ok) vpx_codec_destroy(&s->vpx);
  if (s->opus) opus_multistream_decoder_destroy(s->opus);
  io_close(&s->io);
  fm_free(s->dec);
  fm_free(s->out);
  fm_free(s);
}

/* Common frame rates, when the measured one is within 1 % of them. */
static double snap_fps(double f) {
  static const double kRates[] = { 23.976, 24, 25, 29.97, 30, 48, 50, 59.94, 60, 120 };
  for (int i = 0; i < FM_COUNT(kRates); i++)
    if (fabs(f - kRates[i]) < kRates[i] * 0.01) return kRates[i];
  return f;
}

static void *soft_open(const char *path, int flags, FmVidInfo *info) {
  SoftVid *s = (SoftVid *)fm_calloc(1, sizeof *s);
  s->flags = flags;
  s->vtrack = s->atrack = -1;
  if (!io_open(&s->io, path) || !ne_start(s)) { soft_close(s); return NULL; }
  unsigned ntr = 0;
  if (nestegg_track_count(s->ne, &ntr) != 0) { soft_close(s); return NULL; }
  for (unsigned i = 0; i < ntr && i < 64; i++) {
    int type = nestegg_track_type(s->ne, i), codec = nestegg_track_codec_id(s->ne, i);
    if (type == NESTEGG_TRACK_VIDEO && codec == NESTEGG_CODEC_VP9 && s->vtrack < 0 && !(flags & VID_OPEN_AUDIO_ONLY)) {
      nestegg_video_params vp;
      if (nestegg_track_video_params(s->ne, i, &vp) != 0 || vp.width < 1 || vp.height < 1 || vp.width > 16384 ||
          vp.height > 16384)
        continue;
      s->vtrack = (int)i;
      info->w = (int)(vp.display_width ? vp.display_width : vp.width);
      info->h = (int)(vp.display_height ? vp.display_height : vp.height);
      info->sar = 1.0;
      if (vp.display_width && vp.display_height) {
        double sar = ((double)vp.display_width / vp.display_height) / ((double)vp.width / vp.height);
        if (sar > 0.1 && sar < 10) info->sar = sar;
      }
      uint64_t dd = 0;
      if (nestegg_track_default_duration(s->ne, i, &dd) == 0 && dd > 0) info->fps = 1e9 / (double)dd;
    } else if (type == NESTEGG_TRACK_AUDIO && codec == NESTEGG_CODEC_OPUS && s->atrack < 0 && !(flags & VID_OPEN_NO_AUDIO)) {
      if (opus_start(s, i)) s->atrack = (int)i;
    }
  }
  if (s->vtrack < 0 && s->atrack < 0) { soft_close(s); return NULL; }

  /* read ahead: the first key frame (its header says the profile) and a few
  ** frame times for the rate when the track has no default duration */
  double vt[9];
  int nvt = 0;
  bool key_seen = s->vtrack < 0;
  while (s->ahead_n < SOFT_AHEAD && (!key_seen || (s->vtrack >= 0 && info->fps <= 0 && nvt < 9))) {
    nestegg_packet *p = next_packet(s);
    if (!p) break;
    s->ahead[s->ahead_n++] = p;
    if (pkt_track(p) != s->vtrack) continue;
    if (nvt < 9) vt[nvt++] = pkt_time(p);
    if (!key_seen) {
      u8 *d = NULL;
      size_t n = 0;
      if (nestegg_packet_data(p, 0, &d, &n) == 0 && d && n) {
        int prof = vp9_profile(d, n);
        if (prof != 0) {
          fm_log("video (built-in): VP9 profile %d (10-bit or 4:4:4) is not supported", prof);
          soft_close(s);
          return NULL;
        }
        key_seen = true;
      }
    }
  }
  if (info->fps <= 0 && nvt >= 3 && vt[nvt - 1] > vt[0]) info->fps = snap_fps((nvt - 1) / (vt[nvt - 1] - vt[0]));
  if (info->fps > 1000) info->fps = 0;

  if (s->vtrack >= 0 && !vpx_start(s)) { soft_close(s); return NULL; }
  s->need_key = true;
  info->has_video = s->vtrack >= 0;
  info->has_audio = s->atrack >= 0;
  if (info->has_audio) {
    info->rate = 48000;
    info->channels = s->channels >= 2 ? 2 : 1;
    fm_strlcpy(info->acodec, "opus", sizeof info->acodec);
  }
  if (info->has_video) fm_strlcpy(info->vcodec, "vp9", sizeof info->vcodec);
  uint64_t dur = 0;
  if (nestegg_duration(s->ne, &dur) == 0 && dur > 0) info->duration = (double)dur / 1e9;
  if (!info->sar) info->sar = 1.0;
  fm_strlcpy(info->backend, "built-in", sizeof info->backend);
  return s;
}

static int soft_decode(void *st, FmVidFrame *vf, FmVidPcm *pc) {
  SoftVid *s = (SoftVid *)st;
  for (int guard = 0; guard < 100000; guard++) {
    if (s->vpx_errors >= 100) return VID_EV_ERROR;
    nestegg_packet *p = next_packet(s);
    if (!p) return VID_EV_END;
    int tr = pkt_track(p);
    bool got = false;
    int ev = 0;
    if (tr == s->vtrack && s->vpx_ok) { got = video_packet(s, p, vf); ev = VID_EV_VIDEO; }
    else if (tr == s->atrack && s->opus) { got = audio_packet(s, p, pc); ev = VID_EV_AUDIO; }
    nestegg_free_packet(p);              /* the picture lives in libvpx, the sound in s->out */
    if (got) return ev;
  }
  return VID_EV_ERROR;
}

/* Starts again from the first cluster (files without Cues). */
static bool soft_restart(SoftVid *s) {
  ahead_clear(s);
  if (s->ne) nestegg_destroy(s->ne);
  s->ne = NULL;
  if (io_seek(0, NESTEGG_SEEK_SET, &s->io) != 0) return false;
  return ne_start(s);
}

static bool soft_seek(void *st, double t) {
  SoftVid *s = (SoftVid *)st;
  if (t < 0) t = 0;
  ahead_clear(s);
  s->ended = false;
  int track = s->vtrack >= 0 ? s->vtrack : s->atrack;
  double from = s->vtrack >= 0 ? t : FM_MAX(0.0, t - s->a_preroll);
  bool ok = nestegg_track_seek(s->ne, (unsigned)track, (uint64_t)(from * 1e9)) == 0;
  if (!ok && !soft_restart(s)) return false;
  s->need_key = true;
  s->vpx_errors = 0;
  s->v_skip = t;
  s->a_skip = t;
  if (s->opus) opus_multistream_decoder_ctl(s->opus, OPUS_RESET_STATE);
  return true;
}

const FmVidBackend g_vid_soft = { "built-in", soft_open, soft_decode, soft_seek, soft_close };
