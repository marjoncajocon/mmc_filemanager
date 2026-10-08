/* fdec_aud.c -- audio decoders behind one pull API, and tag parsers.
**
** Design decisions:
**   - The format is chosen from the first bytes, not the extension, so a
**     mislabelled file still plays; the extension only breaks ties.
**   - dr_mp3 gets a seek table (a header-only scan, no decoding) for files
**     up to 64 MB, which also yields the exact length; bigger files seek by
**     decoding forward and estimate their length from the bit rate.
**   - Tags are parsed from bounded reads (16 MB cap for covers); every
**     length field is checked against the buffer before use.
**   - FFmpeg-only formats reuse fdec_vid.c in audio-only mode; its blocks
**     are buffered here so aud_read can hand out any count of frames.
*/
#include "fdec_aud.h"
#include "fnetstream.h"
#include "fsdl.h"
#include "fdec_vid.h"
#include "fplat.h"

#include "dr_mp3.h"
#include "dr_flac.h"
#include "dr_wav.h"
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

enum { AK_NONE, AK_MP3, AK_FLAC, AK_WAV, AK_VORBIS, AK_FF };

struct FmAudio {
  int kind;
  FILE *f;
  /* http(s): the stream reader replaces the FILE for the dr_libs callbacks */
  FmNetStream *ns;
  i64 audio_start;       /* bytes before the first MPEG frame (ID3v2) */
  i64 budget;            /* >= 0: bytes the decoder may still read (finding the first frame) */
  bool live;             /* an endless stream, kept when it moves to the FFmpeg / MF path */
  int kbps;              /* first frame's bitrate, for length and seek estimates */
  drmp3 *mp3;
  drmp3_seek_point *seek_pts;
  drflac *flac;
  drwav *wav;
  stb_vorbis *vorb;
  FmVid *vid;
  int src_ch;            /* channels the decoder produces */
  int ch, rate;          /* what aud_read delivers */
  u64 len, pos;
  float *tmp;            /* multichannel decode buffer before downmix */
  int tmp_frames;
  /* FFmpeg leftovers */
  float *left;
  int left_n, left_off, left_cap;
  char codec[24];
};

/* ---- file callbacks for dr_libs --------------------------------------------- */

static size_t f_read(void *u, void *out, size_t n) {
  FmAudio *a = (FmAudio *)u;
  if (a->ns) {
    /* an endless stream that is not really MP3 must not keep dr_mp3
    ** searching for a frame forever: it gets a budget while it starts */
    if (a->budget >= 0) {
      if (a->budget == 0) return 0;
      if ((i64)n > a->budget) n = (size_t)a->budget;
      size_t got = ns_read(a->ns, out, n);
      a->budget -= (i64)got;
      return got;
    }
    return ns_read(a->ns, out, n);
  }
  return fread(out, 1, n, a->f);
}

static int f_seek_any(FmAudio *a, int off, int origin) {
  if (a->ns) {
    i64 to = origin == 0 ? off : origin == 1 ? ns_tell(a->ns) + off : ns_size(a->ns) + off;
    return (origin != 2 || ns_size(a->ns) >= 0) && ns_seek(a->ns, to);
  }
  int wh = origin == 0 ? SEEK_SET : origin == 1 ? SEEK_CUR : SEEK_END;
  return fm_fseek64(a->f, off, wh) == 0;
}

static i64 f_tell(FmAudio *a) { return a->ns ? ns_tell(a->ns) : fm_ftell64(a->f); }
static drmp3_bool32 mp3_seek(void *u, int off, drmp3_seek_origin o) {
  return (drmp3_bool32)f_seek_any((FmAudio *)u, off, o == DRMP3_SEEK_SET ? 0 : o == DRMP3_SEEK_CUR ? 1 : 2);
}
static drmp3_bool32 mp3_tell(void *u, drmp3_int64 *c) { *c = f_tell((FmAudio *)u); return *c >= 0; }
static drflac_bool32 flac_seek(void *u, int off, drflac_seek_origin o) {
  return (drflac_bool32)f_seek_any((FmAudio *)u, off, o == DRFLAC_SEEK_SET ? 0 : o == DRFLAC_SEEK_CUR ? 1 : 2);
}
static drflac_bool32 flac_tell(void *u, drflac_int64 *c) { *c = f_tell((FmAudio *)u); return *c >= 0; }
static drwav_bool32 wav_seek(void *u, int off, drwav_seek_origin o) {
  return (drwav_bool32)f_seek_any((FmAudio *)u, off, o == DRWAV_SEEK_SET ? 0 : o == DRWAV_SEEK_CUR ? 1 : 2);
}
static drwav_bool32 wav_tell(void *u, drwav_int64 *c) { *c = f_tell((FmAudio *)u); return *c >= 0; }

/* ---- open ---------------------------------------------------------------------- */

