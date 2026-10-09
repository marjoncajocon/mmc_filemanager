/* FmMedia.java -- playback that survives the screen going off.
**
** A foreground service of type mediaPlayback with its notification (title,
** cover art, Previous / Play-Pause / Next / Stop), a MediaSession for the lock
** screen, Bluetooth and headset buttons, audio focus (a phone call or another
** player pauses us) and "becoming noisy" (unplugged headphones pause us).
** src/fmedia.c calls update() / stop() through JNI; every button comes back
** as nativeCommand(), which the app carries out on its main thread.
**
** Design decisions:
**   - Only framework APIs (Notification.MediaStyle, android.media.session):
**     the APK is built without Gradle and has no AndroidX.
**   - The service starts while the app is on screen (as soon as something
**     plays), because Android 12+ refuses to start one from the background.
**   - Everything touching the service, the session and the notification runs
**     on the main looper; update() may come from any thread.
**   - A button pressed after Android ended the app (no native library loaded)
**     just closes the notification.
*/
package io.github.mmc.filemanager;

import android.Manifest;
import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.graphics.Bitmap;
import android.graphics.drawable.Icon;
import android.media.AudioManager;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;

public class FmMedia extends Service {

    static native void nativeCommand(int cmd);

    /* the same numbers as src/fmedia.c */
    static final int CMD_TOGGLE = 1, CMD_PLAY = 2, CMD_PAUSE = 3, CMD_NEXT = 4, CMD_PREV = 5, CMD_STOP = 6;

    private static final String CHANNEL = "playback";
    private static final int NOTE_ID = 7301;
    private static final String EXTRA_CMD = "cmd";

    private static final Handler sMain = new Handler(Looper.getMainLooper());

    /* what plays (written by update(), read on the main looper) */
    private static String sTitle = "", sArtist = "";
    private static boolean sPlaying, sSkip, sLive, sActive;
    private static Bitmap sArt;

    private static Context sApp;
    private static FmMedia sService;
    private static MediaSession sSession;
    private static boolean sAsked, sFocus, sPausedByFocus;
    private static PowerManager.WakeLock sWake;
    private static AudioManager.OnAudioFocusChangeListener sFocusListener;

    /* ---- called from C (src/fmedia.c) ------------------------------------ */

    public static void update(Activity a, String title, String artist, boolean playing, boolean canSkip,
                              boolean live, int[] art, int w, int h, boolean newArt) {
        Bitmap b = null;
        if (newArt && art != null && w > 0 && h > 0) {
            try {
                b = Bitmap.createBitmap(art, w, h, Bitmap.Config.ARGB_8888);
                int m = Math.max(w, h);
                if (m > 512) b = Bitmap.createScaledBitmap(b, w * 512 / m, h * 512 / m, true);
            } catch (Throwable t) {
                b = null;
            }
        }
        final Bitmap nb = b;
        final Context app = a.getApplicationContext();
        askNotifications(a);
        sMain.post(() -> {
            sApp = app;
            sTitle = title != null ? title : "";
            sArtist = artist != null ? artist : "";
            sPlaying = playing;
            sSkip = canSkip;
            sLive = live;
            if (newArt) sArt = nb;
            sActive = true;
            show();
        });
    }

    public static void stop(Activity a) {
        sMain.post(() -> {
            sActive = false;
            if (sService != null) {
                if (Build.VERSION.SDK_INT >= 24) sService.stopForeground(STOP_FOREGROUND_REMOVE);
                else sService.stopForeground(true);
                sService.stopSelf();
            }
            release();
        });
    }

    /* FmActivity.onDestroy (main thread): everything goes now, not posted,
    ** because the process ends right after. */
    static void shutdown(Context c) {
        sActive = false;
        if (sService != null) {
            if (Build.VERSION.SDK_INT >= 24) sService.stopForeground(STOP_FOREGROUND_REMOVE);
            else sService.stopForeground(true);
            sService.stopSelf();
        }
        NotificationManager nm = (NotificationManager) c.getSystemService(NOTIFICATION_SERVICE);
        if (nm != null) nm.cancel(NOTE_ID);
        if (sApp == null) sApp = c.getApplicationContext();
        release();
    }

