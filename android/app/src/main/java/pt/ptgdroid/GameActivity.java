package pt.ptgdroid;

import org.libsdl.app.SDLActivity;

/** Runs the recompiled game (libmain.so) on top of SDL. */
public class GameActivity extends SDLActivity {
    static final String EXTRA_APP_PATH = "app_path";

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected String[] getArguments() {
        String app = getIntent().getStringExtra(EXTRA_APP_PATH);
        return new String[] { app, GameFiles.cDrive(this).getPath() };
    }
}