static int sniff_audio(const u8 *b, size_t n, const char *path) {
  if (n >= 4 && memcmp(b, "fLaC", 4) == 0) return AK_FLAC;
  if (n >= 12 && (memcmp(b, "RIFF", 4) == 0 || memcmp(b, "RF64", 4) == 0) && memcmp(b + 8, "WAVE", 4) == 0)
    return AK_WAV;
  if (n >= 12 && memcmp(b, "FORM", 4) == 0 && (memcmp(b + 8, "AIFF", 4) == 0 || memcmp(b + 8, "AIFC", 4) == 0))
    return AK_WAV;
  if (n >= 36 && memcmp(b, "OggS", 4) == 0) {
    /* first packet of the first page: "\x01vorbis" for Vorbis */
    int nseg = b[26];
    size_t p = 27 + (size_t)nseg;
    if (p + 7 <= n && memcmp(b + p, "\x01vorbis", 7) == 0) return AK_VORBIS;
    return AK_FF;
  }
  if (n >= 3 && memcmp(b, "ID3", 3) == 0) {
    if (path && fm_ends_with_i(path, ".flac")) return AK_FLAC;
    return AK_MP3;
  }
  if (n >= 2 && b[0] == 0xFF && (b[1] & 0xE0) == 0xE0 && (b[1] & 0x06) != 0) {
    /* MPEG audio sync; ADTS AAC has layer bits 00 and goes to FFmpeg */
    return AK_MP3;
  }
  if (path && (fm_ends_with_i(path, ".mp3") || fm_ends_with_i(path, ".mp2"))) return AK_MP3;
  return AK_FF;
}

static void aud_free(FmAudio *a) {
  if (a->mp3) { drmp3_uninit(a->mp3); fm_free(a->mp3); }
  fm_free(a->seek_pts);
  if (a->flac) drflac_close(a->flac);
  if (a->wav) { drwav_uninit(a->wav); fm_free(a->wav); }
  if (a->vorb) stb_vorbis_close(a->vorb);
  if (a->vid) vid_close(a->vid);
  if (a->f) fclose(a->f);
  if (a->ns) ns_close(a->ns);
  fm_free(a->tmp);
  fm_free(a->left);
  fm_free(a);
}

static bool open_mp3(FmAudio *a) {
  a->mp3 = (drmp3 *)fm_calloc(1, sizeof(drmp3));
  /* Over HTTP dr_mp3 gets no seek callback: it would probe the end of the
  ** file for ID3v1/APE tags (a request each way, seconds on slow hosts), and
  ** live radio cannot seek at all. Network seeks go through net_mp3_seek. */
  bool can_seek = !a->ns;
  a->budget = a->ns ? 512 * 1024 : -1;
  bool inited = drmp3_init(a->mp3, f_read, can_seek ? mp3_seek : NULL, can_seek ? mp3_tell : NULL, NULL, a, NULL);
  a->budget = -1;
  if (!inited) {
    fm_free(a->mp3);
    a->mp3 = NULL;
    return false;
  }
  a->src_ch = (int)a->mp3->channels;
  a->rate = (int)a->mp3->sampleRate;
  if (a->ns) {
    /* counting frames would download everything: estimate from the first
    ** frame's bitrate (exact for CBR, close for VBR); live radio has none */
    i64 size = ns_size(a->ns);
    if (size > a->audio_start && a->kbps > 0)
      a->len = (u64)((double)(size - a->audio_start) * 8.0 / (a->kbps * 1000.0) * a->rate);
    return true;
  }
  i64 fsize = fm_fsize(a->f);
  if (fsize > 0 && fsize <= 64ll * 1024 * 1024) {
    drmp3_uint64 nmp3 = 0, npcm = 0;
    if (a->mp3->totalPCMFrameCount != DRMP3_UINT64_MAX) npcm = a->mp3->totalPCMFrameCount;
    else if (drmp3_get_mp3_and_pcm_frame_count(a->mp3, &nmp3, &npcm)) {}
    a->len = npcm;
    drmp3_uint32 cnt = 512;
    a->seek_pts = (drmp3_seek_point *)fm_alloc(sizeof(drmp3_seek_point) * cnt);
    if (drmp3_calculate_seek_points(a->mp3, &cnt, a->seek_pts) && cnt > 0)
      drmp3_bind_seek_table(a->mp3, cnt, a->seek_pts);
    else {
      fm_free(a->seek_pts);
      a->seek_pts = NULL;
    }
  } else if (a->mp3->totalPCMFrameCount != DRMP3_UINT64_MAX) {
    a->len = a->mp3->totalPCMFrameCount;
  }
  return true;
}

/* ID3v2 tag length (header included), 0 when none */
static i64 id3_size(const u8 *b, size_t n) {
  if (n < 10 || memcmp(b, "ID3", 3) != 0) return 0;
  i64 sz = ((i64)(b[6] & 0x7F) << 21) | ((b[7] & 0x7F) << 14) | ((b[8] & 0x7F) << 7) | (b[9] & 0x7F);
  return sz + 10 + ((b[5] & 0x10) ? 10 : 0);
}

