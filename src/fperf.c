/* fperf.c -- `mmcfm --perf`: a scripted tour of the screens, measured.
**
** Design decisions:
**   - Frame CPU time is the wall time of one main-loop turn minus the time
**     spent inside SDL_RenderPresent (which waits for vsync), so it is the
**     work of building and submitting the frame. Process CPU per frame
**     (all threads: decoders, audio, the driver) comes from the OS.
**   - Every stage first warms up (thumbnails decode, dialogs finish their
**     open animation), then draws N measured frames with forced redraws.
**   - Heap is sampled while the screen is up and again after it closed, so
**     memory a screen keeps after closing shows up as a growing column.
**   - Process memory: K32GetProcessMemoryInfo (kernel32 on Windows 7+, else
**     psapi.dll), looked up at run time; /proc/self/status on Linux and
**     Android; 0 elsewhere.
*/
#include "fperf.h"
#include "fapp.h"
#include "fconf.h"
#include "fview.h"
#include "flib.h"
#include "flayout.h"
#include "fthumb.h"
#include "fonline.h"
#include <math.h>
#include <stdarg.h>

#ifdef FM_WIN
#  include "fwin.h"
#elif !defined(FM_WEB)
#  include <sys/resource.h>
#endif

#define PERF_MAX_FRAMES 4000
#define PERF_DIR "perf-tour2"
#define PERF_FILES 2000
#define PERF_PHOTOS 256
#define PERF_FOLDERS 8

/* ---- options and state ----------------------------------------------------------- */

static struct {
  bool on;
  int frames;
  const char *out;
  const char *shots;
  const char *only;
  bool probe;
  char dir[FM_PATH_MAX];
  char wav[FM_PATH_MAX];
  char mpg[FM_PATH_MAX];
  char **argv;
  int argc;
  /* startup marks */
  const char *mark_name[16];
  Uint64 mark_t[16];
  int nmarks;
  double gen_ms;
  void (*frame)(void);
  FILE *report;
  /* contact sheet */
  u8 *sheet;
  int sheet_w, sheet_h, sheet_cell_w, sheet_cell_h, sheet_n;
} P_;

