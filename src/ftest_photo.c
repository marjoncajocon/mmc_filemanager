/* ftest_photo.c -- the online photos view: the justified rows, the albums
** file, recent searches.
**
** The view needs a window; what can go wrong without one is the row
** layout (rows that do not fill the width, heights far from the target,
** panoramas and the last row), the albums store (a round trip through its
** file, odd characters in fields, the order, favourites) and the recent
** list. The albums run on a file in the test folder, never the user's.
*/
#include "ftest.h"
#include "fphoto_int.h"

static void check_rows(const float *ars, int n, float W, float target, float gap) {
  PhRow rows[64];
  int nr = wall_justify(ars, n, W, target, gap, rows, 64);
  TEST_CHECK(nr > 0);
  int seen = 0;
  for (int r = 0; r < nr; r++) {
    TEST_CHECK(rows[r].first == seen);
    TEST_CHECK(rows[r].count > 0);
    seen += rows[r].count;
    float sum = 0;
    for (int k = 0; k < rows[r].count; k++) sum += FM_CLAMP(ars[rows[r].first + k], 0.42f, 3.2f);
    float wsum = sum * rows[r].h + gap * (float)(rows[r].count - 1);
    if (rows[r].full) {
      /* a full row fills the width (heights are rounded to whole pixels) */
      if (fabsf(wsum - W) > sum + 1) printf("  row %d: width %.1f, want %.1f\n", r, wsum, W);
      TEST_CHECK(fabsf(wsum - W) <= sum + 1);
      TEST_CHECK(rows[r].h > target * 0.55f && rows[r].h < target * 1.7f);
    } else {
      TEST_CHECK(r == nr - 1);               /* only the last row may be short */
      TEST_CHECK(fabsf(rows[r].h - target) < 1);
      TEST_CHECK(wsum <= W + 1);
    }
    if (r > 0) TEST_CHECK(rows[r].y > rows[r - 1].y);
  }
  TEST_CHECK(seen == n);
}

static void make_item(PhItem *p, const char *src, const char *id, const char *title, int w, int h) {
  FmPsrcItem it;
  memset(&it, 0, sizeof it);
  fm_strlcpy(it.id, id, sizeof it.id);
  fm_strlcpy(it.title, title, sizeof it.title);
  fm_strlcpy(it.author, "Ada", sizeof it.author);
  fm_strlcpy(it.license, "CC BY 4.0", sizeof it.license);
  fm_snprintf(it.thumb, sizeof it.thumb, "https://example.org/%s/thumb.jpg", id);
  fm_snprintf(it.full, sizeof it.full, "https://example.org/%s/full.jpg", id);
  fm_snprintf(it.page, sizeof it.page, "https://example.org/%s", id);
  fm_snprintf(it.original, sizeof it.original, "https://example.org/%s/original.tif", id);
  it.width = w;
  it.height = h;
  it.color = 0x336699;
  ph_item_init(p, &it, src);
}

