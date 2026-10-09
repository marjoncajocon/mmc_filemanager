/* FmActivity.java -- the Android shell around the native file manager.
**
** SDLActivity does the window, input and audio; this adds what C cannot
** reach without Java: the storage permission, the list of storage volumes
** and handing files to other apps. src/fplat_android.c calls these methods
** through JNI.
**
** Design decisions:
**   - SDL is linked statically into libmain.so, so only "main" is loaded.
**   - Files are shared through our own tiny ContentProvider (FmFileProvider)
**     instead of AndroidX's FileProvider, because the APK is built without
**     Gradle and has no AndroidX.
*/
package io.github.mmc.filemanager;

import android.Manifest;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.hardware.Sensor;
import android.provider.Settings;
import android.graphics.Rect;
import android.view.View;
import android.view.ViewTreeObserver;
import android.view.WindowManager;
import android.webkit.MimeTypeMap;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import java.io.File;
import java.lang.reflect.Method;
import java.util.List;
import java.util.Locale;

public class FmActivity extends SDLActivity {

    private static final int REQ_STORAGE = 4201;

    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    /* SDL turns the accelerometer on at every resume (SENSOR_DELAY_GAME, about
    ** 50 events a second) for games; a file manager never reads it, and the
    ** stream alone kept the UI thread at ~3% CPU while idle. Rotation still
    ** arrives through onConfigurationChanged (configChanges in the manifest). */
    static final class QuietSurface extends SDLSurface {
        QuietSurface(Context context) { super(context); }

        @Override
        public void enableSensor(int sensortype, boolean enabled) {
            if (enabled && sensortype == Sensor.TYPE_ACCELEROMETER) return;
            super.enableSensor(sensortype, enabled);
        }
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new QuietSurface(context);
    }

    /* ---- test hook ---------------------------------------------------------- */

    /* `adb shell am start -n <pkg>/.FmActivity --es args "--selftest"
       --es env "MMCFM_YT_TEST=...;MMCFM_TEST_ONLY=vsrc"` runs the self test
       on a device. Honoured only while <external files>/allow_test exists:
       a file only the user (or adb) can create, so another app launching the
       exported activity with extras gets the normal app. Arguments split on
       spaces, env entries on ';'. */
    @Override
    protected String[] getArguments() {
        Intent it = getIntent();
        File dir = getExternalFilesDir(null);
        if (it == null || dir == null || !new File(dir, "allow_test").exists()) return new String[0];
        String env = it.getStringExtra("env");
        if (env != null) {
            for (String kv : env.split(";")) {
                int k = kv.indexOf('=');
                if (k <= 0) continue;
                try { android.system.Os.setenv(kv.substring(0, k).trim(), kv.substring(k + 1), true); }
                catch (Exception e) { /* skip that one */ }
            }
        }
        String args = it.getStringExtra("args");
        return args == null || args.trim().isEmpty() ? new String[0] : args.trim().split("\\s+");
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        /* Draw behind the status/navigation bars only when the app asks for
           fullscreen; keep the default system bars otherwise. */
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        watchKeyboard();
    }

    /* ---- soft keyboard height ---------------------------------------------- */

    /* SDL's surface keeps its full size when the keyboard opens, so dialogs
       at the bottom would sit under it. The visible frame of the window says
       how much the keyboard covers (works from API 26 on, no AndroidX); the
       native side lays dialogs out above it. */
    private int mImePx = -1;

    private static native void nativeImeInset(int px);

    private void watchKeyboard() {
        final View root = getWindow().getDecorView();
        root.getViewTreeObserver().addOnGlobalLayoutListener(new ViewTreeObserver.OnGlobalLayoutListener() {
            @Override
            public void onGlobalLayout() {
                Rect vis = new Rect();
                root.getWindowVisibleDisplayFrame(vis);
                int full = root.getHeight();
                int covered = full - vis.bottom;
                /* small values are the navigation bar, not a keyboard */
                int px = covered > full * 0.15f ? covered : 0;
                if (px != mImePx) {
                    mImePx = px;
                    try {
                        nativeImeInset(px);
                    } catch (UnsatisfiedLinkError e) {
                        /* native library not loaded yet: the next layout pass retries */
                        mImePx = -1;
                    }
                }
            }
        });
    }

