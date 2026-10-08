/* flayout.c -- responsive layout: side by side, stacked or single panel.
**
** Design decisions:
**   - All sizes are dp so the same rules hold on a 4K desktop and a phone.
**   - The stacked layout keeps both panels at 40..60% of the free height:
**     a panel squeezed to a few rows is useless on a phone.
**   - The sidebar only appears above 1100 dp in landscape; narrower windows
**     reach the same places through the places popup.
*/
#include "flayout.h"
#include "fconf.h"

static float gap(void) { return DP(ui.w / ui.scale < 600 ? 6 : 10); }

int layout_resolve(void) {
  float wdp = ui.w / ui.scale;
  if (conf.layout == LAYOUT_SIDE || conf.layout == LAYOUT_STACK || conf.layout == LAYOUT_SINGLE)
    return conf.layout;
  if (wdp < 360) return LAYOUT_SINGLE;
  return ui.w >= ui.h ? LAYOUT_SIDE : LAYOUT_STACK;
}

void layout_compute(FmLayout *L, int single_tab, bool sidebar_open, float jobs_h, bool audio) {
  memset(L, 0, sizeof *L);
  float wdp = ui.w / ui.scale;
  L->mode = layout_resolve();
  L->narrow = wdp < 640;
  L->wide = wdp >= 1100 && ui.w > ui.h;
  FmRect r = { 0, 0, ui.w, ui.h };
  float g = gap();
  L->top = rect_cut_top(&r, ui.m.bar_h);
  L->status = rect_cut_bottom(&r, DP(ui.touch_mode ? 30 : 26));
  if (audio) L->audio = rect_cut_bottom(&r, DP(ui.touch_mode ? 64 : 56));
  if (jobs_h > 0.5f) {
    L->jobs = rect_cut_bottom(&r, jobs_h);
    L->jobs = rect_inset2(L->jobs, g, 0);
    L->jobs.y += DP(2);
    L->jobs.h -= DP(2);
  }
  r = rect_inset2(r, g, 0);
  rect_cut_top(&r, DP(2));
  rect_cut_bottom(&r, g * 0.6f);
  if (L->wide && sidebar_open && L->mode == LAYOUT_SIDE) {
    L->sidebar = true;
    L->side = rect_cut_left(&r, DP(224));
    rect_cut_left(&r, g);
  }
  L->content = r;
  float split = FM_CLAMP(conf.split, 0.2f, 0.8f);
  switch (L->mode) {
    case LAYOUT_SIDE: {
      float aw = DP(ui.touch_mode ? 68 : 60);
      if (r.w < DP(600)) aw = DP(54);
      float avail = r.w - aw;
      float minw = FM_MIN(DP(220), avail * 0.3f);
      float lw = FM_CLAMP(avail * split, minw, avail - minw);
      L->panel[0] = rect_cut_left(&r, lw);
      L->action = rect_cut_left(&r, aw);
      L->panel[1] = r;
      L->action_vertical = true;
      L->show[0] = L->show[1] = true;
      break;
    }
    case LAYOUT_STACK: {
      float ah = DP(ui.touch_mode ? 64 : 56);
      float avail = r.h - ah;
      float th = avail * FM_CLAMP(split, 0.4f, 0.6f);
      L->panel[0] = rect_cut_top(&r, th);
      L->action = rect_cut_top(&r, ah);
      L->panel[1] = r;
      L->show[0] = L->show[1] = true;
      break;
    }
    default: {
      L->tabs = rect_cut_top(&r, DP(ui.touch_mode ? 48 : 40));
      L->tabs = rect_inset2(L->tabs, 0, DP(4));
      rect_cut_top(&r, DP(4));
      L->action = rect_cut_bottom(&r, DP(ui.touch_mode ? 64 : 56));
      int t = single_tab ? 1 : 0;
      L->panel[t] = r;
      L->show[t] = true;
      break;
    }
  }
}

float layout_split_at(const FmLayout *L, float mx, float my) {
  FmRect c = L->content;
  float s = conf.split;
  if (L->mode == LAYOUT_SIDE) {
    float aw = L->action.w;
    float avail = c.w - aw;
    if (avail > 1) s = (mx - c.x - aw * 0.5f) / avail;
    return FM_CLAMP(s, 0.2f, 0.8f);
  }
  if (L->mode == LAYOUT_STACK) {
    float ah = L->action.h;
    float avail = c.h - ah;
    if (avail > 1) s = (my - c.y - ah * 0.5f) / avail;
    return FM_CLAMP(s, 0.4f, 0.6f);
  }
  return s;
}
