package io.github.obaidr1.hercules;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;

import org.libsdl.app.SDLActivity;

/* SDL3 is linked statically into libmain.so (the game runtime), so only that
 * one library is loaded. */
public class HerculesActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
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
