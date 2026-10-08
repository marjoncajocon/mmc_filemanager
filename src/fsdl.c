/* fsdl.c -- loads SDL2 at run time for FM_SDL_DYNAMIC builds; fm_log.
**
** Search order, so a build never picks up a stray copy:
**   Windows  <exe dir>\SDL2.dll only
**   Linux    <exe dir>/lib/libSDL2-2.0.so.0, <exe dir>/libSDL2-2.0.so.0, system
**   macOS    <exe dir>/../Frameworks/libSDL2-2.0.0.dylib, <exe dir>, Homebrew, system
**   BSD      <exe dir>/lib, system
**
** Design decisions:
**   - The function table is built from fsdl_list.h, so adding an SDL call is
**     one line in one file.
**   - Missing optional symbols (newer than the user's SDL) are not fatal;
**     only SDL_RenderGeometry (2.0.18) and the basics are required.
*/
#include "fcore.h"
#include "fsdl.h"

static int g_sdl_ready;

/* ---- logging ------------------------------------------------------------ */

void fm_log(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (g_sdl_ready) SDL_Log("%s", buf);
  else fprintf(stderr, "mmcfm: %s\n", buf);
}

/* ---- threads ------------------------------------------------------------ */

#if defined(FM_SDL_DYNAMIC) && defined(FM_WIN)
#  include <process.h>
#endif

SDL_Thread *fm_thread_create(SDL_ThreadFunction fn, const char *name, void *data) {
#if defined(FM_SDL_DYNAMIC) && defined(FM_WIN)
  return fmsdl_SDL_CreateThread(fn, name, data, (pfnSDL_CurrentBeginThread)_beginthreadex,
                                (pfnSDL_CurrentEndThread)_endthreadex);
#elif defined(FM_SDL_DYNAMIC)
  return fmsdl_SDL_CreateThread(fn, name, data);
#else
  return SDL_CreateThread(fn, name, data);
#endif
}

#ifndef FM_SDL_DYNAMIC

/* ---- static builds ------------------------------------------------------ */

int fsdl_load(char *err, int cap) {
  FM_UNUSED(err); FM_UNUSED(cap);
  g_sdl_ready = 1;
  return 1;
}

void fsdl_unload(void) { g_sdl_ready = 0; }

#else

/* ---- function table ----------------------------------------------------- */

#define FM_SDL_FN(ret, name, params) ret (SDLCALL *fmsdl_##name) params;
#include "fsdl_list.h"
#undef FM_SDL_FN

typedef struct { const char *name; void **slot; } Sym;

static const Sym kSyms[] = {
#define FM_SDL_FN(ret, name, params) { #name, (void **)&fmsdl_##name },
#include "fsdl_list.h"
#undef FM_SDL_FN
};

/* Symbols a usable SDL must have; the rest may be absent in old versions. */
static const char *const kRequired[] = {
  "SDL_Init", "SDL_CreateWindow", "SDL_CreateRenderer", "SDL_RenderGeometry",
  "SDL_PollEvent", "SDL_CreateTexture", "SDL_UpdateTexture", "SDL_RenderPresent",
};

#ifdef FM_WIN
#  include "fwin.h"
static HMODULE g_lib;

static void *lib_open(char *tried, int cap) {
  wchar_t path[MAX_PATH + 16];
  DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return NULL;
  while (n > 0 && path[n - 1] != L'\\' && path[n - 1] != L'/') n--;
  path[n] = 0;
  wcscat(path, L"SDL2.dll");
  WideCharToMultiByte(CP_UTF8, 0, path, -1, tried, cap, NULL, NULL);
  g_lib = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
  return g_lib;
}
static void *lib_sym(const char *name) { return (void *)GetProcAddress(g_lib, name); }
static void lib_close(void) { if (g_lib) FreeLibrary(g_lib); g_lib = NULL; }

#else
#  include <dlfcn.h>
#  include <unistd.h>
#  ifdef FM_MACOS
#    include <mach-o/dyld.h>
#  endif
#  if defined(FM_BSD)
#    include <sys/types.h>
#    include <sys/sysctl.h>
#  endif
static void *g_lib;

