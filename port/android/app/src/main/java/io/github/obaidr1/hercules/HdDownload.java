package io.github.obaidr1.hercules;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.HashMap;
import java.util.Map;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;

import org.json.JSONArray;
import org.json.JSONObject;
import org.tukaani.xz.XZInputStream;

/* The HD pack on Android: the same ready pack the PC launcher's hd/hd_auto.py
 * downloads (GitHub release "hd-packs", hercules/tools/hd_pack_release.py),
 * without Python. Called from the launcher (hl_launcher.cpp, JNI):
 *
 *   start(api, serial, pack, data)   downloads hd-<serial>-core.tar.xz and
 *                                    hd-<serial>-levels.tar to data/downloads,
 *                                    checks each against SHA256SUMS.txt,
 *                                    unpacks them to <pack>.installing and
 *                                    swaps that in as <pack>
 *   running()                        the download thread is alive
 *   allowMobile(on)                  also over a metered network (mobile
 *                                    data); off: it waits for Wi-Fi
 *   stop()                           ends it; a partial download stays and is
 *                                    continued (HTTP Range) on the next start
 *
 * data/progress.json is the same file hd_auto.py writes:
 * {"phase": download|install, "fraction", "eta": null,
 *  "state": running|done|failed, "error": network|disk|download|null}.
 * Building the pack from the disc (the PC's fallback) is not done on a phone.
 */
public final class HdDownload {
    private static final long FREE_BYTES = 4L << 30;   // levels.tar ~1.3 GB + unpacked pack ~1.8 GB
    private static final String MARKER = "hd-release.json";
    private static volatile Thread thread;
    private static volatile boolean cancel;
    private static volatile boolean mobileOk;
    private static volatile double lastFraction;

    private HdDownload() { }

    static final class Failed extends Exception {
        final String code;
        Failed(String code, String detail) { super(code + ": " + detail); this.code = code; }
    }

    public static void allowMobile(boolean on) { mobileOk = on; }

