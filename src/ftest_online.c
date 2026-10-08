/* ftest_online.c -- the online videos view: number and date formatting,
** the recent-searches list.
**
** The view itself needs a window; what can go wrong without one is the
** text on the cards ("1.2M views", "3 years ago", "1:02:03") and the
** recent list (order, duplicates, the cap). The list runs read-only here,
** so the user's own online.txt is never touched.
*/
#include "ftest.h"
#include "fonline_int.h"

static void check_views(i64 v, const char *want) {
  char b[48];
  ofmt_views(v, b, sizeof b);
  if (strcmp(b, want) != 0) printf("  views %lld -> \"%s\", want \"%s\"\n", (long long)v, b, want);
  TEST_CHECK(strcmp(b, want) == 0);
}

static void check_dur(double s, const char *want) {
  char b[32];
  ofmt_dur(s, b, sizeof b);
  TEST_CHECK(strcmp(b, want) == 0);
}

int test_online_ui(const char *tmp) {
  FM_UNUSED(tmp);
  check_views(-1, "");
  check_views(0, "0 views");
  check_views(1, "1 view");
  check_views(999, "999 views");
  check_views(1000, "1K views");
  check_views(1250, "1.2K views");
  check_views(12500, "12K views");
  check_views(999999, "999K views");
  check_views(1200000, "1.2M views");
  check_views(57000000, "57M views");
  check_views(3400000000ll, "3.4B views");
  check_views(5000000000000ll, "5000B views");

  check_dur(0, "0:00");
  check_dur(65, "1:05");
  check_dur(3723, "1:02:03");
  check_dur(359999, "99:59:59");

  /* ages relative to now, built from today's date */
  char iso[32], age[48];
  i64 now = plat_time_unix();
  i64 day = now / 86400;
  {
    /* civil date of `day` days since 1970 (same algorithm as the adapters) */
    i64 z = day + 719468, era = z / 146097, doe = z - era * 146097;
    i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, y = yoe + era * 400;
    i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1), m = (int)(mp < 10 ? mp + 3 : mp - 9);
    if (m <= 2) y++;
    fm_snprintf(iso, sizeof iso, "%04d-%02d-%02d", (int)y, m, d);
    ofmt_age(iso, age, sizeof age);
    TEST_CHECK(strcmp(age, "today") == 0);
    fm_snprintf(iso, sizeof iso, "%04d%02d%02d", (int)y - 3, m, d == 29 && m == 2 ? 28 : d);
    ofmt_age(iso, age, sizeof age);
    TEST_CHECK(strcmp(age, "3 years ago") == 0);
  }
  ofmt_age("", age, sizeof age);
  TEST_CHECK(age[0] == 0);
  ofmt_age("garbage", age, sizeof age);
  TEST_CHECK(age[0] == 0);
  ofmt_age("1999-13-01", age, sizeof age);
  TEST_CHECK(age[0] == 0);

  /* card meta line */
  FmVsrcItem it;
  memset(&it, 0, sizeof it);
  fm_strlcpy(it.channel, "NASA", sizeof it.channel);
  it.views = 1600000;
  char meta[256];
  ofmt_meta(&it, meta, sizeof meta);
  TEST_CHECK(strcmp(meta, "NASA  \xC2\xB7  1.6M views") == 0);
  it.live = true;
  it.views = -1;
  ofmt_meta(&it, meta, sizeof meta);
  TEST_CHECK(strcmp(meta, "NASA  \xC2\xB7  Live now") == 0);

  /* recent searches: newest first, no duplicates (any case), capped */
  orecent_load(true);
  TEST_CHECK(orecent_count() == 0);
  orecent_add("  cats  ");
  orecent_add("dogs");
  orecent_add("Cats");
  TEST_CHECK(orecent_count() == 2);
  TEST_CHECK(strcmp(orecent_at(0), "Cats") == 0);
  TEST_CHECK(strcmp(orecent_at(1), "dogs") == 0);
  orecent_add("   ");
  TEST_CHECK(orecent_count() == 2);
  char q[32];
  for (int i = 0; i < 15; i++) {
    fm_snprintf(q, sizeof q, "query %d", i);
    orecent_add(q);
  }
  TEST_CHECK(orecent_count() == ORECENT_MAX);
  TEST_CHECK(strcmp(orecent_at(0), "query 14") == 0);
  TEST_CHECK(strcmp(orecent_at(ORECENT_MAX - 1), "query 5") == 0);
  orecent_add("query 9");                  /* moves to the front, nothing dropped */
  TEST_CHECK(orecent_count() == ORECENT_MAX);
  TEST_CHECK(strcmp(orecent_at(0), "query 9") == 0);
  TEST_CHECK(strcmp(orecent_at(1), "query 14") == 0);
  orecent_remove(0);
  TEST_CHECK(strcmp(orecent_at(0), "query 14") == 0);
  orecent_clear();
  TEST_CHECK(orecent_count() == 0);
  return g_test_fail;
}
