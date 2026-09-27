package org.syntheticreality.wos;

import org.libsdl.app.SDLActivity;

public final class WosActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }
}