/* bitrate (kbps) of the first MPEG audio frame header found in b */
static int mpeg_kbps(const u8 *b, size_t n) {
  static const short v1l3[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
  static const short v1l2[16] = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0 };
  static const short v2l3[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
  for (size_t i = 0; i + 4 <= n; i++) {
    if (b[i] != 0xFF || (b[i + 1] & 0xE0) != 0xE0) continue;
    int ver = (b[i + 1] >> 3) & 3, layer = (b[i + 1] >> 1) & 3, idx = b[i + 2] >> 4;
    if (ver == 1 || layer == 0 || idx == 0 || idx == 15) continue;
    if (ver == 3) return layer == 1 ? v1l3[idx] : v1l2[idx];
    return v2l3[idx];
  }
  return 0;
}

static bool is_url(const char *p) { return !fm_strnicmp(p, "http://", 7) || !fm_strnicmp(p, "https://", 8); }

FmAudio *aud_open(const char *path, FmErr *err) { return aud_open_msg(path, NULL, err, NULL, 0); }

FmAudio *aud_open_ex(const char *path, const char *headers, FmErr *err) {
  return aud_open_msg(path, headers, err, NULL, 0);
}

FmAudio *aud_open_msg(const char *path, const char *headers, FmErr *err, char *msg, size_t cap) {
  if (msg && cap) msg[0] = 0;
  FmAudio *a = (FmAudio *)fm_calloc(1, sizeof *a);
  a->budget = -1;                      /* no read limit */
  u8 head[64];
  size_t hn;
  if (is_url(path)) {
    char e[160];
    a->ns = ns_open(path, headers, e, sizeof e);
    if (!a->ns) {
      fm_log("audio: %s: %s", path, e);
      if (msg) fm_strlcpy(msg, e, cap);
      fm_free(a);
      *err = FM_ERR_IO;
      return NULL;
    }
    /* sniff without consuming: radio cannot rewind */
    enum { PEEK = 16384, PEEK_MAX = 128 * 1024 };
    u8 *peek = (u8 *)fm_alloc(PEEK_MAX);
    size_t pn = ns_peek(a->ns, peek, PEEK);
    a->audio_start = id3_size(peek, pn);
    /* a big ID3 tag (cover art) hides the first frame: look further */
    if (a->audio_start > 0 && a->audio_start + 4096 > (i64)pn && a->audio_start + 4096 <= PEEK_MAX)
      pn = ns_peek(a->ns, peek, (size_t)a->audio_start + 4096);
    hn = FM_MIN(pn, sizeof head);
    memcpy(head, peek, hn);
    if (a->audio_start < (i64)pn) a->kbps = mpeg_kbps(peek + a->audio_start, pn - (size_t)a->audio_start);
    fm_free(peek);
  } else {
    a->f = fm_fopen(path, "rb");
    if (!a->f) { fm_free(a); *err = FM_ERR_NOT_FOUND; return NULL; }
    hn = fread(head, 1, sizeof head, a->f);
    fm_fseek64(a->f, 0, SEEK_SET);
  }
  a->kind = sniff_audio(head, hn, path);
  /* radio joins mid-frame, so the first byte is rarely a sync: trust the
  ** server's type, or a frame header found in the first 16 KB */
  if (a->ns && a->kind == AK_FF) {
    const char *ct = ns_content_type(a->ns);
    /* a frame sync found in the scan only counts when the server did not
    ** name another audio type: AAC data can contain one by chance */
    bool named_other = strstr(ct, "aac") || strstr(ct, "ogg") || strstr(ct, "opus") || strstr(ct, "flac") ||
                       strstr(ct, "mp4") || strstr(ct, "wav");
    if (strstr(ct, "audio/mpeg") || strstr(ct, "audio/mp3") || (a->kbps > 0 && !named_other)) a->kind = AK_MP3;
  }
  if (a->ns) fm_log("audio: stream kind %d, %d kbps, %s, '%s'", a->kind, a->kbps, ns_live(a->ns) ? "live" : "file",
                    ns_content_type(a->ns));
  /* Vorbis over HTTP: stb_vorbis wants a FILE, so FFmpeg / Media Foundation */
  if (a->ns && a->kind == AK_VORBIS) a->kind = AK_FF;
  bool ok = false;
  switch (a->kind) {
    case AK_MP3:
      ok = open_mp3(a);
      fm_strlcpy(a->codec, fm_ends_with_i(path, ".mp2") ? "MP2" : "MP3", sizeof a->codec);
      break;
    case AK_FLAC:
      a->flac = drflac_open(f_read, flac_seek, flac_tell, a, NULL);
      if (a->flac) {
        a->src_ch = a->flac->channels;
        a->rate = (int)a->flac->sampleRate;
        a->len = a->flac->totalPCMFrameCount;
        fm_strlcpy(a->codec, "FLAC", sizeof a->codec);
        ok = true;
      }
      break;
    case AK_WAV:
      a->wav = (drwav *)fm_calloc(1, sizeof(drwav));
      if (drwav_init(a->wav, f_read, wav_seek, wav_tell, a, NULL)) {
        a->src_ch = a->wav->channels;
        a->rate = (int)a->wav->sampleRate;
        a->len = a->wav->totalPCMFrameCount;
        fm_strlcpy(a->codec, memcmp(head, "FORM", 4) == 0 ? "AIFF" : "WAV", sizeof a->codec);
        ok = true;
      } else {
        fm_free(a->wav);
        a->wav = NULL;
      }
      break;
    case AK_VORBIS: {
      int e = 0;
      /* stb_vorbis owns the FILE from here (close_handle_on_close) */
      a->vorb = stb_vorbis_open_file(a->f, 1, &e, NULL);
      if (a->vorb) {
        a->f = NULL;
        stb_vorbis_info vi = stb_vorbis_get_info(a->vorb);
        a->src_ch = vi.channels;
        a->rate = (int)vi.sample_rate;
        a->len = stb_vorbis_stream_length_in_samples(a->vorb);
        fm_strlcpy(a->codec, "Vorbis", sizeof a->codec);
        ok = true;
      }
      break;
    }
    default:
      break;
  }
  if (!ok) {
    /* last resort: FFmpeg (or Media Foundation for URLs), when installed */
    if (a->f) { fclose(a->f); a->f = NULL; }
    if (a->ns) { a->live = ns_live(a->ns); ns_close(a->ns); a->ns = NULL; }
    FmErr ve;
    a->vid = vid_open(path, VID_OPEN_AUDIO_ONLY, &ve);
    if (a->vid && vid_info(a->vid)->has_audio) {
      const FmVidInfo *vi = vid_info(a->vid);
      a->kind = AK_FF;
      a->src_ch = vi->channels;
      a->rate = vi->rate;
      a->len = (u64)(vi->duration * vi->rate);
      fm_strlcpy(a->codec, vi->acodec[0] ? vi->acodec : "FFmpeg", sizeof a->codec);
      ok = a->rate > 0 && a->src_ch > 0;
    }
  }
  if (!ok || a->rate <= 0 || a->src_ch <= 0 || a->rate > 768000) {
    aud_free(a);
    *err = ff_available() ? FM_ERR_FORMAT : FM_ERR_UNSUPPORTED;
    if (msg && !msg[0] && is_url(path))
      fm_strlcpy(msg, *err == FM_ERR_UNSUPPORTED ? "This stream's format needs FFmpeg" : "Not an audio stream this player reads",
                 cap);
    return NULL;
  }
  a->ch = a->src_ch >= 2 ? 2 : 1;
  *err = FM_OK;
  return a;
}

int aud_channels(const FmAudio *a) { return a->ch; }
int aud_rate(const FmAudio *a) { return a->rate; }
u64 aud_length(const FmAudio *a) { return a->len; }
u64 aud_tell(const FmAudio *a) { return a->pos; }
const char *aud_codec(const FmAudio *a) { return a->codec; }

void aud_now_playing(const FmAudio *a, char *out, size_t cap) {
  if (a && a->ns) ns_now_playing(a->ns, out, cap);
  else if (cap) out[0] = 0;
}

bool aud_is_live(const FmAudio *a) { return a && (a->live || (a->ns && ns_live(a->ns))); }

void aud_close(FmAudio *a) {
  if (a) aud_free(a);
}

/* ---- decode ---------------------------------------------------------------------- */

static int ff_read(FmAudio *a, float *out, int frames) {
  int done = 0;
  int ch = a->src_ch;
  while (done < frames) {
    if (a->left_off < a->left_n) {
      int n = FM_MIN(frames - done, a->left_n - a->left_off);
      memcpy(out + (size_t)done * ch, a->left + (size_t)a->left_off * ch, (size_t)n * ch * sizeof(float));
      a->left_off += n;
      done += n;
      continue;
    }
    FmVidFrame vf;
    FmVidPcm pcm;
    int ev = vid_decode(a->vid, &vf, &pcm);
    if (ev == VID_EV_AUDIO) {
      if (pcm.channels != ch || pcm.frames <= 0) continue;
      if (pcm.frames > a->left_cap) {
        fm_free(a->left);
        a->left_cap = pcm.frames;
        a->left = (float *)fm_alloc((size_t)a->left_cap * ch * sizeof(float));
      }
      memcpy(a->left, pcm.pcm, (size_t)pcm.frames * ch * sizeof(float));
      a->left_n = pcm.frames;
      a->left_off = 0;
    } else if (ev != VID_EV_VIDEO) {
      break;
    }
  }
  return done;
}

static int raw_read(FmAudio *a, float *out, int frames) {
  switch (a->kind) {
    case AK_MP3: return (int)drmp3_read_pcm_frames_f32(a->mp3, (drmp3_uint64)frames, out);
    case AK_FLAC: return (int)drflac_read_pcm_frames_f32(a->flac, (drflac_uint64)frames, out);
    case AK_WAV: return (int)drwav_read_pcm_frames_f32(a->wav, (drwav_uint64)frames, out);
    case AK_VORBIS: return stb_vorbis_get_samples_float_interleaved(a->vorb, a->src_ch, out, frames * a->src_ch);
    case AK_FF: return ff_read(a, out, frames);
    default: return 0;
  }
}

int aud_read(FmAudio *a, float *out, int frames) {
  if (frames <= 0) return 0;
  int got;
  if (a->src_ch == a->ch) {
    got = raw_read(a, out, frames);
  } else {
    if (a->tmp_frames < frames) {
      fm_free(a->tmp);
      a->tmp_frames = frames;
      a->tmp = (float *)fm_alloc((size_t)frames * a->src_ch * sizeof(float));
    }
    got = raw_read(a, a->tmp, frames);
    /* downmix: even channels left, odd channels right (FL FR C LFE BL BR ...) */
    int sc = a->src_ch;
    for (int i = 0; i < got; i++) {
      const float *s = a->tmp + (size_t)i * sc;
      float l = 0, r = 0;
      int nl = 0, nr = 0;
      for (int c = 0; c < sc; c++) {
        if (sc >= 6 && c == 3) continue;          /* LFE */
        if (sc >= 3 && c == 2) { l += s[c] * 0.7071f; r += s[c] * 0.7071f; continue; }
        if (c & 1) { r += s[c]; nr++; } else { l += s[c]; nl++; }
      }
      float k = 1.0f / (float)FM_MAX(1, FM_MAX(nl, nr));
      out[i * 2] = FM_CLAMP(l * k, -1.0f, 1.0f);
      out[i * 2 + 1] = FM_CLAMP(r * k, -1.0f, 1.0f);
    }
  }
  if (got < 0) got = 0;
  a->pos += (u64)got;
  return got;
}

/* Over HTTP a sample-exact MP3 seek would decode from the start (download
** everything): jump to the proportional byte instead and let the decoder
** find the next frame there. */
static bool net_mp3_seek(FmAudio *a, u64 frame) {
  i64 size = ns_size(a->ns);
  if (!ns_seekable(a->ns) || size <= a->audio_start || !a->len) return false;
  double f = FM_CLAMP((double)frame / (double)a->len, 0.0, 0.999);
  i64 at = a->audio_start + (i64)(f * (double)(size - a->audio_start));
  if (!ns_seek(a->ns, at)) return false;
  drmp3_uninit(a->mp3);
  memset(a->mp3, 0, sizeof *a->mp3);
  if (!drmp3_init(a->mp3, f_read, mp3_seek, mp3_tell, NULL, a, NULL)) return false;
  return true;
}

bool aud_seek(FmAudio *a, u64 frame) {
  bool ok = false;
  switch (a->kind) {
    case AK_MP3: ok = a->ns ? net_mp3_seek(a, frame) : drmp3_seek_to_pcm_frame(a->mp3, frame) != 0; break;
    case AK_FLAC: ok = drflac_seek_to_pcm_frame(a->flac, frame) != 0; break;
    case AK_WAV: ok = drwav_seek_to_pcm_frame(a->wav, frame) != 0; break;
    case AK_VORBIS: ok = stb_vorbis_seek(a->vorb, (unsigned)frame) != 0; break;
    case AK_FF:
      ok = vid_seek(a->vid, (double)frame / a->rate);
      a->left_n = a->left_off = 0;
      break;
    default: break;
  }
  if (ok) a->pos = frame;
  return ok;
}

/* ---- text helpers ------------------------------------------------------------------ */

static void put_utf8(char *out, size_t cap, size_t *o, u32 cp) {
  char b[4];
  int n = utf8_encode(cp, b);
  if (*o + (size_t)n + 1 > cap) return;
  memcpy(out + *o, b, (size_t)n);
  *o += (size_t)n;
}

static void latin1_to_utf8(const u8 *s, size_t n, char *out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < n && s[i]; i++) put_utf8(out, cap, &o, s[i]);
  out[o] = 0;
}