int test_photo_ui(const char *tmp) {
  /* rows: mixed shapes, all the same, a panorama, a lone last photo */
  static const float kMix[] = { 1.5f, 0.75f, 1.33f, 1.0f, 1.78f, 0.67f, 2.4f, 1.25f, 0.8f, 1.6f, 1.33f, 0.56f,
                                1.5f, 1.5f, 1.0f, 0.75f, 3.9f, 1.2f, 1.33f, 1.5f, 0.7f };
  check_rows(kMix, FM_COUNT(kMix), 1000, 180, 6);
  check_rows(kMix, FM_COUNT(kMix), 380, 120, 3);
  static const float kSame[] = { 1.5f, 1.5f, 1.5f, 1.5f, 1.5f, 1.5f, 1.5f, 1.5f, 1.5f, 1.5f };
  check_rows(kSame, FM_COUNT(kSame), 1200, 180, 6);
  static const float kPano[] = { 12.0f, 1.0f };
  check_rows(kPano, 2, 900, 180, 6);
  static const float kOne[] = { 1.33f };
  check_rows(kOne, 1, 900, 180, 6);

  /* items: the source key travels with the item, the size gives the shape */
  PhItem a, b, c;
  make_item(&a, "openverse", "ov-1", "Red\tbarn\nat dusk", 4000, 3000);
  make_item(&b, "nasa", "as11-40-5903", "Buzz Aldrin on the Moon", 0, 0);
  make_item(&c, "wikimedia", "File:Tower.jpg", "Tower", 2000, 3000);
  TEST_CHECK(strcmp(a.it.source, "openverse") == 0 && strcmp(a.src, "openverse") == 0);
  TEST_CHECK(fabsf(a.ar - 4.0f / 3.0f) < 0.001f);
  TEST_CHECK(b.ar == 0);
  char meta[200];
  ph_meta(&a, meta, sizeof meta, false);
  TEST_CHECK(strcmp(meta, "by Ada  \xC2\xB7  CC BY 4.0") == 0);

  /* albums: a round trip through the file */
  char path[FM_PATH_MAX];
  fm_path_join(path, sizeof path, tmp, "photo-albums.txt");
  palb_test_file(path);
  palb_load(false);
  TEST_CHECK(palb_count() == 1 && palb_size(PALB_FAV) == 0);
  palb_fav_toggle(&a);
  palb_fav_toggle(&b);
  TEST_CHECK(palb_is_fav(&a) && palb_is_fav(&b) && !palb_is_fav(&c));
  int trips = palb_create("  Trips  ");
  TEST_CHECK(trips == 1 && strcmp(palb_name(trips), "Trips") == 0);
  TEST_CHECK(palb_create("trips") == 2 && strcmp(palb_name(2), "trips (2)") == 0);
  TEST_CHECK(palb_add(trips, &c));
  TEST_CHECK(palb_add(trips, &a));
  TEST_CHECK(!palb_add(trips, &a));             /* already there */
  TEST_CHECK(palb_size(trips) == 2);
  TEST_CHECK(palb_find(trips, "openverse", "ov-1") == 0);   /* newest first */
  TEST_CHECK(strcmp(palb_cover(trips), a.it.thumb) == 0);
  palb_pump();                                  /* saves */
  palb_shutdown();
  TEST_CHECK(palb_count() == 0);
  palb_load(false);
  TEST_CHECK(palb_count() == 3);
  TEST_CHECK(palb_size(PALB_FAV) == 2 && palb_size(trips) == 2);
  TEST_CHECK(palb_find_name("Trips") == trips && palb_find_name("trips (2)") == 2);
  PhItem g;
  palb_get(trips, 0, &g);
  TEST_CHECK(strcmp(g.it.id, "ov-1") == 0);
  TEST_CHECK(strcmp(g.it.title, "Red barn at dusk") == 0);   /* no tabs or breaks in a line */
  TEST_CHECK(strcmp(g.src, "openverse") == 0 && strcmp(g.it.source, "openverse") == 0);
  TEST_CHECK(strcmp(g.it.original, a.it.original) == 0);
  TEST_CHECK(strcmp(g.it.page, a.it.page) == 0 && strcmp(g.it.license, "CC BY 4.0") == 0);
  TEST_CHECK(g.it.width == 4000 && g.it.height == 3000 && g.it.color == 0x336699);
  palb_get(trips, 1, &g);
  TEST_CHECK(strcmp(g.it.id, "File:Tower.jpg") == 0 && fabsf(g.ar - 2.0f / 3.0f) < 0.001f);
  palb_remove(trips, "openverse", "ov-1");
  TEST_CHECK(palb_size(trips) == 1);
  palb_fav_toggle(&a);                          /* off again */
  TEST_CHECK(!palb_is_fav(&a) && palb_size(PALB_FAV) == 1);
  palb_shutdown();
  palb_load(false);
  TEST_CHECK(palb_size(trips) == 1 && palb_size(PALB_FAV) == 1);
  palb_shutdown();
  palb_test_file(NULL);

  /* recent searches: newest first, no duplicates, capped; read-only here */
  precent_load(true);
  TEST_CHECK(precent_count() == 0);
  precent_add("  mountains ");
  precent_add("cats");
  precent_add("Mountains");
  TEST_CHECK(precent_count() == 2 && strcmp(precent_at(0), "Mountains") == 0);
  char q[32];
  for (int i = 0; i < 14; i++) {
    fm_snprintf(q, sizeof q, "q%d", i);
    precent_add(q);
  }
  TEST_CHECK(precent_count() == PRECENT_MAX && strcmp(precent_at(0), "q13") == 0);
  precent_clear();
  TEST_CHECK(precent_count() == 0);
  return g_test_fail;
}
