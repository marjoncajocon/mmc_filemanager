/* fdatause.h -- the data usage meter: what the network moved, per day and
** per group (online videos, audio, photos, reading, cloud, other), kept in
** PLACE_CONFIG/data-usage.txt. fnet counts the bytes (net_traffic); this
** folds them into days on the main thread.
*/
#ifndef FDATAUSE_H
#define FDATAUSE_H

#include "fcore.h"
#include "fnet.h"

typedef struct FmDataUse {
  u64 today[NET_GROUPS];
  u64 month[NET_GROUPS];        /* this calendar month */
  u64 all[NET_GROUPS];          /* since `since` (the last clear) */
  char since[16];               /* "2026-10-11" */
} FmDataUse;

void datause_init(void);
/* Main thread, every loop (also in the background): cheap; saves every 30 s
** while data moves, and when the day changes. */
void datause_pump(void);
void datause_shutdown(void);    /* saves */
void datause_get(FmDataUse *u);
void datause_clear(void);       /* everything back to 0, from today */
const char *datause_group_name(int g);

#endif