static void utf16_to_utf8(const u8 *s, size_t n, bool be, char *out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i + 1 < n; i += 2) {
    u32 c = be ? (u32)(s[i] << 8 | s[i + 1]) : (u32)(s[i] | s[i + 1] << 8);
    if (c == 0) break;
    if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
      u32 d = be ? (u32)(s[i + 2] << 8 | s[i + 3]) : (u32)(s[i + 2] | s[i + 3] << 8);
      if (d >= 0xDC00 && d < 0xE000) {
        c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
        i += 2;
      }
    }
    put_utf8(out, cap, &o, c);
  }
  out[o] = 0;
}

static void utf8_copy(const u8 *s, size_t n, char *out, size_t cap) {
  size_t m = 0;
  while (m < n && s[m]) m++;
  if (m >= cap) {
    m = cap - 1;
    while (m > 0 && (s[m] & 0xC0) == 0x80) m--;     /* keep whole code points */
  }
  memcpy(out, s, m);
  out[m] = 0;
}

static void trim(char *s) {
  size_t n = strlen(s);
  while (n > 0 && (u8)s[n - 1] <= ' ') s[--n] = 0;
  size_t i = 0;
  while (s[i] && (u8)s[i] <= ' ') i++;
  if (i) memmove(s, s + i, n - i + 1);
}

