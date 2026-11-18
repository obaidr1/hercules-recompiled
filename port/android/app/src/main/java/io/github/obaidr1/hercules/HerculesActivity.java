package io.github.obaidr1.hercules;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.view.HapticFeedbackConstants;
import android.view.View;

import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

import org.libsdl.app.SDLActivity;

/* SDL3 is linked statically into libmain.so (the game runtime), so only that
 * one library is loaded. */
public class HerculesActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    /* Called from native code (touch_pad.h psx_android_haptic): 0 = a press,
     * 1 = an action confirmed. performHapticFeedback follows the system's
     * touch-feedback setting and needs no permission. */
    public static void haptic(final int kind) {
        final View v = getContentView();
        if (v == null) return;
        final int c = kind == 1 && Build.VERSION.SDK_INT >= 30 ? HapticFeedbackConstants.CONFIRM
                                                               : HapticFeedbackConstants.VIRTUAL_KEY;
        v.post(() -> v.performHapticFeedback(c));
    }

    /* Called from native code (the launcher's event-log upload, on its own
     * thread): POST a JSON body, return the HTTP status, 0 without a
     * connection. */
    public static int postJson(String url, String body) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setRequestMethod("POST");
            c.setConnectTimeout(15000);
            c.setReadTimeout(20000);
            c.setDoOutput(true);
            c.setRequestProperty("Content-Type", "application/json");
            byte[] data = body.getBytes(StandardCharsets.UTF_8);
            c.setFixedLengthStreamingMode(data.length);
            try (OutputStream o = c.getOutputStream()) { o.write(data); }
            int code = c.getResponseCode();
            try (InputStream in = code < 400 ? c.getInputStream() : c.getErrorStream()) {
                if (in != null) while (in.read() >= 0) { }
            }
            return code;
        } catch (Exception e) {
            return 0;
        } finally {
            if (c != null) c.disconnect();
        }
    }

    /* Called from native code (the launcher's HD option): the ready HD pack,
     * see HdDownload. */
    public static boolean hdStart(String api, String serial, String pack, String data) {
        return HdDownload.start(api, serial, pack, data);
    }

    public static boolean hdRunning() {
        return HdDownload.running();
    }

    public static boolean hdStop() {
        HdDownload.stop();
        return true;
    }

    /* Called from native code ("back to the launcher" in the in-game menu):
     * the launcher and the game share this process, so start the app anew --
     * it opens on the launcher -- and end this process. */
    public static void restartToLauncher() {
        Context ctx = getContext();
        if (ctx == null) return;
        Intent restart = Intent.makeRestartActivityTask(new ComponentName(ctx, HerculesActivity.class));
        ctx.startActivity(restart);
        Runtime.getRuntime().exit(0);
    }
}