    /* Android 13+: notifications need a grant; asked once per run. Without
    ** it playback still goes on, only the notification is not shown. */
    private static void askNotifications(Activity a) {
        if (Build.VERSION.SDK_INT < 33 || sAsked) return;
        sAsked = true;
        if (a.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) return;
        a.runOnUiThread(() -> {
            try {
                a.requestPermissions(new String[] { Manifest.permission.POST_NOTIFICATIONS }, 4202);
            } catch (Throwable t) {
                /* not fatal */
            }
        });
    }

    /* ---- the session, the notification, focus ------------------------------- */

    private static void send(int cmd) {
        try {
            nativeCommand(cmd);
        } catch (Throwable t) {
            /* the app is gone (no native library): drop the notification */
            sActive = false;
            if (sService != null) sService.stopSelf();
            release();
        }
    }

    private static void show() {
        if (sApp == null) return;
        session();
        focus(sPlaying);
        wake(sPlaying);
        if (sService == null) {
            Intent i = new Intent(sApp, FmMedia.class);
            try {
                if (Build.VERSION.SDK_INT >= 26) sApp.startForegroundService(i);
                else sApp.startService(i);
            } catch (Throwable t) {
                /* Android 12+ in the background: try again on the next change */
            }
        } else {
            sService.post();
        }
    }

    private static void session() {
        if (sSession == null) {
            sSession = new MediaSession(sApp, "mmcfm");
            sSession.setCallback(new MediaSession.Callback() {
                @Override public void onPlay() { send(CMD_PLAY); }
                @Override public void onPause() { send(CMD_PAUSE); }
                @Override public void onSkipToNext() { send(CMD_NEXT); }
                @Override public void onSkipToPrevious() { send(CMD_PREV); }
                @Override public void onStop() { send(CMD_STOP); }
            });
            Intent open = new Intent(sApp, FmActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            sSession.setSessionActivity(PendingIntent.getActivity(sApp, 0, open, flags()));
            sSession.setActive(true);
        }
        MediaMetadata.Builder md = new MediaMetadata.Builder()
            .putString(MediaMetadata.METADATA_KEY_TITLE, sTitle)
            .putString(MediaMetadata.METADATA_KEY_ARTIST, sArtist);
        if (sArt != null) md.putBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART, sArt);
        sSession.setMetadata(md.build());
        long actions = PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE | PlaybackState.ACTION_PLAY_PAUSE
            | PlaybackState.ACTION_STOP;
        if (sSkip) actions |= PlaybackState.ACTION_SKIP_TO_NEXT | PlaybackState.ACTION_SKIP_TO_PREVIOUS;
        sSession.setPlaybackState(new PlaybackState.Builder()
            .setActions(actions)
            .setState(sPlaying ? PlaybackState.STATE_PLAYING : PlaybackState.STATE_PAUSED,
                      PlaybackState.PLAYBACK_POSITION_UNKNOWN, sPlaying ? 1f : 0f)
            .build());
    }