/* ---- ID3v2 ---------------------------------------------------------------------- */

static u32 syncsafe(const u8 *p) {
  return (u32)(p[0] & 0x7F) << 21 | (u32)(p[1] & 0x7F) << 14 | (u32)(p[2] & 0x7F) << 7 | (u32)(p[3] & 0x7F);
}

static size_t unsync(u8 *p, size_t n) {
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    p[o++] = p[i];
    if (p[i] == 0xFF && i + 1 < n && p[i + 1] == 0x00) i++;
  }
  return o;
}

static void id3_text(const u8 *p, size_t n, char *out, size_t cap) {
  out[0] = 0;
  if (n < 1) return;
  u8 enc = p[0];
  p++; n--;
  if (enc == 0) latin1_to_utf8(p, n, out, cap);
  else if (enc == 3) utf8_copy(p, n, out, cap);
  else if (enc == 1) {
    bool be = false;
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { be = true; p += 2; n -= 2; }
    else if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { p += 2; n -= 2; }
    utf16_to_utf8(p, n, be, out, cap);
  } else if (enc == 2) {
    utf16_to_utf8(p, n, true, out, cap);
  }
  trim(out);
}

/* Skips an encoded, NUL-terminated string; returns bytes used (or n). */
static size_t id3_skip_str(const u8 *p, size_t n, u8 enc) {
  if (enc == 1 || enc == 2) {
    for (size_t i = 0; i + 1 < n; i += 2)
      if (p[i] == 0 && p[i + 1] == 0) return i + 2;
    return n;
  }
  for (size_t i = 0; i < n; i++)
    if (p[i] == 0) return i + 1;
  return n;
}

static void take_cover(FmAudMeta *m, const u8 *data, size_t n, int pic_type, int *best) {
  if (n < 8 || n > AUD_COVER_MAX) return;
  int score = pic_type == 3 ? 2 : 1;
  if (score <= *best) return;
  fm_free(m->cover);
  m->cover = (u8 *)fm_alloc(n);
  memcpy(m->cover, data, n);
  m->cover_len = n;
  *best = score;
}