    /* An unmetered network (Wi-Fi, Ethernet), or any network when mobile data
     * is allowed. */
    private static boolean networkOk() {
        try {
            Context ctx = org.libsdl.app.SDLActivity.getContext();
            ConnectivityManager cm = ctx == null ? null : (ConnectivityManager) ctx.getSystemService(Context.CONNECTIVITY_SERVICE);
            Network n = cm == null ? null : cm.getActiveNetwork();
            NetworkCapabilities c = n == null ? null : cm.getNetworkCapabilities(n);
            if (c == null || !c.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)) return false;
            return mobileOk || c.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED);
        } catch (Exception e) {
            return true;                                  // unknown: let the download itself tell
        }
    }

    /* Phase "wifi" (the launcher shows "waiting for Wi-Fi") until the network suits. */
    private static void waitForNetwork() throws Failed {
        while (!networkOk()) {
            if (cancel) throw new Failed("stopped", "while waiting for Wi-Fi");
            progress("wifi", lastFraction, "running", null, true);
            try { Thread.sleep(5000); } catch (InterruptedException ignored) { }
        }
    }

    public static synchronized boolean start(String api, String serial, String pack, String data) {
        if (thread != null && thread.isAlive()) return true;
        cancel = false;
        thread = new Thread(() -> run(api, serial, new File(pack), new File(data)), "hd-download");
        thread.setPriority(Thread.MIN_PRIORITY);
        thread.start();
        return true;
    }

    public static boolean running() {
        Thread t = thread;
        return t != null && t.isAlive();
    }

    public static synchronized void stop() {
        Thread t = thread;
        if (t == null) return;
        cancel = true;
        try { t.join(5000); } catch (InterruptedException ignored) { }
    }

    // ------------------------------------------------------------ progress
    private static File progressFile;
    private static long lastWrite;

    private static void progress(String phase, double fraction, String state, String error, boolean force) {
        long now = System.currentTimeMillis();
        if (!force && now - lastWrite < 500) return;
        lastWrite = now;
        if (!phase.equals("wifi")) lastFraction = fraction;
        String json = "{\"phase\": \"" + phase + "\", \"fraction\": " + String.format(java.util.Locale.ROOT, "%.4f",
                Math.max(0, Math.min(1, fraction))) + ", \"eta\": null, \"state\": \"" + state + "\", \"error\": "
                + (error == null ? "null" : "\"" + error + "\"") + ", \"updated\": " + now / 1000 + "}";
        File tmp = new File(progressFile.getPath() + ".tmp");
        try (OutputStream o = new FileOutputStream(tmp)) {
            o.write(json.getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            return;
        }
        tmp.renameTo(progressFile);
    }

    private static void log(File data, String msg) {
        try (OutputStream o = new FileOutputStream(new File(data, "hd_download.log"), true)) {
            o.write((new java.text.SimpleDateFormat("HH:mm:ss ", java.util.Locale.ROOT).format(new java.util.Date())
                     + msg + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (IOException ignored) { }
    }

    // ----------------------------------------------------------------- run
    private static void run(String api, String serial, File pack, File data) {
        data.mkdirs();
        progressFile = new File(data, "progress.json");
        progress("download", 0, "running", null, true);
        try {
            install(api, serial, pack, data);
            progress("install", 1, "done", null, true);
            log(data, "HD pack " + serial + " installed");
        } catch (Failed f) {
            log(data, "failed: " + f.getMessage());
            progress("download", 0, cancel ? "stopped" : "failed", f.code, true);
        } catch (Exception e) {
            log(data, "failed: " + e);
            progress("download", 0, "failed", "failed", true);
        }
    }

    private static void install(String api, String serial, File pack, File data) throws Failed {
        waitForNetwork();
        Map<String, String[]> assets = releaseAssets(api);   // name -> {url, sha256, size}
        String core = "hd-" + serial + "-core.tar.xz", levels = "hd-" + serial + "-levels.tar";
        if (!assets.containsKey(core) || !assets.containsKey(levels))
            throw new Failed("download", "no ready pack for " + serial);
        String[] names = {core, levels};
        long total = 0;
        for (String n : names) total += Long.parseLong(assets.get(n)[2]);
        File downloads = new File(data, "downloads");
        downloads.mkdirs();
        if (data.getUsableSpace() < FREE_BYTES - partialBytes(downloads))
            throw new Failed("disk", data.getUsableSpace() / (1 << 20) + " MB free");
        File tmp = new File(pack.getPath() + ".installing");
        deleteTree(tmp);
        tmp.mkdirs();
        // each archive counts once for the download and a quarter for unpacking it
        final double weight = 1.25 * total;
        double done = 0;
        for (String n : names) {
            String[] a = assets.get(n);
            long size = Long.parseLong(a[2]);
            final double base = done;
            File f = download(a[0], new File(downloads, n), a[1], size,
                              got -> progress("download", (base + got) / weight, "running", null, false));
            unpack(f, tmp, n.endsWith(".xz"),
                   read -> progress("download", (base + size + 0.25 * read) / weight, "running", null, false));
            f.delete();                                    // keeps the disk need at one archive
            done += 1.25 * size;
            log(data, n + ": " + size / (1 << 20) + " MB unpacked");
        }
        progress("install", 0.5, "running", null, true);
        if (!new File(tmp, "replacements").isDirectory() || !new File(tmp, "config.yaml").exists())
            throw new Failed("download", "archives without config.yaml / replacements/");
        writeText(new File(tmp, MARKER), "{\"serial\": \"" + serial + "\", \"core\": \"" + assets.get(core)[1]
                  + "\", \"levels\": \"" + assets.get(levels)[1] + "\"}");
        pack.getParentFile().mkdirs();
        File old = new File(pack.getPath() + ".replaced");
        deleteTree(old);
        if (pack.exists() && !pack.renameTo(old)) throw new Failed("disk", "cannot move " + pack);
        if (!tmp.renameTo(pack)) throw new Failed("disk", "cannot install " + pack);
        deleteTree(old);
    }

    private static long partialBytes(File downloads) {
        long n = 0;
        File[] fs = downloads.listFiles();
        if (fs != null) for (File f : fs) n += f.length();
        return n;
    }

    // -------------------------------------------------------------- network
    private static HttpURLConnection open(String url, long from) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(20000);
        c.setReadTimeout(60000);
        c.setInstanceFollowRedirects(true);
        c.setRequestProperty("User-Agent", "hercules-recompiled-hd");
        if (from > 0) c.setRequestProperty("Range", "bytes=" + from + "-");
        return c;
    }

    private static String fetchText(String url, boolean json) throws Failed {
        HttpURLConnection c = null;
        try {
            c = open(url, 0);
            if (json) c.setRequestProperty("Accept", "application/vnd.github+json");
            int code = c.getResponseCode();
            if (code == 404) throw new Failed("download", url + ": 404");
            if (code != 200) throw new Failed("network", url + ": HTTP " + code);
            try (InputStream in = c.getInputStream()) {
                java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
                byte[] buf = new byte[16384];
                for (int r; (r = in.read(buf)) > 0; ) b.write(buf, 0, r);
                return b.toString("UTF-8");
            }
        } catch (IOException e) {
            throw new Failed("network", url + ": " + e);
        } finally {
            if (c != null) c.disconnect();
        }
    }

    private static Map<String, String[]> releaseAssets(String api) throws Failed {
        Map<String, String[]> out = new HashMap<>();
        try {
            JSONArray list = new JSONObject(fetchText(api, true)).getJSONArray("assets");
            Map<String, JSONObject> byName = new HashMap<>();
            for (int i = 0; i < list.length(); i++) {
                JSONObject a = list.getJSONObject(i);
                byName.put(a.getString("name"), a);
            }
            JSONObject sums = byName.get("SHA256SUMS.txt");
            if (sums == null) throw new Failed("download", "release without SHA256SUMS.txt");
            for (String line : fetchText(sums.getString("browser_download_url"), false).split("\n")) {
                String[] p = line.trim().split("\\s+");
                if (p.length < 2) continue;
                String name = p[p.length - 1].replaceFirst("^\\*", "");
                JSONObject a = byName.get(name);
                if (a != null)
                    out.put(name, new String[] {a.getString("browser_download_url"), p[0].toLowerCase(java.util.Locale.ROOT),
                                                String.valueOf(a.getLong("size"))});
            }
        } catch (org.json.JSONException e) {
            throw new Failed("network", "release list: " + e);
        }
        return out;
    }

    interface Report { void at(double part); }

    private static String sha256(File f) throws IOException {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            try (InputStream in = new FileInputStream(f)) {
                byte[] buf = new byte[1 << 20];
                for (int r; (r = in.read(buf)) > 0; ) {
                    if (cancel) throw new IOException("stopped");
                    md.update(buf, 0, r);
                }
            }
            StringBuilder s = new StringBuilder();
            for (byte b : md.digest()) s.append(String.format("%02x", b));
            return s.toString();
        } catch (java.security.NoSuchAlgorithmException e) {
            throw new IOException(e);
        }
    }

    /* url -> dst, checked. A finished file from an earlier run is kept; a
     * .part is continued where it stopped. */
    private static File download(String url, File dst, String digest, long size, Report report) throws Failed {
        try {
            if (dst.exists() && sha256(dst).equals(digest)) return dst;
            dst.delete();
            File part = new File(dst.getPath() + ".part");
            for (int attempt = 0; ; attempt++) {
                waitForNetwork();
                long have = part.exists() ? part.length() : 0;
                if (have > size) { part.delete(); have = 0; }
                if (have < size) {
                    try {
                        fetchTo(url, part, have, size, report);
                    } catch (IOException e) {
                        if (!cancel && !networkOk()) { attempt--; continue; }   // left Wi-Fi: wait, then go on
                        if (cancel || attempt >= 4) throw e;
                        try { Thread.sleep(3000L << attempt); } catch (InterruptedException ignored) { }
                        continue;                          // continue from what arrived
                    }
                }
                if (sha256(part).equals(digest)) break;
                part.delete();                             // a bad copy: once more from the start
                if (attempt >= 1) throw new Failed("download", url + ": checksum mismatch");
            }
            if (!part.renameTo(dst)) throw new Failed("disk", "cannot rename " + part);
            return dst;
        } catch (IOException e) {
            throw new Failed(cancel ? "stopped" : "network", url + ": " + e);
        }
    }

    private static void fetchTo(String url, File part, long have, long size, Report report) throws IOException {
        HttpURLConnection c = open(url, have);
        try {
            int code = c.getResponseCode();
            boolean append = code == 206;
            if (!append && code != 200) throw new IOException("HTTP " + code);
            long got = append ? have : 0;
            try (InputStream in = new BufferedInputStream(c.getInputStream(), 1 << 16);
                 RandomAccessFile out = new RandomAccessFile(part, "rw")) {
                out.setLength(got);
                out.seek(got);
                byte[] buf = new byte[1 << 16];
                long check = got;
                for (int r; (r = in.read(buf)) > 0; ) {
                    if (cancel) throw new IOException("stopped");
                    out.write(buf, 0, r);
                    got += r;
                    if (got - check > (8 << 20)) {                 // every 8 MB: still on Wi-Fi?
                        check = got;
                        if (!networkOk()) throw new IOException("network changed");
                    }
                    report.at(got);
                }
            }
            if (got != size) throw new IOException("short download " + got + "/" + size);
        } finally {
            c.disconnect();
        }
    }

    // ------------------------------------------------------------------ tar
    /* A .tar / .tar.xz from the release into dst: regular files only (pax and
     * GNU long-name headers carry long paths), each path checked to stay
     * inside dst. */
    private static void unpack(File archive, File dst, boolean xz, Report report) throws Failed {
        final long size = archive.length();
        String root;
        try {
            root = dst.getCanonicalPath() + File.separator;
        } catch (IOException e) {
            throw new Failed("disk", e.toString());
        }
        try (FileInputStream raw = new FileInputStream(archive);
             InputStream in = xz ? new XZInputStream(new BufferedInputStream(raw, 1 << 16))
                                 : new BufferedInputStream(raw, 1 << 16)) {
            byte[] h = new byte[512], buf = new byte[1 << 16];
            String longName = null;
            int n = 0;
            while (true) {
                if (cancel) throw new IOException("stopped");
                if (!readFully(in, h, 512)) throw new Failed("download", "truncated archive " + archive.getName());
                if (isZero(h)) break;
                char type = (char) h[156];
                long len = octal(h, 124, 12);
                String name = longName != null ? longName : header(h);
                longName = null;
                if (type == 'x' || type == 'L') {           // pax / GNU long name: the next entry's path
                    byte[] b = new byte[(int) len];
                    if (!readFully(in, b, (int) len)) throw new Failed("download", "truncated header");
                    skip(in, pad(len));
                    String s = new String(b, StandardCharsets.UTF_8);
                    if (type == 'L') longName = s.replaceAll("\0+$", "");
                    else for (String rec : s.split("\n")) {
                        int sp = rec.indexOf(' '), eq = rec.indexOf('=');
                        if (sp > 0 && eq > sp && rec.substring(sp + 1, eq).equals("path")) longName = rec.substring(eq + 1);
                    }
                    continue;
                }
                if (type == 'g') { skip(in, len + pad(len)); continue; }
                if (type != '0' && type != '\0') throw new Failed("download", "not a file in archive: " + name);
                File out = new File(dst, name);
                if (!out.getCanonicalPath().startsWith(root)) throw new Failed("download", "bad path in archive: " + name);
                out.getParentFile().mkdirs();
                out.delete();
                try (OutputStream o = new FileOutputStream(out)) {
                    long left = len;
                    while (left > 0) {
                        int r = in.read(buf, 0, (int) Math.min(buf.length, left));
                        if (r <= 0) throw new Failed("download", "truncated archive " + archive.getName());
                        o.write(buf, 0, r);
                        left -= r;
                    }
                }
                skip(in, pad(len));
                if (++n % 200 == 0) report.at(raw.getChannel().position());
            }
        } catch (IOException e) {
            throw new Failed(cancel ? "stopped" : e.getMessage() != null && (e.getMessage().contains("ENOSPC") || e.getMessage().contains("No space")) ? "disk"
                                                                                                             : "download",
                             archive.getName() + ": " + e);
        }
    }

    private static String header(byte[] h) {
        String name = cstr(h, 0, 100);
        if (h[257] == 'u' && h[258] == 's' && h[259] == 't' && h[260] == 'a' && h[261] == 'r') {
            String prefix = cstr(h, 345, 155);
            if (!prefix.isEmpty()) name = prefix + "/" + name;
        }
        return name;
    }

    private static String cstr(byte[] b, int off, int len) {
        int e = off;
        while (e < off + len && b[e] != 0) e++;
        return new String(b, off, e - off, StandardCharsets.UTF_8);
    }

    private static long octal(byte[] b, int off, int len) {
        long v = 0;
        for (int i = off; i < off + len; i++) {
            if (b[i] == 0 || b[i] == ' ') { if (v > 0) break; else continue; }
            v = v * 8 + (b[i] - '0');
        }
        return v;
    }

    private static long pad(long len) { return (512 - len % 512) % 512; }

    private static boolean isZero(byte[] b) {
        for (byte x : b) if (x != 0) return false;
        return true;
    }

    private static boolean readFully(InputStream in, byte[] b, int len) throws IOException {
        int got = 0;
        while (got < len) {
            int r = in.read(b, got, len - got);
            if (r <= 0) return false;
            got += r;
        }
        return true;
    }

    private static void skip(InputStream in, long n) throws IOException {
        byte[] b = new byte[512];
        while (n > 0) {
            int r = in.read(b, 0, (int) Math.min(b.length, n));
            if (r <= 0) throw new IOException("truncated archive");
            n -= r;
        }
    }

    private static void writeText(File f, String s) throws Failed {
        try (OutputStream o = new FileOutputStream(f)) {
            o.write(s.getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            throw new Failed("disk", e.toString());
        }
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }
}
