package io.github.mmc.filemanager;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.Network;
import android.os.Build;

import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

import org.libsdl.app.SDLActivity;

/* FmNet.java -- HTTP(S) for the native side (src/fnet.c), on HttpURLConnection,
** so TLS and the trusted certificates are the system's own.
**
** One connection per request; fnet.c pulls the body with read() from its
** worker thread. Redirects are followed by hand (the built-in follower never
** crosses http <-> https). Transparent gzip is kept for API replies but turned
** off for Range requests, whose byte offsets must stay exact.
**
** Every request goes over the current default network (Android 6+). Phones
** with "network acceleration" (seen on Honor) otherwise send some sockets
** over mobile data while on Wi-Fi: another public address, and YouTube's
** stream links, bound to the address that asked for them, answer 403. */
public final class FmNet {
    private FmNet() {}

    private static HttpURLConnection connect(URL u) throws java.io.IOException {
        if (Build.VERSION.SDK_INT >= 23) {
            try {
                Context ctx = SDLActivity.getContext();
                ConnectivityManager cm = ctx == null ? null
                        : (ConnectivityManager) ctx.getSystemService(Context.CONNECTIVITY_SERVICE);
                Network n = cm == null ? null : cm.getActiveNetwork();
                if (n != null) return (HttpURLConnection) n.openConnection(u);
            } catch (SecurityException e) { /* no permission: the system's choice */ }
        }
        return (HttpURLConnection) u.openConnection();
    }

    public static final class Conn {
        HttpURLConnection h;
        InputStream in;
        public int status;
        public long length = -1;
        public String type = "", error = "";
        boolean ended;   /* the body was read to its end */
    }

    /* body != null: a POST (redirects then turn into GETs, as browsers do) */
    public static Conn open(String url, String headers, String agent, byte[] body) {
        Conn c = new Conn();
        try {
            for (int hop = 0; hop < 8; hop++) {
                HttpURLConnection h = connect(new URL(url));
                h.setInstanceFollowRedirects(false);
                h.setConnectTimeout(10000);
                h.setReadTimeout(30000);
                h.setRequestProperty("User-Agent", agent);
                boolean range = false;
                if (headers != null) {
                    for (String line : headers.split("\r\n|\n")) {
                        int k = line.indexOf(':');
                        if (k <= 0) continue;
                        String name = line.substring(0, k).trim(), v = line.substring(k + 1).trim();
                        if (name.equalsIgnoreCase("Range")) range = true;
                        h.setRequestProperty(name, v);
                    }
                }
                if (range) h.setRequestProperty("Accept-Encoding", "identity");
                if (body != null) {
                    h.setDoOutput(true);
                    h.setFixedLengthStreamingMode(body.length);
                    java.io.OutputStream os = h.getOutputStream();
                    os.write(body);
                    os.close();
                }
                int st = h.getResponseCode();
                if (st >= 300 && st < 400 && st != 304) {
                    String loc = h.getHeaderField("Location");
                    h.disconnect();
                    if (loc == null) { c.error = "redirect without a location"; return c; }
                    url = new URL(new URL(url), loc).toString();
                    body = null;
                    continue;
                }
                c.h = h;
                c.status = st;
                String cl = h.getHeaderField("Content-Length");
                if (cl != null) try { c.length = Long.parseLong(cl.trim()); } catch (NumberFormatException e) { c.length = -1; }
                String t = h.getContentType();
                c.type = t != null ? t : "";
                c.in = st >= 400 ? h.getErrorStream() : h.getInputStream();
                return c;
            }
            c.error = "too many redirects";
        } catch (Throwable e) {
            String m = e.getMessage();
            c.error = e.getClass().getSimpleName() + (m != null ? ": " + m : "");
        }
        return c;
    }

    /* a response header by name, "" when absent */
    public static String header(Conn c, String name) {
        String v = c.h != null ? c.h.getHeaderField(name) : null;
        return v != null ? v : "";
    }

    /* bytes read, 0 at the end, -1 on an error (then c.error says why) */
    public static int read(Conn c, byte[] buf) {
        if (c.in == null) return 0;
        try {
            int n = c.in.read(buf);
            if (n < 0) c.ended = true;
            return n < 0 ? 0 : n;
        } catch (Throwable e) {
            String m = e.getMessage();
            c.error = e.getClass().getSimpleName() + (m != null ? ": " + m : "");
            return -1;
        }
    }

    /* A body read to its end leaves the connection in the system's keep-alive
       pool: HLS fetches a segment every few seconds and each fresh TLS
       handshake cost a phone ~0.5-1 s. A body left unread (a cancel, a seek)
       is disconnected so the pool never keeps a half-read connection. */
    public static void close(Conn c) {
        try { if (c.in != null) c.in.close(); } catch (Throwable e) { /* closing anyway */ }
        if (c.h != null && !c.ended) c.h.disconnect();
        c.in = null;
        c.h = null;
    }
}