    private static int flags() {
        int f = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= 23) f |= PendingIntent.FLAG_IMMUTABLE;
        return f;
    }

    private static void focus(boolean want) {
        AudioManager am = (AudioManager) sApp.getSystemService(Context.AUDIO_SERVICE);
        if (am == null) return;
        if (sFocusListener == null) {
            sFocusListener = change -> {
                if (change == AudioManager.AUDIOFOCUS_LOSS) {
                    sFocus = false;
                    sPausedByFocus = false;
                    if (sPlaying) send(CMD_PAUSE);
                } else if (change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT) {
                    if (sPlaying) { sPausedByFocus = true; send(CMD_PAUSE); }
                } else if (change == AudioManager.AUDIOFOCUS_GAIN) {
                    sFocus = true;
                    if (sPausedByFocus) { sPausedByFocus = false; send(CMD_PLAY); }
                }
            };
        }
        if (want && !sFocus) {
            sFocus = am.requestAudioFocus(sFocusListener, AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN)
                == AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
        }
    }

    private static void wake(boolean on) {
        if (on && sWake == null) {
            PowerManager pm = (PowerManager) sApp.getSystemService(Context.POWER_SERVICE);
            if (pm == null) return;
            sWake = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "mmcfm:playback");
            sWake.setReferenceCounted(false);
            sWake.acquire();
        } else if (!on && sWake != null) {
            if (sWake.isHeld()) sWake.release();
            sWake = null;
        }
    }

    private static void release() {
        if (sApp != null) {
            wake(false);
            if (sFocus && sFocusListener != null) {
                AudioManager am = (AudioManager) sApp.getSystemService(Context.AUDIO_SERVICE);
                if (am != null) am.abandonAudioFocus(sFocusListener);
            }
        }
        sFocus = false;
        sPausedByFocus = false;
        if (sSession != null) {
            sSession.setActive(false);
            sSession.release();
            sSession = null;
        }
    }

    /* ---- the service ---------------------------------------------------------- */

    private final BroadcastReceiver mNoisy = new BroadcastReceiver() {
        @Override public void onReceive(Context c, Intent i) {
            if (sPlaying) send(CMD_PAUSE);              /* headphones unplugged */
        }
    };

    @Override public void onCreate() {
        super.onCreate();
        sService = this;
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
            NotificationChannel ch = new NotificationChannel(CHANNEL, "Playback", NotificationManager.IMPORTANCE_LOW);
            ch.setShowBadge(false);
            if (nm != null) nm.createNotificationChannel(ch);
        }
        registerReceiver(mNoisy, new IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY));
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        /* startForeground first: Android ends a service that is slow to call it */
        Notification n = build();
        try {
            if (Build.VERSION.SDK_INT >= 29) startForeground(NOTE_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
            else startForeground(NOTE_ID, n);
        } catch (Throwable t) {
            stopSelf();
            return START_NOT_STICKY;
        }
        int cmd = intent != null ? intent.getIntExtra(EXTRA_CMD, 0) : 0;
        if (cmd != 0) send(cmd);
        if (!sActive) stopSelf();
        return START_NOT_STICKY;
    }

    @Override public void onDestroy() {
        try { unregisterReceiver(mNoisy); } catch (Throwable t) { /* not registered */ }
        if (sService == this) sService = null;
        super.onDestroy();
    }

    @Override public IBinder onBind(Intent intent) { return null; }

    void post() {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm != null) nm.notify(NOTE_ID, build());
    }

    private PendingIntent action(int cmd) {
        Intent i = new Intent(this, FmMedia.class).putExtra(EXTRA_CMD, cmd).setAction("cmd" + cmd);
        if (Build.VERSION.SDK_INT >= 26) return PendingIntent.getForegroundService(this, cmd, i, flags());
        return PendingIntent.getService(this, cmd, i, flags());
    }

    private Notification.Action button(int icon, String label, int cmd) {
        return new Notification.Action.Builder(Icon.createWithResource(this, icon), label, action(cmd)).build();
    }

    private Notification build() {
        Notification.Builder b = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(this, CHANNEL)
                                                             : new Notification.Builder(this);
        int small = getResources().getIdentifier("ic_notify", "drawable", getPackageName());
        if (small == 0) small = getApplicationInfo().icon;
        Intent open = new Intent(this, FmActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        b.setSmallIcon(small)
         .setContentTitle(sTitle.isEmpty() ? "MMC File Manager" : sTitle)
         .setContentText(sLive && sArtist.isEmpty() ? "Live" : sArtist)
         .setContentIntent(PendingIntent.getActivity(this, 0, open, flags()))
         .setVisibility(Notification.VISIBILITY_PUBLIC)
         .setOngoing(sPlaying)
         .setShowWhen(false);
        if (sArt != null) b.setLargeIcon(sArt);
        int n = 0;
        if (sSkip) { b.addAction(button(android.R.drawable.ic_media_previous, "Previous", CMD_PREV)); n++; }
        int play = n;
        b.addAction(sPlaying ? button(android.R.drawable.ic_media_pause, "Pause", CMD_TOGGLE)
                             : button(android.R.drawable.ic_media_play, "Play", CMD_TOGGLE));
        n++;
        if (sSkip) { b.addAction(button(android.R.drawable.ic_media_next, "Next", CMD_NEXT)); n++; }
        b.addAction(button(android.R.drawable.ic_menu_close_clear_cancel, "Stop", CMD_STOP));
        Notification.MediaStyle st = new Notification.MediaStyle();
        if (sSession != null) st.setMediaSession(sSession.getSessionToken());
        st.setShowActionsInCompactView(sSkip ? new int[] { 0, play, 2 } : new int[] { play, 1 });
        b.setStyle(st);
        return b.build();
    }
}
