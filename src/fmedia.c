/* fmedia.c -- the system's media controls (see fmedia.h).
**
** Design decisions:
**   - Polled, not pushed: twice a second (and right after a command) the
**     music player and the video player are asked what plays; Java hears
**     only about changes. The players stay unaware of the notification.
**   - A video wins over music: it is what the user looks at or just sent to
**     the background. Previous / Next belong to the music queue.
**   - Commands from Java (the notification's buttons, the MediaSession, an
**     audio focus loss) arrive on Java threads: they are kept in an atomic
**     and carried out here, on the main thread, as tray_pump does.
**   - The cover art goes over only when the track changes.
*/
#include "fmedia.h"
#include "fapp.h"
#include "fsdl.h"
#include "fview.h"

void media_bg_pump(void) {
  video_bg_pump();                     /* a video playing as sound */
  audio_queue_pump();                  /* the queue's position, should Android end the app */
}

#ifdef FM_ANDROID
#include <jni.h>

enum { MC_NONE, MC_TOGGLE, MC_PLAY, MC_PAUSE, MC_NEXT, MC_PREV, MC_STOP };

static SDL_atomic_t g_cmd;

JNIEXPORT void JNICALL Java_io_github_mmc_filemanager_FmMedia_nativeCommand(JNIEnv *e, jclass c, jint cmd) {
  (void)e; (void)c;
  SDL_AtomicSet(&g_cmd, (int)cmd);
  app_wake();
}

static struct {
  int state;                           /* 0 not loaded, 1 ready, -1 missing */
  jclass cls;
  jmethodID update, stop;
} J;

static JNIEnv *java(jobject *act) {
  JNIEnv *e = (JNIEnv *)SDL_AndroidGetJNIEnv();
  *act = e ? (jobject)SDL_AndroidGetActivity() : NULL;
  if (!*act) return NULL;
  if (!J.state) {
    /* the activity's class loader: FindClass would use the system one */
    J.state = -1;
    jclass ac = (*e)->GetObjectClass(e, *act);
    jmethodID gl = (*e)->GetMethodID(e, ac, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = gl ? (*e)->CallObjectMethod(e, *act, gl) : NULL;
    if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
    if (loader) {
      jclass lc = (*e)->GetObjectClass(e, loader);
      jmethodID load = (*e)->GetMethodID(e, lc, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
      jstring name = (*e)->NewStringUTF(e, "io.github.mmc.filemanager.FmMedia");
      jclass c = load ? (jclass)(*e)->CallObjectMethod(e, loader, load, name) : NULL;
      if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionClear(e); c = NULL; }
      if (c) {
        J.cls = (jclass)(*e)->NewGlobalRef(e, c);
        (*e)->DeleteLocalRef(e, c);
        J.update = (*e)->GetStaticMethodID(e, J.cls, "update",
                                           "(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;ZZZ[IIIZ)V");
        J.stop = (*e)->GetStaticMethodID(e, J.cls, "stop", "(Landroid/app/Activity;)V");
        if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
        if (J.update && J.stop) J.state = 1;
      }
      (*e)->DeleteLocalRef(e, name);
      (*e)->DeleteLocalRef(e, lc);
      (*e)->DeleteLocalRef(e, loader);
    }
    (*e)->DeleteLocalRef(e, ac);
    if (J.state < 0) fm_log("media: FmMedia.java is missing, no notification");
  }
  if (J.state > 0) return e;
  (*e)->DeleteLocalRef(e, *act);
  *act = NULL;
  return NULL;
}

static jstring jstr(JNIEnv *e, const char *s) {
  return (*e)->NewStringUTF(e, s ? s : "");
}

/* What plays now: a video first, else the music player. */
static int now(FmMediaInfo *m, bool want_cover) {
  if (video_media_info(m)) return 2;
  if (audio_media_info(m, want_cover)) return 1;
  return 0;
}

static void run(int cmd) {
  FmMediaInfo m;
  int who = now(&m, false);
  if (!who) return;
  bool video = who == 2;
  switch (cmd) {
    case MC_TOGGLE: video ? video_media_toggle() : audio_toggle_play(); break;
    case MC_PLAY: if (!m.playing) video ? video_media_toggle() : audio_toggle_play(); break;
    case MC_PAUSE: if (m.playing) video ? video_media_toggle() : audio_toggle_play(); break;
    case MC_NEXT: if (!video) audio_next_track(); break;
    case MC_PREV: if (!video) audio_prev_track(); break;
    case MC_STOP:
      if (video && video_mini_active()) video_bg_stop();
      else if (video) video_media_toggle();          /* the player on screen: pause it */
      else audio_stop();
      break;
    default: break;
  }
}

static struct {
  bool shown;
  char key[600];                       /* what Java shows now */
  int track;
  u64 next;
} S;

void media_pump(void) {
  int cmd = SDL_AtomicSet(&g_cmd, MC_NONE);
  if (cmd != MC_NONE) {
    run(cmd);
    S.next = 0;                        /* show the change at once */
  }
  u64 t = SDL_GetTicks64();
  if (t < S.next) return;
  S.next = t + 500;

  FmMediaInfo m;
  int who = now(&m, false);
  if (!who) {
    if (S.shown) {
      jobject act;
      JNIEnv *e = java(&act);
      if (e) {
        (*e)->CallStaticVoidMethod(e, J.cls, J.stop, act);
        if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
        (*e)->DeleteLocalRef(e, act);
      }
      S.shown = false;
      S.key[0] = 0;
    }
    return;
  }
  char key[sizeof S.key];
  fm_snprintf(key, sizeof key, "%d|%d|%d|%d|%s|%s", m.track, (int)m.playing, (int)m.can_skip, (int)m.live, m.title,
              m.artist);
  if (S.shown && !strcmp(key, S.key)) return;
  bool new_art = !S.shown || m.track != S.track;
  if (new_art && who == 1) audio_media_info(&m, true);   /* again, with the cover */

  jobject act;
  JNIEnv *e = java(&act);
  if (e) {
    jintArray art = NULL;
    if (m.cover && m.cover_w > 0 && m.cover_h > 0) {
      jsize n = (jsize)m.cover_w * m.cover_h;
      art = (*e)->NewIntArray(e, n);
      jint *px = art ? (jint *)fm_alloc((size_t)n * sizeof(jint)) : NULL;
      if (px) {
        const u8 *s = m.cover;
        for (jsize i = 0; i < n; i++, s += 4)                    /* RGBA bytes to ARGB ints */
          px[i] = (jint)(((u32)s[3] << 24) | ((u32)s[0] << 16) | ((u32)s[1] << 8) | s[2]);
        (*e)->SetIntArrayRegion(e, art, 0, n, px);
        fm_free(px);
      }
    }
    jstring ti = jstr(e, m.title[0] ? m.title : "MMC File Manager"), ar = jstr(e, m.artist);
    (*e)->CallStaticVoidMethod(e, J.cls, J.update, act, ti, ar, (jboolean)m.playing,
                               (jboolean)(who == 1 && m.can_skip), (jboolean)m.live, art, (jint)m.cover_w,
                               (jint)m.cover_h, (jboolean)new_art);
    if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
    (*e)->DeleteLocalRef(e, ti);
    (*e)->DeleteLocalRef(e, ar);
    if (art) (*e)->DeleteLocalRef(e, art);
    (*e)->DeleteLocalRef(e, act);
    S.shown = true;
    fm_strlcpy(S.key, key, sizeof S.key);
    S.track = m.track;
  }
  fm_free(m.cover);
}

#else

void media_pump(void) {}

#endif