bool aud_meta_id3v2(const u8 *t, size_t n, FmAudMeta *m, bool want_cover) {
  if (n < 10 || memcmp(t, "ID3", 3) != 0) return false;
  int ver = t[3];
  u8 flags = t[5];
  size_t size = syncsafe(t + 6);
  if (ver < 2 || ver > 4) return false;
  if (size > n - 10) size = n - 10;
  u8 *b = (u8 *)fm_alloc(size + 1);
  memcpy(b, t + 10, size);
  if ((flags & 0x80) && ver < 4) size = unsync(b, size);
  size_t p = 0;
  if ((flags & 0x40) && ver >= 3 && size >= 4) {
    size_t ext = ver == 4 ? syncsafe(b) : ((size_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]) + 4;
    p = ext <= size ? ext : size;
  }
  int best = 0;
  bool found = false;
  size_t hdr = ver == 2 ? 6 : 10;
  while (p + hdr <= size) {
    const u8 *h = b + p;
    if (h[0] == 0) break;                                /* padding */
    char id[5] = { 0 };
    size_t fs;
    u8 fflags = 0;
    if (ver == 2) {
      memcpy(id, h, 3);
      fs = (size_t)h[3] << 16 | h[4] << 8 | h[5];
    } else {
      memcpy(id, h, 4);
      fs = ver == 4 ? syncsafe(h + 4) : ((size_t)h[4] << 24 | h[5] << 16 | h[6] << 8 | h[7]);
      fflags = h[9];
    }
    p += hdr;
    if (fs > size - p) break;
    u8 *d = b + p;
    size_t dn = fs;
    p += fs;
    if (ver == 4) {
      if (fflags & 0x0C) continue;                       /* compressed / encrypted */
      if (fflags & 0x01) { if (dn < 4) continue; d += 4; dn -= 4; }
      if (fflags & 0x02) dn = unsync(d, dn);
    } else if (ver == 3 && (h[9] & 0xC0)) {
      continue;
    }
    if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) { id3_text(d, dn, m->title, sizeof m->title); found = true; }
    else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) { id3_text(d, dn, m->artist, sizeof m->artist); found = true; }
    else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) { id3_text(d, dn, m->album, sizeof m->album); found = true; }
    else if (want_cover && (!strcmp(id, "APIC") || !strcmp(id, "PIC")) && dn > 4) {
      u8 enc = d[0];
      size_t q = 1;
      if (ver == 2) q += 3;                              /* image format "JPG" */
      else q += id3_skip_str(d + q, dn - q, 0);          /* MIME type */
      if (q >= dn) continue;
      int ptype = d[q++];
      if (q >= dn) continue;
      q += id3_skip_str(d + q, dn - q, enc);             /* description */
      if (q < dn) { take_cover(m, d + q, dn - q, ptype, &best); found = true; }
    }
  }
  fm_free(b);
  return found;
}

static bool meta_id3v1(FILE *f, FmAudMeta *m) {
  u8 t[128];
  i64 sz = fm_fsize(f);
  if (sz < 128 || fm_fseek64(f, sz - 128, SEEK_SET) != 0 || fread(t, 1, 128, f) != 128) return false;
  if (memcmp(t, "TAG", 3) != 0) return false;
  char tmp[64];
  if (!m->title[0]) { memcpy(tmp, t + 3, 30); tmp[30] = 0; latin1_to_utf8((u8 *)tmp, 30, m->title, sizeof m->title); trim(m->title); }
  if (!m->artist[0]) { memcpy(tmp, t + 33, 30); tmp[30] = 0; latin1_to_utf8((u8 *)tmp, 30, m->artist, sizeof m->artist); trim(m->artist); }
  if (!m->album[0]) { memcpy(tmp, t + 63, 30); tmp[30] = 0; latin1_to_utf8((u8 *)tmp, 30, m->album, sizeof m->album); trim(m->album); }
  return m->title[0] != 0;
}

/* ---- Vorbis comments and FLAC pictures ------------------------------------------------ */

static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u32 be32(const u8 *p) { return (u32)p[3] | (u32)p[2] << 8 | (u32)p[1] << 16 | (u32)p[0] << 24; }

static bool flac_picture(const u8 *d, size_t n, FmAudMeta *m, int *best) {
  if (n < 32) return false;
  u32 type = be32(d);
  size_t p = 4;
  u32 ml = be32(d + p); p += 4;
  if (ml > n - p) return false;
  p += ml;
  if (p + 4 > n) return false;
  u32 dl = be32(d + p); p += 4;
  if (dl > n - p) return false;
  p += dl;
  if (p + 20 > n) return false;
  p += 16;
  u32 len = be32(d + p); p += 4;
  if (len > n - p) return false;
  take_cover(m, d + p, len, (int)type, best);
  return true;
}

static int b64v(u8 c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static size_t b64_decode(const u8 *s, size_t n, u8 *out) {
  u32 acc = 0;
  int bits = 0;
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    int v = b64v(s[i]);
    if (v < 0) continue;
    acc = acc << 6 | (u32)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[o++] = (u8)(acc >> bits);
    }
  }
  return o;
}

