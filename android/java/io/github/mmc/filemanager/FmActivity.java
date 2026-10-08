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
import android.provider.Settings;
import android.view.WindowManager;
import android.webkit.MimeTypeMap;

import org.libsdl.app.SDLActivity;

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

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        /* Draw behind the status/navigation bars only when the app asks for
           fullscreen; keep the default system bars otherwise. */
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
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
