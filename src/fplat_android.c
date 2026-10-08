/* fplat_android.c -- the Android-only parts of fplat.h, through JNI.
**
** fplat_posix.c does files and folders on Android as everywhere else; this
** file adds what needs Java: the storage permission, the volume list and
** handing files to other apps. The methods live in FmActivity.java.
**
** Design decisions:
**   - Every call attaches through SDL_AndroidGetJNIEnv (valid on any SDL
**     thread) and releases its local references, so it is safe from the UI
**     thread and from job workers alike.
**   - The permission answer is cached for a second; the UI asks every frame.
*/
#include "fcore.h"
#ifdef FM_ANDROID
#include "fplat.h"
#include "fsdl.h"
#include <jni.h>

int plat_android_volumes(FmVolume *out, int max);

/* ---- jni helpers -------------------------------------------------------- */

typedef struct Jni {
  JNIEnv *env;
  jobject act;
  jclass cls;
} Jni;

static bool jni_begin(Jni *j) {
  j->env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  if (!j->env) return false;
  j->act = (jobject)SDL_AndroidGetActivity();
  if (!j->act) return false;
  j->cls = (*j->env)->GetObjectClass(j->env, j->act);
  return j->cls != NULL;
}

static void jni_end(Jni *j) {
  JNIEnv *e = j->env;
  if (!e) return;
  if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
  if (j->cls) (*e)->DeleteLocalRef(e, j->cls);
  if (j->act) (*e)->DeleteLocalRef(e, j->act);
}

static jmethodID method(Jni *j, const char *name, const char *sig) {
  jmethodID m = (*j->env)->GetMethodID(j->env, j->cls, name, sig);
  if (!m && (*j->env)->ExceptionCheck(j->env)) (*j->env)->ExceptionClear(j->env);
  return m;
}

static bool call_bool_str(const char *name, const char *arg) {
  Jni j;
  bool r = false;
  if (jni_begin(&j)) {
    jmethodID m = method(&j, name, "(Ljava/lang/String;)Z");
    if (m) {
      jstring s = (*j.env)->NewStringUTF(j.env, arg);
      r = (*j.env)->CallBooleanMethod(j.env, j.act, m, s) != JNI_FALSE;
      (*j.env)->DeleteLocalRef(j.env, s);
    }
  }
  jni_end(&j);
  return r;
}

/* ---- permission --------------------------------------------------------- */

static u64 g_perm_at;
static bool g_perm;

bool plat_storage_granted(void) {
  u64 now = plat_now_ms();
  if (g_perm_at && now - g_perm_at < 1000) return g_perm;
  Jni j;
  bool r = false;
  if (jni_begin(&j)) {
    jmethodID m = method(&j, "fmHasStorage", "()Z");
    if (m) r = (*j.env)->CallBooleanMethod(j.env, j.act, m) != JNI_FALSE;
  }
  jni_end(&j);
  g_perm = r;
  g_perm_at = now;
  return r;
}

void plat_storage_request(void) {
  Jni j;
  if (jni_begin(&j)) {
    jmethodID m = method(&j, "fmRequestStorage", "()V");
    if (m) (*j.env)->CallVoidMethod(j.env, j.act, m);
  }
  jni_end(&j);
  g_perm_at = 0;
}

/* ---- volumes ------------------------------------------------------------ */

int plat_android_volumes(FmVolume *out, int max) {
  int n = 0;
  Jni j;
  if (jni_begin(&j)) {
    jmethodID m = method(&j, "fmVolumes", "()Ljava/lang/String;");
    jstring s = m ? (jstring)(*j.env)->CallObjectMethod(j.env, j.act, m) : NULL;
    if (s) {
      const char *u = (*j.env)->GetStringUTFChars(j.env, s, NULL);
      /* "path\tlabel\tkind\n" per volume */
      const char *p = u;
      while (u && *p && n < max) {
        const char *e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        char line[FM_PATH_MAX + 128];
        size_t len = (size_t)(e - p);
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = 0;
        char *t1 = strchr(line, '\t');
        char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (t1 && t2) {
          *t1 = 0;
          *t2 = 0;
          FmVolume *v = &out[n];
          memset(v, 0, sizeof *v);
          fm_strlcpy(v->path, line, sizeof v->path);
          fm_strlcpy(v->name, t1 + 1, sizeof v->name);
          v->kind = strcmp(t2 + 1, "internal") == 0 ? VOL_INTERNAL :
                    strcmp(t2 + 1, "usb") == 0 ? VOL_USB : VOL_SDCARD;
          plat_disk_space(v->path, &v->total, &v->free);
          n++;
        }
        p = *e ? e + 1 : e;
      }
      if (u) (*j.env)->ReleaseStringUTFChars(j.env, s, u);
      (*j.env)->DeleteLocalRef(j.env, s);
    }
  }
  jni_end(&j);
  if (n == 0 && max > 0) {
    FmVolume *v = &out[0];
    memset(v, 0, sizeof *v);
    fm_strlcpy(v->path, "/storage/emulated/0", sizeof v->path);
    fm_strlcpy(v->name, "Internal storage", sizeof v->name);
    v->kind = VOL_INTERNAL;
    plat_disk_space(v->path, &v->total, &v->free);
    n = 1;
  }
  return n;
}

/* ---- other apps --------------------------------------------------------- */

bool plat_open_external(const char *path) { return call_bool_str("fmOpenFile", path); }
bool plat_share(const char *path) { return call_bool_str("fmShare", path); }

/* ---- SDL's HIDDeviceManager natives -------------------------------------- */

/* SDL is built without HIDAPI (no game controllers in a file manager, and it
** would pull in C++), but SDLActivity.onCreate still constructs
** HIDDeviceManager, which calls these natives. Without them the app dies at
** start with UnsatisfiedLinkError. USB/Bluetooth scanning only starts when
** native HIDAPI asks for it, so empty bodies are complete. */
#define HID_FN(name) JNIEXPORT void JNICALL Java_org_libsdl_app_HIDDeviceManager_##name

HID_FN(HIDDeviceRegisterCallback)(JNIEnv *e, jobject o) { (void)e; (void)o; }
HID_FN(HIDDeviceReleaseCallback)(JNIEnv *e, jobject o) { (void)e; (void)o; }
HID_FN(HIDDeviceConnected)(JNIEnv *e, jobject o, jint id, jstring ident, jint vendor, jint product,
                           jstring serial, jint release, jstring maker, jstring prod, jint iface,
                           jint iclass, jint isub, jint iproto) {
  (void)e; (void)o; (void)id; (void)ident; (void)vendor; (void)product; (void)serial; (void)release;
  (void)maker; (void)prod; (void)iface; (void)iclass; (void)isub; (void)iproto;
}
HID_FN(HIDDeviceOpenPending)(JNIEnv *e, jobject o, jint id) { (void)e; (void)o; (void)id; }
HID_FN(HIDDeviceOpenResult)(JNIEnv *e, jobject o, jint id, jboolean ok) { (void)e; (void)o; (void)id; (void)ok; }
HID_FN(HIDDeviceDisconnected)(JNIEnv *e, jobject o, jint id) { (void)e; (void)o; (void)id; }
HID_FN(HIDDeviceInputReport)(JNIEnv *e, jobject o, jint id, jbyteArray r) { (void)e; (void)o; (void)id; (void)r; }
HID_FN(HIDDeviceFeatureReport)(JNIEnv *e, jobject o, jint id, jbyteArray r) { (void)e; (void)o; (void)id; (void)r; }

#endif