static bool vorbis_comments(const u8 *d, size_t n, FmAudMeta *m, bool want_cover, int *best) {
  if (n < 8) return false;
  size_t p = 0;
  u32 vl = le32(d);
  if (vl > n - 4) return false;
  p = 4 + vl;
  if (p + 4 > n) return false;
  u32 cnt = le32(d + p);
  p += 4;
  bool found = false;
  for (u32 i = 0; i < cnt && p + 4 <= n; i++) {
    u32 l = le32(d + p);
    p += 4;
    if (l > n - p) break;
    const u8 *c = d + p;
    p += l;
    const u8 *eq = (const u8 *)memchr(c, '=', l);
    if (!eq) continue;
    size_t kl = (size_t)(eq - c), vlen = l - kl - 1;
    const u8 *v = eq + 1;
    char *dst = NULL;
    if (kl == 5 && fm_strnicmp((const char *)c, "TITLE", 5) == 0) dst = m->title;
    else if (kl == 6 && fm_strnicmp((const char *)c, "ARTIST", 6) == 0) dst = m->artist;
    else if (kl == 5 && fm_strnicmp((const char *)c, "ALBUM", 5) == 0) dst = m->album;
    if (dst && !dst[0]) {
      utf8_copy(v, vlen, dst, AUD_META_TEXT);
      trim(dst);
      found = true;
    } else if (want_cover && kl == 22 && fm_strnicmp((const char *)c, "METADATA_BLOCK_PICTURE", 22) == 0) {
      u8 *bin = (u8 *)fm_alloc(vlen / 4 * 3 + 4);
      size_t bn = b64_decode(v, vlen, bin);
      if (flac_picture(bin, bn, m, best)) found = true;
      fm_free(bin);
    }
  }
  return found;
}

static bool meta_flac(FILE *f, FmAudMeta *m, bool want_cover) {
  u8 h[4];
  if (fread(h, 1, 4, f) != 4 || memcmp(h, "fLaC", 4) != 0) return false;
  bool found = false;
  int best = 0;
  for (int guard = 0; guard < 1024; guard++) {
    if (fread(h, 1, 4, f) != 4) break;
    bool last = (h[0] & 0x80) != 0;
    int type = h[0] & 0x7F;
    size_t len = (size_t)h[1] << 16 | h[2] << 8 | h[3];
    if ((type == 4 || (type == 6 && want_cover)) && len <= AUD_COVER_MAX + 4096) {
      u8 *d = (u8 *)fm_alloc(len ? len : 1);
      if (fread(d, 1, len, f) != len) { fm_free(d); break; }
      if (type == 4) found |= vorbis_comments(d, len, m, want_cover, &best);
      else found |= flac_picture(d, len, m, &best);
      fm_free(d);
    } else if (fm_fseek64(f, (i64)len, SEEK_CUR) != 0) {
      break;
    }
    if (last) break;
  }
  return found;
}

/* Reassembles the second packet of the first Ogg stream (the comment header). */
static bool meta_ogg(FILE *f, FmAudMeta *m, bool want_cover) {
  u8 *pkt = NULL;
  size_t pn = 0, pcap = 0;
  int packet = 0;
  u32 serial = 0;
  bool have_serial = false, done = false;
  size_t limit = want_cover ? AUD_COVER_MAX + 65536 : 256 * 1024;
  for (int pages = 0; pages < 4096 && !done; pages++) {
    u8 h[27], seg[255];
    if (fread(h, 1, 27, f) != 27 || memcmp(h, "OggS", 4) != 0) break;
    int ns = h[26];
    if (fread(seg, 1, (size_t)ns, f) != (size_t)ns) break;
    u32 ser = le32(h + 14);
    if (!have_serial) { serial = ser; have_serial = true; }
    size_t body = 0;
    for (int i = 0; i < ns; i++) body += seg[i];
    if (ser != serial) {
      if (fm_fseek64(f, (i64)body, SEEK_CUR) != 0) break;
      continue;
    }
    for (int i = 0; i < ns && !done; i++) {
      size_t l = seg[i];
      if (packet == 1 && pn + l <= limit) {
        if (pn + l > pcap) {
          pcap = FM_MAX(pcap * 2, pn + l + 4096);
          pkt = (u8 *)fm_realloc(pkt, pcap);
        }
        if (fread(pkt + pn, 1, l, f) != l) { done = true; break; }
        pn += l;
      } else if (l && fm_fseek64(f, (i64)l, SEEK_CUR) != 0) {
        done = true;
        break;
      }
      if (l < 255) {                           /* packet ends here */
        if (packet == 1) done = true;
        packet++;
      }
    }
  }
  bool found = false;
  int best = 0;
  if (pkt && pn > 8) {
    if (pn > 7 && memcmp(pkt, "\x03vorbis", 7) == 0) found = vorbis_comments(pkt + 7, pn - 7, m, want_cover, &best);
    else if (memcmp(pkt, "OpusTags", 8) == 0) found = vorbis_comments(pkt + 8, pn - 8, m, want_cover, &best);
  }
  fm_free(pkt);
  return found;
}

/* ---- MP4 (m4a) ---------------------------------------------------------------------- */

/* Finds a child atom of type `t` in [start, end); returns its payload range. */
static bool mp4_find(FILE *f, i64 start, i64 end, const char *t, i64 *ps, i64 *pe) {
  i64 p = start;
  while (p + 8 <= end) {
    u8 h[16];
    if (fm_fseek64(f, p, SEEK_SET) != 0 || fread(h, 1, 8, f) != 8) return false;
    u64 sz = be32(h);
    i64 hl = 8;
    if (sz == 1) {
      if (fread(h + 8, 1, 8, f) != 8) return false;
      sz = (u64)be32(h + 8) << 32 | be32(h + 12);
      hl = 16;
    } else if (sz == 0) {
      sz = (u64)(end - p);
    }
    if (sz < (u64)hl || (i64)sz > end - p) return false;
    if (memcmp(h + 4, t, 4) == 0) {
      *ps = p + hl;
      *pe = p + (i64)sz;
      return true;
    }
    p += (i64)sz;
  }
  return false;
}

