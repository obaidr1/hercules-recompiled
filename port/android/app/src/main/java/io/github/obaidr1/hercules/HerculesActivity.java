package io.github.obaidr1.hercules;

import org.libsdl.app.SDLActivity;

/* SDL3 is linked statically into libmain.so (the game runtime), so only that
 * one library is loaded. */
public class HerculesActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }
}
