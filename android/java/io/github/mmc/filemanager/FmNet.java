package io.github.mmc.filemanager;

import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/* FmNet.java -- HTTP(S) for the native side (src/fnet.c), on HttpURLConnection,
** so TLS and the trusted certificates are the system's own.
**
** One connection per request; fnet.c pulls the body with read() from its
** worker thread. Redirects are followed by hand (the built-in follower never
** crosses http <-> https). Transparent gzip is kept for API replies but turned
** off for Range requests, whose byte offsets must stay exact. */
public final class FmNet {
    private FmNet() {}

    public static final class Conn {
        HttpURLConnection h;
        InputStream in;
        public int status;
        public long length = -1;
        public String type = "", error = "";
    }

    public static Conn open(String url, String headers, String agent) {
        Conn c = new Conn();
        try {
            for (int hop = 0; hop < 8; hop++) {
                HttpURLConnection h = (HttpURLConnection) new URL(url).openConnection();
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
                int st = h.getResponseCode();
                if (st >= 300 && st < 400 && st != 304) {
                    String loc = h.getHeaderField("Location");
                    h.disconnect();
                    if (loc == null) { c.error = "redirect without a location"; return c; }
                    url = new URL(new URL(url), loc).toString();
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
            return n < 0 ? 0 : n;
        } catch (Throwable e) {
            String m = e.getMessage();
            c.error = e.getClass().getSimpleName() + (m != null ? ": " + m : "");
            return -1;
        }
    }

    public static void close(Conn c) {
        try { if (c.in != null) c.in.close(); } catch (Throwable e) { /* closing anyway */ }
        if (c.h != null) c.h.disconnect();
        c.in = null;
        c.h = null;
    }
}