static bool meta_mp4(FILE *f, FmAudMeta *m, bool want_cover) {
  i64 end = fm_fsize(f), s, e, s2, e2;
  if (end <= 0) return false;
  if (!mp4_find(f, 0, end, "moov", &s, &e)) return false;
  if (!mp4_find(f, s, e, "udta", &s2, &e2)) return false;
  if (!mp4_find(f, s2, e2, "meta", &s, &e)) return false;
  if (!mp4_find(f, s + 4, e, "ilst", &s2, &e2)) return false;     /* meta is a full box */
  static const struct { const char *atom; int field; } kTags[] = {
    { "\xA9nam", 0 }, { "\xA9""ART", 1 }, { "aART", 1 }, { "\xA9""alb", 2 }, { "covr", 3 },
  };
  bool found = false;
  int best = 0;
  for (int i = 0; i < FM_COUNT(kTags); i++) {
    if (kTags[i].field == 3 && !want_cover) continue;
    if (!mp4_find(f, s2, e2, kTags[i].atom, &s, &e)) continue;
    i64 ds, de;
    if (!mp4_find(f, s, e, "data", &ds, &de) || de - ds < 8) continue;
    size_t n = (size_t)(de - ds - 8);
    if (n > AUD_COVER_MAX) continue;
    u8 *d = (u8 *)fm_alloc(n + 1);
    if (fm_fseek64(f, ds + 8, SEEK_SET) == 0 && fread(d, 1, n, f) == n) {
      char *dst = kTags[i].field == 0 ? m->title : kTags[i].field == 1 ? m->artist : m->album;
      if (kTags[i].field == 3) take_cover(m, d, n, 3, &best);
      else if (!dst[0]) { utf8_copy(d, n, dst, AUD_META_TEXT); trim(dst); }
      found = true;
    }
    fm_free(d);
  }
  return found;
}

/* ---- RIFF INFO --------------------------------------------------------------------- */

static bool meta_wav(FILE *f, FmAudMeta *m) {
  u8 h[12];
  if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) != 0) return false;
  i64 end = fm_fsize(f), p = 12;
  bool found = false;
  while (p + 8 <= end) {
    u8 c[8];
    if (fm_fseek64(f, p, SEEK_SET) != 0 || fread(c, 1, 8, f) != 8) break;
    u32 len = le32(c + 4);
    if (memcmp(c, "LIST", 4) == 0 && len >= 4 && len < 1024 * 1024) {
      u8 *d = (u8 *)fm_alloc(len);
      if (fread(d, 1, len, f) == len && memcmp(d, "INFO", 4) == 0) {
        size_t q = 4;
        while (q + 8 <= len) {
          u32 sl = le32(d + q + 4);
          if (sl > len - q - 8) break;
          char *dst = !memcmp(d + q, "INAM", 4) ? m->title : !memcmp(d + q, "IART", 4) ? m->artist
                    : !memcmp(d + q, "IPRD", 4) ? m->album : NULL;
          if (dst) { utf8_copy(d + q + 8, sl, dst, AUD_META_TEXT); trim(dst); found = true; }
          q += 8 + sl + (sl & 1);
        }
      }
      fm_free(d);
    }
    p += 8 + (i64)len + (len & 1);
  }
  return found;
}

/* ---- entry --------------------------------------------------------------------------- */

bool aud_meta(const char *path, FmAudMeta *m, bool want_cover) {
  memset(m, 0, sizeof *m);
  FILE *f = fm_fopen(path, "rb");
  if (!f) return false;
  u8 h[16];
  size_t hn = fread(h, 1, sizeof h, f);
  fm_fseek64(f, 0, SEEK_SET);
  bool found = false;
  if (hn >= 10 && memcmp(h, "ID3", 3) == 0) {
    size_t size = syncsafe(h + 6) + 10;
    if (!want_cover && size > 1024 * 1024) size = 1024 * 1024;
    if (size > AUD_COVER_MAX + 65536) size = AUD_COVER_MAX + 65536;
    u8 *t = (u8 *)fm_alloc(size);
    size_t got = fread(t, 1, size, f);
    found = aud_meta_id3v2(t, got, m, want_cover);
    fm_free(t);
    if (!m->title[0]) found |= meta_id3v1(f, m);
  } else if (hn >= 4 && memcmp(h, "fLaC", 4) == 0) {
    found = meta_flac(f, m, want_cover);
  } else if (hn >= 4 && memcmp(h, "OggS", 4) == 0) {
    found = meta_ogg(f, m, want_cover);
  } else if (hn >= 12 && memcmp(h + 4, "ftyp", 4) == 0) {
    found = meta_mp4(f, m, want_cover);
  } else if (hn >= 12 && memcmp(h, "RIFF", 4) == 0) {
    found = meta_wav(f, m);
  } else {
    found = meta_id3v1(f, m);
  }
  fclose(f);
  return found;
}

void aud_meta_free(FmAudMeta *m) {
  fm_free(m->cover);
  m->cover = NULL;
  m->cover_len = 0;
}
