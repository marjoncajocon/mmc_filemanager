/* fdatause.c -- the data usage meter (see fdatause.h).
**
** Design decisions:
**   - fnet counts every byte received or sent under the tag of the thread
**     that moves it (fm_thread_tag, inherited by the threads a view starts),
**     so nothing else in the program has to report its traffic. This file
**     only folds what fnet counted since the last look into today.
**   - One line per day ("d 20261011 b0 b1 ..." with a number per group), the
**     last 400 days; older days are folded into one "o" line, so the totals
**     since the last clear stay right while the file stays small.
**   - Saved every 30 s while data moves (a write a minute at most while
**     streaming), when the day changes, and at exit; the bytes of a crash's
**     last half minute are lost, which is fine for a meter.
*/
#include "fdatause.h"
#include "fplat.h"
#include "fsdl.h"
#include <time.h>

#define DU_DAYS 400
#define DU_SAVE_MS 30000

typedef struct DuDay { int day; u64 b[NET_GROUPS]; } DuDay;

static struct {
  bool init;
  DuDay d[DU_DAYS];
  int n;                       /* days kept, oldest first */
  u64 older[NET_GROUPS];       /* days folded out of d */
  int since;                   /* yyyymmdd of the last clear */
  u64 last[NET_GROUPS];        /* net_traffic at the last fold */
  bool dirty;
  u64 saved_at;
} D;

static int today(void) {
  time_t t = time(NULL);
  struct tm *lt = localtime(&t);
  if (!lt) return 20000101;
  return (lt->tm_year + 1900) * 10000 + (lt->tm_mon + 1) * 100 + lt->tm_mday;
}

static bool file_path(char *out, size_t cap) {
  char dir[FM_PATH_MAX];
  return plat_place(PLACE_CONFIG, dir, sizeof dir) && fm_path_join(out, cap, dir, "data-usage.txt");
}

static DuDay *day_slot(int day) {
  if (D.n && D.d[D.n - 1].day == day) return &D.d[D.n - 1];
  if (D.n == DU_DAYS) {                         /* the oldest day goes into "older" */
    for (int g = 0; g < NET_GROUPS; g++) D.older[g] += D.d[0].b[g];
    memmove(D.d, D.d + 1, sizeof D.d[0] * (DU_DAYS - 1));
    D.n--;
  }
  DuDay *s = &D.d[D.n++];
  memset(s, 0, sizeof *s);
  s->day = day;
  return s;
}

static void load(void) {
  char p[FM_PATH_MAX];
  D.since = today();
  if (!file_path(p, sizeof p)) return;
  FILE *f = fm_fopen(p, "rb");
  if (!f) return;
  char line[512];
  while (fgets(line, sizeof line, f)) {
    unsigned long long v[NET_GROUPS] = { 0 };
    int day = 0;
    if (!strncmp(line, "since ", 6)) {
      D.since = atoi(line + 6);
    } else if (line[0] == 'd' || line[0] == 'o') {
      const char *s = line + 1;
      if (line[0] == 'd') {
        while (*s == ' ') s++;
        day = atoi(s);
        while (*s && *s != ' ') s++;
      }
      for (int g = 0; g < NET_GROUPS; g++) {
        while (*s == ' ') s++;
        if (*s < '0' || *s > '9') break;
        while (*s >= '0' && *s <= '9') v[g] = v[g] * 10 + (unsigned long long)(*s++ - '0');   /* no strtoull in tcc's msvcrt */
      }
      if (line[0] == 'o') {
        for (int g = 0; g < NET_GROUPS; g++) D.older[g] += v[g];
      } else if (day > 19700000 && (!D.n || day > D.d[D.n - 1].day)) {
        DuDay *x = day_slot(day);
        for (int g = 0; g < NET_GROUPS; g++) x->b[g] = v[g];
      }
    }
  }
  fclose(f);
}

static void save(void) {
  char p[FM_PATH_MAX], tmp[FM_PATH_MAX];
  if (!file_path(p, sizeof p)) return;
  fm_snprintf(tmp, sizeof tmp, "%s.part", p);
  FILE *f = fm_fopen(tmp, "wb");
  if (!f) return;
  fprintf(f, "# mmcfm data usage: bytes per day for other, online videos, audio, photos, reading, cloud\n");
  fprintf(f, "since %d\n", D.since);
  fprintf(f, "o");
  for (int g = 0; g < NET_GROUPS; g++) fprintf(f, " %llu", (unsigned long long)D.older[g]);
  fprintf(f, "\n");
  for (int i = 0; i < D.n; i++) {
    fprintf(f, "d %d", D.d[i].day);
    for (int g = 0; g < NET_GROUPS; g++) fprintf(f, " %llu", (unsigned long long)D.d[i].b[g]);
    fprintf(f, "\n");
  }
  bool ok = fclose(f) == 0;
  if (ok) plat_remove_file(p);                  /* plat_rename never replaces: the old file goes first */
  if (!ok || plat_rename(tmp, p) != FM_OK) plat_remove_file(tmp);
  D.dirty = false;
  D.saved_at = SDL_GetTicks64();
}

void datause_init(void) {
  if (D.init) return;
  memset(&D, 0, sizeof D);
  D.init = true;
  load();
  net_traffic(D.last);
  D.saved_at = SDL_GetTicks64();
}

void datause_pump(void) {
  if (!D.init) return;
  u64 now[NET_GROUPS];
  net_traffic(now);
  int day = today();
  bool moved = false, new_day = !D.n || D.d[D.n - 1].day != day;
  for (int g = 0; g < NET_GROUPS && !moved; g++) moved = now[g] != D.last[g];
  if (moved) {
    DuDay *x = day_slot(day);
    for (int g = 0; g < NET_GROUPS; g++) {
      x->b[g] += now[g] - D.last[g];
      D.last[g] = now[g];
    }
    D.dirty = true;
  }
  if (D.dirty && (new_day || SDL_GetTicks64() - D.saved_at >= DU_SAVE_MS)) save();
}

void datause_shutdown(void) {
  if (!D.init) return;
  datause_pump();
  if (D.dirty) save();
}

void datause_get(FmDataUse *u) {
  memset(u, 0, sizeof *u);
  if (!D.init) return;
  int day = today(), month = day / 100;
  for (int g = 0; g < NET_GROUPS; g++) u->all[g] = D.older[g];
  for (int i = 0; i < D.n; i++)
    for (int g = 0; g < NET_GROUPS; g++) {
      u->all[g] += D.d[i].b[g];
      if (D.d[i].day / 100 == month) u->month[g] += D.d[i].b[g];
      if (D.d[i].day == day) u->today[g] += D.d[i].b[g];
    }
  fm_snprintf(u->since, sizeof u->since, "%04d-%02d-%02d", D.since / 10000, D.since / 100 % 100, D.since % 100);
}

void datause_clear(void) {
  if (!D.init) return;
  datause_pump();
  D.n = 0;
  memset(D.older, 0, sizeof D.older);
  D.since = today();
  save();
}

const char *datause_group_name(int g) {
  static const char *const kName[NET_GROUPS] = { "Other", "Online videos", "Online audio", "Online photos",
                                                 "Online reading", "Cloud storage" };
  return g >= 0 && g < NET_GROUPS ? kName[g] : "Other";
}
