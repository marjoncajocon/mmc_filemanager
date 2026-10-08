/* feq.h -- 10-band equalizer for the music and video players: presets,
** RBJ biquads, stereo width, balance and a soft limiter, plus its settings UI.
**
** One FmEqConf (conf.eq) drives both players. A player keeps an FmEq,
** calls eq_follow each frame on the main thread (it redesigns the filters
** only when the settings or the rate changed) and eq_process in its audio
** callback, under the same lock.
*/
#ifndef FEQ_H
#define FEQ_H

#include "fui.h"
#include "fconf.h"

enum {
  EQ_FLAT = 0, EQ_BASS, EQ_FULL_BASS, EQ_BASS_TREBLE, EQ_TREBLE, EQ_ROCK, EQ_POP, EQ_JAZZ,
  EQ_CLASSICAL, EQ_DANCE, EQ_HIPHOP, EQ_VOCAL, EQ_LOUDNESS, EQ_LAPTOP, EQ_HEADPHONES, EQ_CUSTOM,
  EQ_PRESETS
};

#define EQ_MAX_DB 12.0f

extern const float kEqFreq[EQ_BANDS];

const char *eq_preset_name(int preset);
/* Loads a preset's curve and preamp into c (EQ_CUSTOM: c->custom). */
void eq_preset(FmEqConf *c, int preset);
void eq_defaults(FmEqConf *c);
void eq_sanitize(FmEqConf *c);
/* Flat, centred and full width: the DSP is skipped. */
bool eq_is_flat(const FmEqConf *c);

/* Filter set designed for one sample rate (main thread). */
typedef struct FmEqCoef {
  bool active;
  u32 mask;                   /* bands that are not 0 dB */
  float k[EQ_BANDS][5];       /* b0 b1 b2 a1 a2, normalised */
  float pre;                  /* linear preamp */
  float width, gl, gr;        /* stereo width, balance gains */
  float rel;                  /* limiter release per sample */
} FmEqCoef;

/* Processing state of one player. */
typedef struct FmEq {
  FmEqCoef c;
  float z[EQ_BANDS][2][2];    /* per band, channel: transposed DF2 state */
  float lim;                  /* limiter gain */
  FmEqConf applied;           /* settings the filters were built from */
  int rate;
} FmEq;

void eq_design(const FmEqConf *c, int rate, FmEqCoef *out);
/* Response of the filters at f Hz, in dB (preamp included). */
float eq_response_db(const FmEqCoef *k, int rate, float f);
/* Main thread: redesigns when conf.eq or the rate changed and installs the
** result with mx held (the audio callback's lock). */
void eq_follow(FmEq *e, int rate, SDL_mutex *mx);
/* Audio thread: in place on interleaved stereo float. No-op when inactive. */
void eq_process(FmEq *e, float *lr, int frames);

/* Settings body (presets, band sliders over the response curve, preamp,
** width, balance) inside r; scrolls by itself. */
void eq_ui(FmRect r);

#endif