static void exe_dir(char *out, size_t cap) {
  out[0] = 0;
#if defined(FM_MACOS)
  uint32_t sz = (uint32_t)cap;
  if (_NSGetExecutablePath(out, &sz) != 0) out[0] = 0;
#elif defined(FM_BSD) && defined(KERN_PROC_PATHNAME)
  int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
  size_t sz = cap;
  if (sysctl(mib, 4, out, &sz, NULL, 0) != 0) out[0] = 0;
#else
  ssize_t n = readlink("/proc/self/exe", out, cap - 1);
  out[n > 0 ? n : 0] = 0;
#endif
  char *slash = strrchr(out, '/');
  if (slash) *slash = 0; else out[0] = 0;
}

static void *lib_open(char *tried, int cap) {
  char dir[FM_PATH_MAX], p[FM_PATH_MAX + 64];
  exe_dir(dir, sizeof dir);
  static const char *const local[] = {
#if defined(FM_MACOS)
    "../Frameworks/libSDL2-2.0.0.dylib", "../Frameworks/libSDL2.dylib", "libSDL2-2.0.0.dylib",
#else
    "lib/libSDL2-2.0.so.0", "libSDL2-2.0.so.0",
#endif
  };
  static const char *const sys[] = {
#if defined(FM_MACOS)
    "/opt/homebrew/lib/libSDL2-2.0.0.dylib", "/usr/local/lib/libSDL2-2.0.0.dylib",
    "libSDL2-2.0.0.dylib", "libSDL2.dylib",
#elif defined(FM_BSD)
    "libSDL2-2.0.so.0", "libSDL2.so", "/usr/local/lib/libSDL2-2.0.so.0", "/usr/local/lib/libSDL2.so",
#else
    "libSDL2-2.0.so.0", "libSDL2.so",
#endif
  };
  tried[0] = 0;
  if (dir[0]) {
    for (int i = 0; i < FM_COUNT(local); i++) {
      fm_snprintf(p, sizeof p, "%s/%s", dir, local[i]);
      if ((g_lib = dlopen(p, RTLD_NOW | RTLD_LOCAL)) != NULL) return g_lib;
    }
  }
  for (int i = 0; i < FM_COUNT(sys); i++) {
    if ((g_lib = dlopen(sys[i], RTLD_NOW | RTLD_LOCAL)) != NULL) return g_lib;
  }
  fm_strlcpy(tried, sys[0], (size_t)cap);
  return NULL;
}
static void *lib_sym(const char *name) { return dlsym(g_lib, name); }
static void lib_close(void) { if (g_lib) dlclose(g_lib); g_lib = NULL; }
#endif

int fsdl_load(char *err, int cap) {
  char tried[FM_PATH_MAX];
  if (!lib_open(tried, sizeof tried)) {
#if defined(FM_WIN)
    fm_snprintf(err, (size_t)cap, "SDL2.dll was not found next to the program:\n%s", tried);
#elif defined(FM_MACOS)
    fm_snprintf(err, (size_t)cap, "SDL2 is not installed.\nInstall it with: brew install sdl2");
#else
    fm_snprintf(err, (size_t)cap, "SDL2 is not installed.\nInstall the libsdl2 package "
                "(apt install libsdl2-2.0-0, dnf install SDL2, pkg install sdl2).");
#endif
    return 0;
  }
  for (int i = 0; i < FM_COUNT(kSyms); i++) *kSyms[i].slot = lib_sym(kSyms[i].name);
  for (int i = 0; i < FM_COUNT(kRequired); i++) {
    if (!lib_sym(kRequired[i])) {
      fm_snprintf(err, (size_t)cap, "The installed SDL2 is too old (missing %s).\n"
                  "Version 2.0.18 or newer is needed.", kRequired[i]);
      lib_close();
      return 0;
    }
  }
  g_sdl_ready = 1;
  return 1;
}

void fsdl_unload(void) {
  g_sdl_ready = 0;
  lib_close();
}

#endif
