/* flyrics.h -- song lyrics from LRCLIB (lrclib.net: free, no key), synced
** to the time where it has them: the music and video players show the line
** being sung.
**
** One song at a time (the one playing); fetched on a worker thread, kept
** in a disk cache so a song is downloaded once.
*/
#ifndef FLYRICS_H
#define FLYRICS_H

#include "fcore.h"
#include "fgfx.h"

enum { LY_NONE, LY_LOADING, LY_SYNCED, LY_PLAIN, LY_MISSING, LY_ERROR };

/* Main thread: the lyrics of this song (title as shown: "Artist - Song
** (Official Video)" is understood; artist may be "" or a channel name);
** starts a fetch when the song changed. Returns LY_*. */
int  lyrics_want(const char *artist, const char *title, double duration);
const char *lyrics_message(void);       /* LY_MISSING / LY_ERROR: why */
const char *lyrics_song(void);          /* "Artist - Song" as found */
/* Draws them in r at playback position pos: synced, the line being sung
** in the middle and highlighted; plain, scrollable text. overlay: the
** video player's two lines (now and next) on a dark band. */
void lyrics_draw(FmRect r, double pos, bool overlay);
/* ms until the next line starts at pos (for a wake-up), -1 = none */
int  lyrics_next_ms(double pos);
void lyrics_shutdown(void);

/* For the self test: parse LRCLIB's JSON reply; the lines afterwards. */
int  lyrics_parse(const char *json, size_t len);   /* LY_* */
int  lyrics_count(void);
const char *lyrics_line(int i, double *t);          /* t = -1 for plain lyrics */
/* "Adele - Hello (Official Music Video)" -> artist "Adele", track "Hello" */
void lyrics_clean(const char *artist, const char *title, char *a, size_t acap, char *t, size_t tcap);

#endif