    /* ---- storage permission -------------------------------------------- */

    public boolean fmHasStorage() {
        if (Build.VERSION.SDK_INT >= 30) {
            return Environment.isExternalStorageManager();
        }
        if (Build.VERSION.SDK_INT >= 23) {
            return checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                    == PackageManager.PERMISSION_GRANTED;
        }
        return true;
    }

    public void fmRequestStorage() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (Build.VERSION.SDK_INT >= 30) {
                    try {
                        Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                Uri.parse("package:" + getPackageName()));
                        startActivity(i);
                    } catch (ActivityNotFoundException e) {
                        try {
                            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                        } catch (ActivityNotFoundException ignored) {
                            /* no settings screen on this device */
                        }
                    }
                } else if (Build.VERSION.SDK_INT >= 23) {
                    requestPermissions(new String[] {
                            Manifest.permission.READ_EXTERNAL_STORAGE,
                            Manifest.permission.WRITE_EXTERNAL_STORAGE }, REQ_STORAGE);
                }
            }
        });
    }

    /* ---- volumes -------------------------------------------------------- */

    /** One line per volume: path TAB label TAB kind (internal|sd|usb). */
    public String fmVolumes() {
        StringBuilder sb = new StringBuilder();
        String primary = Environment.getExternalStorageDirectory().getAbsolutePath();
        sb.append(primary).append('\t').append("Internal storage").append('\t').append("internal").append('\n');
        if (Build.VERSION.SDK_INT >= 24) {
            try {
                StorageManager sm = (StorageManager) getSystemService(Context.STORAGE_SERVICE);
                List<StorageVolume> vols = sm.getStorageVolumes();
                for (StorageVolume v : vols) {
                    if (v.isPrimary()) continue;
                    String state = v.getState();
                    if (!Environment.MEDIA_MOUNTED.equals(state)
                            && !Environment.MEDIA_MOUNTED_READ_ONLY.equals(state)) continue;
                    String path = volumePath(v);
                    if (path == null) continue;
                    String label = v.getDescription(this);
                    String kind = "sd";
                    if (label != null && label.toLowerCase(Locale.ROOT).contains("usb")) kind = "usb";
                    sb.append(path).append('\t').append(label == null ? path : label)
                      .append('\t').append(kind).append('\n');
                }
            } catch (Exception ignored) {
                /* fall back to the primary volume only */
            }
        }
        return sb.toString();
    }

    private static String volumePath(StorageVolume v) {
        if (Build.VERSION.SDK_INT >= 30) {
            File d = v.getDirectory();
            return d == null ? null : d.getAbsolutePath();
        }
        try {
            Method m = StorageVolume.class.getMethod("getPath");
            Object r = m.invoke(v);
            return r == null ? null : r.toString();
        } catch (Exception e) {
            return null;
        }
    }

    /* ---- open with / share ---------------------------------------------- */

    private static String mimeOf(String path) {
        int dot = path.lastIndexOf('.');
        if (dot >= 0) {
            String ext = path.substring(dot + 1).toLowerCase(Locale.ROOT);
            String m = MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext);
            if (m != null) return m;
        }
        return "*/*";
    }

    public boolean fmOpenFile(final String path) {
        final Uri uri = FmFileProvider.uriFor(path);
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Intent i = new Intent(Intent.ACTION_VIEW);
                i.setDataAndType(uri, mimeOf(path));
                i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
                try {
                    startActivity(Intent.createChooser(i, null));
                } catch (ActivityNotFoundException ignored) {
                    /* nothing can open it */
                }
            }
        });
        return true;
    }

    public boolean fmShare(final String path) {
        final Uri uri = FmFileProvider.uriFor(path);
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Intent i = new Intent(Intent.ACTION_SEND);
                i.setType(mimeOf(path));
                i.putExtra(Intent.EXTRA_STREAM, uri);
                i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
                try {
                    startActivity(Intent.createChooser(i, null));
                } catch (ActivityNotFoundException ignored) {
                    /* no share target */
                }
            }
        });
        return true;
    }
}