static double now_ms(void) {
  return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

bool perf_wanted(int argc, char **argv) {
#ifdef FM_WEB
  FM_UNUSED(argc); FM_UNUSED(argv);
  return false;
#else
  for (int i = 1; i < argc; i++)
    if (!strcmp(argv[i], "--perf")) return true;
  return false;
#endif
}

void perf_mark(const char *what) {
  if (P_.nmarks < FM_COUNT(P_.mark_t)) {
    P_.mark_name[P_.nmarks] = what;
    P_.mark_t[P_.nmarks] = SDL_GetPerformanceCounter();
    P_.nmarks++;
  }
}

/* ---- process numbers -------------------------------------------------------------- */

#ifdef FM_WIN
typedef struct PerfPmc {
  DWORD cb, PageFaultCount;
  SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
         QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage,
         PrivateUsage;
} PerfPmc;
typedef BOOL (WINAPI *PmiFn)(HANDLE, PerfPmc *, DWORD);
#endif

/* Private (committed, not shared) and working-set bytes, in MB. */
static void proc_mem(double *priv, double *ws) {
  *priv = *ws = 0;
#ifdef FM_WIN
  static PmiFn fn;
  static int tried;
  if (!tried) {
    tried = 1;
    HMODULE k = GetModuleHandleA("kernel32.dll");
    if (k) fn = (PmiFn)(void *)GetProcAddress(k, "K32GetProcessMemoryInfo");
    if (!fn) {
      HMODULE ps = LoadLibraryA("psapi.dll");
      if (ps) fn = (PmiFn)(void *)GetProcAddress(ps, "GetProcessMemoryInfo");
    }
  }
  PerfPmc m;
  memset(&m, 0, sizeof m);
  m.cb = sizeof m;
  if (fn && fn(GetCurrentProcess(), &m, sizeof m)) {
    *priv = (double)m.PrivateUsage / (1024.0 * 1024.0);
    *ws = (double)m.WorkingSetSize / (1024.0 * 1024.0);
  }
#elif defined(FM_LINUX) || defined(FM_ANDROID)
  FILE *f = fopen("/proc/self/status", "r");
  if (!f) return;
  char line[256];
  while (fgets(line, sizeof line, f)) {
    long kb = 0;
    if (sscanf(line, "VmRSS: %ld", &kb) == 1) *ws = kb / 1024.0;
    else if (sscanf(line, "RssAnon: %ld", &kb) == 1) *priv = kb / 1024.0;
  }
  fclose(f);
#endif
}

/* CPU time of the whole process (all threads), ms. */
static double proc_cpu_ms(void) {
#ifdef FM_WIN
  FILETIME c, e, k, u;
  if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
  u64 kk = ((u64)k.dwHighDateTime << 32) | k.dwLowDateTime;
  u64 uu = ((u64)u.dwHighDateTime << 32) | u.dwLowDateTime;
  return (double)(kk + uu) / 10000.0;
#elif defined(FM_WEB)
  return 0;
#else
  struct rusage ru;
  if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
  return (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000.0 +
         (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000.0;
#endif
}

/* ---- report output ------------------------------------------------------------------ */

static void out(const char *fmt, ...) FM_PRINTF(1, 2);
static void out(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  fputs(buf, stdout);
  fflush(stdout);
  if (P_.report) fputs(buf, P_.report);
}

/* ---- generated media ------------------------------------------------------------------ */

static void put16le(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32le(u8 *p, u32 v) { put16le(p, v); put16le(p + 2, v >> 16); }

static bool write_file(const char *path, const void *data, size_t n) {
  FILE *f = fm_fopen(path, "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, n, f) == n;
  return fclose(f) == 0 && ok;
}

/* 24-bit BMP "photo": a soft two-colour gradient with a sun, hue from seed. */
static bool make_photo(const char *path, int seed) {
  enum { W = 160, H = 120, STRIDE = W * 3 };
  static u8 buf[54 + STRIDE * H];
  memset(buf, 0, 54);
  buf[0] = 'B'; buf[1] = 'M';
  put32le(buf + 2, sizeof buf);
  put32le(buf + 10, 54);
  put32le(buf + 14, 40);
  put32le(buf + 18, W);
  put32le(buf + 22, H);          /* bottom-up */
  put16le(buf + 26, 1);
  put16le(buf + 28, 24);
  put32le(buf + 34, STRIDE * H);
  float hue = (float)(seed * 37 % 360) * 3.14159265f / 180.0f;
  float cr = 0.5f + 0.5f * cosf(hue), cg = 0.5f + 0.5f * cosf(hue + 2.1f), cb = 0.5f + 0.5f * cosf(hue + 4.2f);
  float sx = (float)(20 + seed * 13 % 120), sy = (float)(20 + seed * 7 % 60);
  for (int y = 0; y < H; y++) {
    u8 *row = buf + 54 + (size_t)(H - 1 - y) * STRIDE;
    for (int x = 0; x < W; x++) {
      float t = (float)y / H;
      float r = cr * (1 - t) + 0.15f * t, g = cg * (1 - t) + 0.2f * t, b = cb * (1 - t) + 0.3f * t;
      float dx = (float)x - sx, dy = (float)y - sy;
      if (dx * dx + dy * dy < 140.0f) { r = 1.0f; g = 0.92f; b = 0.6f; }
      row[x * 3 + 0] = (u8)(b * 255);
      row[x * 3 + 1] = (u8)(g * 255);
      row[x * 3 + 2] = (u8)(r * 255);
    }
  }
  return write_file(path, buf, sizeof buf);
}

/* 12 s of a chord with a slow swell, 44.1 kHz stereo, so the visualizer moves. */
static bool make_wav(const char *path) {
  enum { RATE = 44100, SECS = 12, N = RATE * SECS };
  size_t bytes = (size_t)N * 4;
  u8 *b = (u8 *)fm_alloc(44 + bytes);
  memcpy(b, "RIFF", 4); put32le(b + 4, (u32)(36 + bytes)); memcpy(b + 8, "WAVEfmt ", 8);
  put32le(b + 16, 16); put16le(b + 20, 1); put16le(b + 22, 2);
  put32le(b + 24, RATE); put32le(b + 28, RATE * 4); put16le(b + 32, 4); put16le(b + 34, 16);
  memcpy(b + 36, "data", 4); put32le(b + 40, (u32)bytes);
  static const float kFreq[4] = { 110.0f, 220.0f, 330.0f, 1320.0f };
  for (int i = 0; i < N; i++) {
    float t = (float)i / RATE, v = 0;
    for (int k = 0; k < 4; k++)
      v += sinf(2 * 3.14159265f * kFreq[k] * t) * (0.5f + 0.5f * sinf(t * (0.7f + k * 0.9f))) * 0.2f;
    int s = (int)(v * 32767.0f);
    s = FM_CLAMP(s, -32767, 32767);
    put16le(b + 44 + (size_t)i * 4, (u32)(u16)(i16)s);
    put16le(b + 46 + (size_t)i * 4, (u32)(u16)(i16)s);
  }
  bool ok = write_file(path, b, 44 + bytes);
  fm_free(b);
  return ok;
}

/* ---- a tiny MPEG-1 writer: intra-only, DC coefficients only ---------------------------- */

typedef struct Bits { u8 *p; size_t n, cap; u32 acc; int nacc; } Bits;

static void bits_put(Bits *b, u32 v, int n) {
  for (int i = n - 1; i >= 0; i--) {
    b->acc = (b->acc << 1) | ((v >> i) & 1u);
    if (++b->nacc == 8) {
      if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 65536; b->p = (u8 *)fm_realloc(b->p, b->cap); }
      b->p[b->n++] = (u8)b->acc;
      b->acc = 0;
      b->nacc = 0;
    }
  }
}

static void bits_align(Bits *b) { while (b->nacc) bits_put(b, 0, 1); }
static void bits_code(Bits *b, u32 code) { bits_align(b); bits_put(b, 0x000001u, 24); bits_put(b, code, 8); }

/* dct_dc_size VLCs (ISO 11172-2 tables B.5a/B.5b): {code, length} by size */
static const u8 kDcLum[9][2] = { {4,3}, {0,2}, {1,2}, {5,3}, {6,3}, {14,4}, {30,5}, {62,6}, {126,7} };
static const u8 kDcChr[9][2] = { {0,2}, {1,2}, {2,2}, {6,3}, {14,4}, {30,5}, {62,6}, {126,7}, {254,8} };

static void put_dc(Bits *b, int diff, bool chroma) {
  int a = diff < 0 ? -diff : diff, size = 0;
  while (a >> size) size++;
  const u8 *c = chroma ? kDcChr[size] : kDcLum[size];
  bits_put(b, c[0], c[1]);
  if (size) bits_put(b, (u32)(diff > 0 ? diff : diff + (1 << size) - 1), size);
  bits_put(b, 2, 2);                       /* end of block */
}

/* One picture's pixels as 8x8 block means: a hue sweep moving with t. */
static int clip255(float v) { return v < 0 ? 0 : v > 255 ? 255 : (int)v; }

static void mpeg_picture(Bits *b, int mbw, int mbh, int frame, int tref) {
  bits_code(b, 0x00);                      /* picture */
  bits_put(b, (u32)tref, 10);
  bits_put(b, 1, 3);                       /* I */
  bits_put(b, 0xFFFF, 16);                 /* vbv_delay */
  bits_put(b, 0, 1);                       /* extra_bit_picture */
  float t = (float)frame / 30.0f;
  for (int my = 0; my < mbh; my++) {
    bits_code(b, (u32)(my + 1));            /* slice */
    bits_put(b, 8, 5);                     /* quantizer_scale */
    bits_put(b, 0, 1);                     /* extra_bit_slice */
    int py = 128, pcb = 128, pcr = 128;    /* DC predictors reset per slice */
    for (int mx = 0; mx < mbw; mx++) {
      bits_put(b, 1, 1);                   /* address increment 1 */
      bits_put(b, 1, 1);                   /* type: intra */
      for (int k = 0; k < 4; k++) {
        float x = (float)(mx * 2 + (k & 1)), y = (float)(my * 2 + (k >> 1));
        int v = clip255(128 + 70 * sinf(x * 0.12f + t * 2.0f) * cosf(y * 0.15f - t * 1.3f) +
                        30 * sinf((x + y) * 0.05f + t));
        put_dc(b, v - py, false);
        py = v;
      }
      int cb = clip255(128 + 60 * sinf(mx * 0.1f + t)), cr = clip255(128 + 60 * cosf(my * 0.2f - t * 0.7f));
      put_dc(b, cb - pcb, true);
      pcb = cb;
      put_dc(b, cr - pcr, true);
      pcr = cr;
    }
  }
}

static void ps_time(u8 *p, u32 tag, u64 t) {
  p[0] = (u8)((tag << 4) | ((t >> 29) & 0x0E) | 1);
  p[1] = (u8)(t >> 22);
  p[2] = (u8)(((t >> 14) & 0xFE) | 1);
  p[3] = (u8)(t >> 7);
  p[4] = (u8)(((t << 1) & 0xFE) | 1);
}

/* MPEG-1 program stream: pack + system header, then one or more PES packets
** per picture (PTS on the first). 640x352, 30 fps, 10 s. */
static bool make_mpg(const char *path) {
  enum { W = 640, H = 352, FRAMES = 300, CHUNK = 4000 };
  int mbw = W / 16, mbh = H / 16;
  FILE *f = fm_fopen(path, "wb");
  if (!f) return false;
  u8 hdr[32];
  Bits b;
  memset(&b, 0, sizeof b);
  /* pack header (SCR 0, mux rate 2.5 MB/s) */
  bits_code(&b, 0xBA);
  bits_put(&b, 2, 4);
  bits_put(&b, 0, 3); bits_put(&b, 1, 1);
  bits_put(&b, 0, 15); bits_put(&b, 1, 1);
  bits_put(&b, 0, 15); bits_put(&b, 1, 1);
  bits_put(&b, 1, 1); bits_put(&b, 50000, 22); bits_put(&b, 1, 1);
  /* system header: no audio, one video stream */
  bits_code(&b, 0xBB);
  bits_put(&b, 9, 16);                      /* header_length */
  bits_put(&b, 1, 1); bits_put(&b, 50000, 22); bits_put(&b, 1, 1);
  bits_put(&b, 0, 6);                       /* audio_bound */
  bits_put(&b, 0, 4);                       /* fixed, CSPS, audio lock, video lock */
  bits_put(&b, 1, 1);                       /* marker */
  bits_put(&b, 1, 5);                       /* video_bound */
  bits_put(&b, 0xFF, 8);                    /* reserved */
  bits_put(&b, 0xE0, 8); bits_put(&b, 3, 2); bits_put(&b, 1, 1); bits_put(&b, 46, 13);
  bool ok = fwrite(b.p, 1, b.n, f) == b.n;
  for (int i = 0; i < FRAMES && ok; i++) {
    b.n = 0;
    if (i % 30 == 0) {
      bits_code(&b, 0xB3);                 /* sequence header */
      bits_put(&b, W, 12);
      bits_put(&b, H, 12);
      bits_put(&b, 1, 4);                  /* square pixels */
      bits_put(&b, 5, 4);                  /* 30 fps */
      bits_put(&b, 0x3FFFF, 18);
      bits_put(&b, 1, 1);
      bits_put(&b, 20, 10);
      bits_put(&b, 0, 3);                  /* constrained, no matrices */
      bits_code(&b, 0xB8);                 /* group of pictures */
      int sec = i / 30;
      bits_put(&b, 0, 1);
      bits_put(&b, 0, 5);
      bits_put(&b, (u32)(sec / 60), 6);
      bits_put(&b, 1, 1);
      bits_put(&b, (u32)(sec % 60), 6);
      bits_put(&b, 0, 6);
      bits_put(&b, 1, 1);                  /* closed */
      bits_put(&b, 0, 1);
    }
    mpeg_picture(&b, mbw, mbh, i, i % 30);
    if (i == FRAMES - 1) bits_code(&b, 0xB7);
    bits_align(&b);
    for (size_t off = 0; off < b.n && ok; off += CHUNK) {
      size_t n = FM_MIN((size_t)CHUNK, b.n - off);
      bool pts = off == 0;
      size_t len = n + (pts ? 5 : 1);
      hdr[0] = 0; hdr[1] = 0; hdr[2] = 1; hdr[3] = 0xE0;
      hdr[4] = (u8)(len >> 8); hdr[5] = (u8)len;
      if (pts) ps_time(hdr + 6, 2, (u64)(9000 + i * 3000));
      else hdr[6] = 0x0F;
      ok = fwrite(hdr, 1, pts ? 11 : 7, f) == (size_t)(pts ? 11 : 7) && fwrite(b.p + off, 1, n, f) == n;
    }
  }
  fm_free(b.p);
  return (fclose(f) == 0) && ok;
}

/* ---- the test folder ----------------------------------------------------------------- */

static const char *const kWords[] = { "Invoice", "Report", "Notes", "Backup", "Budget", "Meeting",
                                      "Holiday", "Project", "Draft", "Summary", "Contract", "Plan" };
static const char *const kExts[] = { "txt", "pdf", "docx", "xlsx", "pptx", "zip", "7z", "c", "h",
                                     "json", "md", "exe", "apk", "ttf", "iso", "html", "csv", "tar.gz" };

static bool make_folder(const char *dir) {
  char p[FM_PATH_MAX], n[160];
  if (plat_mkdirs(dir) != FM_OK && !plat_is_dir(dir)) return false;
  int made = 0;
  for (int i = 0; i < PERF_FOLDERS; i++, made++) {
    fm_snprintf(n, sizeof n, "Folder %02d %s", i + 1, kWords[i % FM_COUNT(kWords)]);
    fm_path_join(p, sizeof p, dir, n);
    plat_mkdirs(p);
  }
  for (int i = 0; i < PERF_PHOTOS; i++, made++) {
    fm_snprintf(n, sizeof n, "2026-10-08 Photo %04d.bmp", i + 1);
    fm_path_join(p, sizeof p, dir, n);
    if (!make_photo(p, i)) return false;
  }
  fm_path_join(P_.wav, sizeof P_.wav, dir, "Tone chord.wav");
  if (!make_wav(P_.wav)) return false;
  fm_path_join(P_.mpg, sizeof P_.mpg, dir, "Clip intra.mpg");
  if (!make_mpg(P_.mpg)) return false;
  made += 2;
  char body[4096];
  for (int i = 0; i < (int)sizeof body; i++) body[i] = (char)('a' + i % 26);
  for (int i = 0; made < PERF_FILES; i++, made++) {
    const char *w = kWords[(i * 7) % FM_COUNT(kWords)];
    const char *e = kExts[(i * 5) % FM_COUNT(kExts)];
    if (i % 9 == 4)
      fm_snprintf(n, sizeof n, "%s %04d with a much longer descriptive name than usual.%s", w, i, e);
    else
      fm_snprintf(n, sizeof n, "%s_%04d.%s", w, i, e);
    fm_path_join(p, sizeof p, dir, n);
    if (!write_file(p, body, (size_t)((i * 7919) % (int)sizeof body))) return false;
  }
  return true;
}

bool perf_prepare(int *argc, char ***argv) {
  P_.on = true;
  P_.frames = 90;
  char **av = *argv;
  int ac = *argc;
  for (int i = 1; i < ac; i++) {
    if (!strcmp(av[i], "--frames") && i + 1 < ac) {
      int n = atoi(av[++i]);
      P_.frames = FM_CLAMP(n, 5, PERF_MAX_FRAMES);
    } else if (!strcmp(av[i], "--out") && i + 1 < ac) P_.out = av[++i];
    else if (!strcmp(av[i], "--perf-shots") && i + 1 < ac) P_.shots = av[++i];
    else if (!strcmp(av[i], "--perf-only") && i + 1 < ac) P_.only = av[++i];
    else if (!strcmp(av[i], "--perf-probe")) P_.probe = true;
  }
  char cache[FM_PATH_MAX], mark[FM_PATH_MAX];
  if (!plat_place(PLACE_CACHE, cache, sizeof cache) && !plat_place(PLACE_TEMP, cache, sizeof cache)) return false;
  fm_path_join(P_.dir, sizeof P_.dir, cache, PERF_DIR);
  fm_path_join(P_.wav, sizeof P_.wav, P_.dir, "Tone chord.wav");
  fm_path_join(P_.mpg, sizeof P_.mpg, P_.dir, "Clip intra.mpg");
  fm_path_join(mark, sizeof mark, cache, PERF_DIR ".v1.ok");
  double t0 = now_ms();
  if (!plat_exists(mark) || !plat_exists(P_.mpg)) {
    fprintf(stderr, "perf: generating %d files in %s\n", PERF_FILES, P_.dir);
    if (!make_folder(P_.dir)) {
      fprintf(stderr, "perf: could not write the test folder %s\n", P_.dir);
      return false;
    }
    write_file(mark, "ok\n", 3);
  }
  P_.gen_ms = now_ms() - t0;
  /* the app's arguments: screenshot defaults (never saved), both panels on
  ** the folder, plus what the user passed (theme, light/dark, touch) */
  P_.argv = (char **)fm_calloc((size_t)ac + 8, sizeof(char *));
  int n = 0;
  P_.argv[n++] = av[0];
  P_.argv[n++] = (char *)"--shot";
  P_.argv[n++] = (char *)"perf";
  P_.argv[n++] = (char *)"--left";
  P_.argv[n++] = P_.dir;
  P_.argv[n++] = (char *)"--right";
  P_.argv[n++] = P_.dir;
  for (int i = 1; i < ac; i++) {
    const char *a = av[i];
    if (!strcmp(a, "--perf") || !strcmp(a, "--perf-probe")) continue;
    if (!strcmp(a, "--frames") || !strcmp(a, "--out") || !strcmp(a, "--perf-shots") ||
        !strcmp(a, "--perf-only") || !strcmp(a, "--size")) { i++; continue; }
    P_.argv[n++] = av[i];
  }
  P_.argc = n;
  *argc = n;
  *argv = P_.argv;
  return true;
}

/* ---- input -------------------------------------------------------------------------- */

static float pt_scale(void) {
  int ww = 1, wh = 1;
  SDL_GetWindowSize(app.win, &ww, &wh);
  return ui.w > 0 ? (float)ww / ui.w : 1.0f;
}

static void push_key(SDL_Keycode k, Uint16 mod) {
  SDL_Event e;
  memset(&e, 0, sizeof e);
  e.type = SDL_KEYDOWN;
  e.key.windowID = SDL_GetWindowID(app.win);
  e.key.state = SDL_PRESSED;
  e.key.keysym.sym = k;
  e.key.keysym.mod = mod;
  SDL_PushEvent(&e);
}

static void push_motion(float x, float y) {
  float s = pt_scale();
  SDL_Event e;
  memset(&e, 0, sizeof e);
  e.type = SDL_MOUSEMOTION;
  e.motion.windowID = SDL_GetWindowID(app.win);
  e.motion.x = (int)(x * s);
  e.motion.y = (int)(y * s);
  SDL_PushEvent(&e);
}

static void push_wheel(float dy) {
  SDL_Event e;
  memset(&e, 0, sizeof e);
  e.type = SDL_MOUSEWHEEL;
  e.wheel.windowID = SDL_GetWindowID(app.win);
  e.wheel.y = (Sint32)dy;
  e.wheel.preciseY = dy;
  e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
  SDL_PushEvent(&e);
}

static void push_click(int button, float x, float y) {
  float s = pt_scale();
  SDL_Event e;
  memset(&e, 0, sizeof e);
  e.type = SDL_MOUSEBUTTONDOWN;
  e.button.windowID = SDL_GetWindowID(app.win);
  e.button.button = (Uint8)button;
  e.button.state = SDL_PRESSED;
  e.button.clicks = 1;
  e.button.x = (int)(x * s);
  e.button.y = (int)(y * s);
  SDL_PushEvent(&e);
  e.type = SDL_MOUSEBUTTONUP;
  e.button.state = SDL_RELEASED;
  SDL_PushEvent(&e);
}

/* The left panel's rectangle, as the app lays it out with its defaults. */
static FmRect left_panel(void) {
  FmLayout L;
  layout_compute(&L, 0, true, 0, audio_mini_active());
  return L.show[0] ? L.panel[0] : L.panel[1];
}

/* ---- the screens ------------------------------------------------------------------------ */

static void run_frames(int n, bool force) {
  for (int i = 0; i < n; i++) {
    if (force) ui_redraw();
    P_.frame();
  }
}

static void panels(int view, bool thumbs) {
  conf.view[0] = conf.view[1] = view;
  conf.thumbnails = thumbs;
}

static void en_list(void) { panels(VIEW_LIST, false); }
static void en_list_th(void) { panels(VIEW_LIST, true); }
static void en_grid(void) { panels(VIEW_GRID, false); }
static void en_grid_th(void) { panels(VIEW_GRID, true); }

static void st_scroll(int i) {
  FmRect r = left_panel();
  push_motion(r.x + r.w * 0.5f, r.y + r.h * 0.6f);
  push_wheel(i % 120 < 60 ? -2.0f : 2.0f);   /* down, then back up */
}

static void en_settings(void) {
  panels(VIEW_LIST, true);
  push_key(SDLK_COMMA, KMOD_LCTRL);
}

static void lv_escape(void) {
  push_key(SDLK_ESCAPE, 0);
  run_frames(3, true);
}

static void st_settings_scroll(int i) {
  push_motion(ui.w * 0.5f, ui.h * 0.5f);
  push_wheel(i % 3 == 0 ? -1.0f : 0.0f);     /* slowly down to the end */
}

static void en_context(void) {
  FmRect r = left_panel();
  float x = r.x + r.w * 0.4f, y = r.y + ui.m.bar_h + DP(80);
  push_motion(x, y);
  push_click(SDL_BUTTON_RIGHT, x, y);
}

static void en_music(void) { app_open(P_.wav, NULL, 0, 0); }
static void lv_viewer(void) { app_close_viewer(); run_frames(2, true); }
static void lv_mini(void) { audio_stop(); run_frames(2, true); }
static void en_video(void) { app_open(P_.mpg, NULL, 0, 0); }
static void en_library(void) { lib_demo(P_.dir, 0); }
static void lv_library(void) { lib_ui_close(); run_frames(2, true); }
/* the online videos view without a search: no network needed */
static void en_online(void) { online_open("archive"); }
static void lv_online(void) { online_close(); run_frames(2, true); }

typedef struct Stage {
  const char *name;
  void (*enter)(void);
  void (*step)(int i);
  void (*leave)(void);
  int warm_ms;
} Stage;

static const Stage kStages[] = {
  { "list",            en_list,     NULL,               NULL,       300 },
  { "list+thumbs",     en_list_th,  NULL,               NULL,       1500 },
  { "grid",            en_grid,     NULL,               NULL,       300 },
  { "grid+thumbs",     en_grid_th,  NULL,               NULL,       1500 },
  { "scroll-grid",     en_grid_th,  st_scroll,          NULL,       200 },
  { "scroll-list",     en_list_th,  st_scroll,          NULL,       200 },
  { "settings",        en_settings, NULL,               lv_escape,  600 },
  { "settings-scroll", en_settings, st_settings_scroll, lv_escape,  600 },
  { "context-menu",    en_context,  NULL,               lv_escape,  400 },
  { "music+viz",       en_music,    NULL,               lv_viewer,  1000 },
  { "mini-player",     NULL,        NULL,               lv_mini,    400 },
  { "video",           en_video,    NULL,               lv_viewer,  1000 },
  { "library",         en_library,  NULL,               lv_library, 1500 },
  { "online",          en_online,   NULL,               lv_online,  600 },
};

typedef struct Row {
  const char *name;
  int frames;
  double bmin, bavg, b95, pres, cpu;
  double calls, verts, idx, ftex, fclip;
  int tex_n;
  double tex_mb, heap_mb, heap_after_mb, priv_mb, ws_mb;
  int atlas, atlas_used;
} Row;

static int cmp_d(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

static bool stage_selected(const char *name) {
  if (!P_.only) return true;
  const char *s = P_.only;
  size_t n = strlen(name);
  while (*s) {
    const char *e = strchr(s, ',');
    size_t len = e ? (size_t)(e - s) : strlen(s);
    if (len == n && !strncmp(s, name, n)) return true;
    if (!e) break;
    s = e + 1;
  }
  return false;
}

/* ---- screenshots ------------------------------------------------------------------------ */

static void write_bmp32(const char *path, const u8 *px, int w, int h) {
  FILE *f = fm_fopen(path, "wb");
  if (!f) return;
  u8 hdr[54];
  memset(hdr, 0, sizeof hdr);
  hdr[0] = 'B'; hdr[1] = 'M';
  put32le(hdr + 2, 54 + (u32)(w * h * 4));
  put32le(hdr + 10, 54);
  put32le(hdr + 14, 40);
  put32le(hdr + 18, (u32)w);
  put32le(hdr + 22, (u32)-h);              /* top-down */
  put16le(hdr + 26, 1);
  put16le(hdr + 28, 32);
  fwrite(hdr, 1, sizeof hdr, f);
  fwrite(px, 4, (size_t)w * h, f);
  fclose(f);
}

/* Draws the current screen once more, reads it back and saves it; a half
** size copy goes into the contact sheet (3 columns). */
static void shoot(int idx, const char *name) {
  int w, h;
  SDL_GetRendererOutputSize(app.ren, &w, &h);
  gfx_begin(w, h, T.bg);
  ui_begin();
  app_frame();
  ui_end();
  gfx_flush();
  u8 *px = (u8 *)fm_alloc((size_t)w * h * 4);
  bool ok = SDL_RenderReadPixels(app.ren, NULL, SDL_PIXELFORMAT_ARGB8888, px, w * 4) == 0;
  SDL_RenderPresent(app.ren);
  if (ok) {
    char p[FM_PATH_MAX], n[64];
    fm_snprintf(n, sizeof n, "%02d-%s.bmp", idx + 1, name);
    for (char *c = n; *c; c++) if (*c == '+') *c = '_';
    fm_path_join(p, sizeof p, P_.shots, n);
    write_bmp32(p, px, w, h);
    int cw = w / 2, ch = h / 2, cols = 3;
    if (!P_.sheet) {
      int rows = (FM_COUNT(kStages) + cols - 1) / cols;
      P_.sheet_cell_w = cw + 8;
      P_.sheet_cell_h = ch + 8;
      P_.sheet_w = P_.sheet_cell_w * cols;
      P_.sheet_h = P_.sheet_cell_h * rows;
      P_.sheet = (u8 *)fm_calloc((size_t)P_.sheet_w * P_.sheet_h, 4);
    }
    int cx = (P_.sheet_n % cols) * P_.sheet_cell_w + 4, cy = (P_.sheet_n / cols) * P_.sheet_cell_h + 4;
    if (cy + ch <= P_.sheet_h && cw <= P_.sheet_cell_w) {
      for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++) {
          const u8 *a = px + ((size_t)(y * 2) * w + x * 2) * 4;
          u8 *d = P_.sheet + ((size_t)(cy + y) * P_.sheet_w + cx + x) * 4;
          for (int k = 0; k < 4; k++)
            d[k] = (u8)((a[k] + a[k + 4] + a[(size_t)w * 4 + k] + a[(size_t)w * 4 + k + 4] + 2) >> 2);
        }
    }
    P_.sheet_n++;
  }
  fm_free(px);
}

/* ---- the tour ------------------------------------------------------------------------- */

/* Events queued during the idle measurement, by kind, to explain wakeups. */
static int g_ev[8];
static const char *const kEvName[8] = { "input", "window", "user", "render", "app", "sentinel", "quit", "other" };

static int SDLCALL count_event(void *u, SDL_Event *e) {
  FM_UNUSED(u);
  int k = 7;
  if (e->type >= SDL_KEYDOWN && e->type < SDL_CLIPBOARDUPDATE) k = 0;
  else if (e->type == SDL_WINDOWEVENT || e->type == SDL_SYSWMEVENT) k = 1;
  else if (e->type >= SDL_USEREVENT) k = 2;
  else if (e->type == SDL_RENDER_TARGETS_RESET || e->type == SDL_RENDER_DEVICE_RESET) k = 3;
  else if (e->type >= SDL_APP_TERMINATING && e->type <= SDL_LOCALECHANGED) k = 4;
  else if (e->type == SDL_POLLSENTINEL) k = 5;
  else if (e->type == SDL_QUIT) k = 6;
  g_ev[k]++;
  return 1;
}

/* Our heap without the tour's own buffers (the contact sheet). */
static double heap_mb(void) {
  size_t n = fm_mem_in_use();
  if (P_.sheet) n -= (size_t)P_.sheet_w * P_.sheet_h * 4;
  return (double)n / (1024.0 * 1024.0);
}

static void measure(const Stage *s, Row *r, double *bt) {
  memset(r, 0, sizeof *r);
  r->name = s->name;
  if (s->enter) s->enter();
  /* warm up: dialogs open, thumbnails decode, playback starts */
  double t0 = now_ms();
  int warm = 0;
  while (warm < 5 || now_ms() - t0 < s->warm_ms) {
    if (s->step) s->step(warm);
    run_frames(1, true);
    warm++;
  }
  GfxStats g0, g;
  gfx_get_stats(&g0);
  double cpu0 = proc_cpu_ms();
  int n = 0;
  double sum_calls = 0, sum_verts = 0, sum_idx = 0, sum_ftex = 0, sum_fclip = 0, sum_pres = 0;
  for (int i = 0; i < P_.frames; i++) {
    if (s->step) s->step(warm + i);
    ui_redraw();
    u32 f0 = g0.frames;
    double a = now_ms();
    P_.frame();
    double b = now_ms();
    gfx_get_stats(&g);
    if (g.frames == f0) continue;          /* nothing drawn (should not happen) */
    g0.frames = g.frames;
    double build = (b - a) - g.present_ms;
    if (build < 0) build = 0;
    bt[n++] = build;
    sum_calls += g.calls;
    sum_verts += g.verts;
    sum_idx += g.indices;
    sum_ftex += g.flush_tex;
    sum_fclip += g.flush_clip;
    sum_pres += g.present_ms;
  }
  double cpu1 = proc_cpu_ms();
  r->frames = n;
  if (n > 0) {
    double sum = 0;
    for (int i = 0; i < n; i++) sum += bt[i];
    qsort(bt, (size_t)n, sizeof(double), cmp_d);
    r->bmin = bt[0];
    r->bavg = sum / n;
    r->b95 = bt[FM_MIN(n - 1, (int)(n * 0.95))];
    r->pres = sum_pres / n;
    r->cpu = (cpu1 - cpu0) / n;
    r->calls = sum_calls / n;
    r->verts = sum_verts / n;
    r->idx = sum_idx / n;
    r->ftex = sum_ftex / n;
    r->fclip = sum_fclip / n;
  }
  gfx_get_stats(&g);
  r->tex_n = g.tex_live;
  r->tex_mb = (double)g.tex_bytes / (1024.0 * 1024.0);
  FontStats fs;
  font_get_stats(&fs);
  r->atlas = fs.atlas_w;
  r->atlas_used = fs.atlas_h ? fs.used_h * 100 / fs.atlas_h : 0;
  r->heap_mb = heap_mb();
  proc_mem(&r->priv_mb, &r->ws_mb);
}

static void print_row(const Row *r) {
  out("%-16s %4d %6.2f %6.2f %6.2f %6.2f %6.2f %5.0f %6.0f %6.0f %4.0f %4.0f %4d %6.1f %5d %3d%% %6.1f %6.1f %6.1f %6.1f\n",
      r->name, r->frames, r->bmin, r->bavg, r->b95, r->pres, r->cpu, r->calls, r->verts, r->idx,
      r->ftex, r->fclip, r->tex_n, r->tex_mb, r->atlas, r->atlas_used, r->heap_mb, r->heap_after_mb,
      r->priv_mb, r->ws_mb);
}

/* --perf-probe: what the GPU driver charges this process for textures and
** for geometry, to tell our memory from the driver's. */
static void probe(void) {
  double p0, w0, p1, w1;
  run_frames(5, true);
  proc_mem(&p0, &w0);
  SDL_Texture *t[400];
  static u32 px[64 * 64];
  for (int i = 0; i < 400; i++) {
    t[i] = SDL_CreateTexture(app.ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 48, 48);
    SDL_UpdateTexture(t[i], NULL, px, 48 * 4);
  }
  run_frames(3, true);
  proc_mem(&p1, &w1);
  out("probe: 400 x 48x48 textures: +%.1f MB private (%.1f KB each, 9 KB of pixels)\n", p1 - p0, (p1 - p0) * 1024 / 400);
  for (int i = 0; i < 400; i++) SDL_DestroyTexture(t[i]);
  run_frames(3, true);
  proc_mem(&p0, &w0);
  for (int i = 0; i < 4; i++) {
    t[i] = SDL_CreateTexture(app.ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 1024, 1024);
    u32 *big = (u32 *)fm_calloc(1024 * 1024, 4);
    SDL_UpdateTexture(t[i], NULL, big, 1024 * 4);
    fm_free(big);
  }
  run_frames(3, true);
  proc_mem(&p1, &w1);
  out("probe: 4 x 1024x1024 textures: +%.1f MB private (16 MB of pixels)\n", p1 - p0);
  for (int i = 0; i < 4; i++) SDL_DestroyTexture(t[i]);
  /* geometry: a frame with 200k indices */
  run_frames(3, true);
  proc_mem(&p0, &w0);
  for (int k = 0; k < 10; k++) {
    ui_redraw();
    int w, h;
    SDL_GetRendererOutputSize(app.ren, &w, &h);
    gfx_begin(w, h, T.bg);
    for (int i = 0; i < 33000; i++) gfx_rect(FM_RECT(i % 500, i / 500, 1, 1), T.text);
    gfx_end();
  }
  proc_mem(&p1, &w1);
  out("probe: 10 frames of 200k indices: +%.1f MB private\n", p1 - p0);
}

int perf_run(void (*frame_fn)(void)) {
  P_.frame = frame_fn;
  if (P_.probe) { probe(); return 0; }
  if (P_.out) P_.report = fm_fopen(P_.out, "w");
  if (P_.shots) plat_mkdirs(P_.shots);
  double *bt = (double *)fm_alloc(sizeof(double) * PERF_MAX_FRAMES);

  /* first frame: closes the startup timeline */
  run_frames(1, true);
  perf_mark("first frame");

  SDL_RendererInfo ri;
  memset(&ri, 0, sizeof ri);
  SDL_GetRendererInfo(app.ren, &ri);
  SDL_version v;
  SDL_GetVersion(&v);
  out("mmcfm --perf  (%s, %d-bit, SDL %d.%d.%d, renderer %s%s, %dx%d px, scale %.2f, %d frames/screen)\n",
      FM_VERSION, (int)(sizeof(void *) * 8), v.major, v.minor, v.patch, ri.name ? ri.name : "?",
      (ri.flags & SDL_RENDERER_PRESENTVSYNC) ? " vsync" : "", (int)ui.w, (int)ui.h, ui.scale, P_.frames);
  out("test folder: %s (%d entries; %s %.0f ms)\n\n", P_.dir, PERF_FILES,
      P_.gen_ms > 50 ? "generated in" : "reused,", P_.gen_ms);

  out("startup:");
  for (int i = 1; i < P_.nmarks; i++)
    out(" %s +%.1f", P_.mark_name[i],
        (double)(P_.mark_t[i] - P_.mark_t[i - 1]) * 1000.0 / (double)SDL_GetPerformanceFrequency());
  if (P_.nmarks > 1)
    out("  = %.1f ms from main to the first frame\n\n",
        (double)(P_.mark_t[P_.nmarks - 1] - P_.mark_t[0]) * 1000.0 / (double)SDL_GetPerformanceFrequency());

  out("build = frame CPU work in ms (min avg p95), pres = ms inside RenderPresent (vsync wait),\n"
      "cpu = process CPU ms per frame (all threads), draws/verts/idx per frame, split = batch\n"
      "breaks by texture / clip, tex = live textures and MB, atlas = glyph atlas edge and fill,\n"
      "heap = our allocations (while shown / after closing), priv / ws = process MB\n\n");
  out("%-16s %4s %6s %6s %6s %6s %6s %5s %6s %6s %4s %4s %4s %6s %5s %4s %6s %6s %6s %6s\n", "screen", "n",
      "min", "avg", "p95", "pres", "cpu", "draws", "verts", "idx", "tex", "clip", "tex", "texMB",
      "atlas", "fill", "heap", "after", "priv", "ws");

  Row rows[FM_COUNT(kStages)];
  int nrows = 0;
  for (int s = 0; s < FM_COUNT(kStages); s++) {
    const Stage *st = &kStages[s];
    if (!stage_selected(st->name)) continue;
    Row *r = &rows[nrows++];
    measure(st, r, bt);
    if (P_.shots) shoot(s, st->name);
    if (st->leave) st->leave();
    run_frames(3, true);
    r->heap_after_mb = heap_mb();
    print_row(r);
  }

  /* idle: back on the panels, no forced redraws */
  panels(VIEW_LIST, true);
  push_motion(-100, -100);
  double t0 = now_ms();
  while (now_ms() - t0 < 1500) P_.frame();      /* settle: thumbnails, fades */
  GfxStats g0, g1;
  gfx_get_stats(&g0);
  double c0 = proc_cpu_ms();
  int wakeups = 0, anim = 0;
  memset(g_ev, 0, sizeof g_ev);
  SDL_AddEventWatch(count_event, NULL);
  t0 = now_ms();
  while (now_ms() - t0 < 5000) {
    P_.frame();
    wakeups++;
    if (ui.anim) anim++;
  }
  SDL_DelEventWatch(count_event, NULL);
  double secs = (now_ms() - t0) / 1000.0;
  gfx_get_stats(&g1);
  double c1 = proc_cpu_ms();
  double priv, ws;
  proc_mem(&priv, &ws);
  out("\nidle %.1f s: %u frames drawn, %d loop wakeups, %.0f ms CPU (%.2f%% of one core), "
      "heap %.1f MB, private %.1f MB, working set %.1f MB, %d textures (%.1f MB)\n",
      secs, g1.frames - g0.frames, wakeups, c1 - c0, (c1 - c0) / (secs * 10.0),
      heap_mb(), priv, ws, g1.tex_live,
      (double)g1.tex_bytes / (1024.0 * 1024.0));
  out("idle events:");
  for (int k = 0; k < 8; k++)
    if (g_ev[k]) out(" %s %d", kEvName[k], g_ev[k]);
  out("%s; frames that asked to animate: %d\n", wakeups ? "" : " none", anim);

  if (P_.shots && P_.sheet) {
    char p[FM_PATH_MAX];
    fm_path_join(p, sizeof p, P_.shots, "contact.bmp");
    write_bmp32(p, P_.sheet, P_.sheet_w, P_.sheet_h);
    out("screenshots: %s (contact.bmp + one per screen)\n", P_.shots);
    fm_free(P_.sheet);
    P_.sheet = NULL;
  }
  if (P_.report) fclose(P_.report);
  P_.report = NULL;
  fm_free(bt);
  return 0;
}
