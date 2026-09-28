package org.syntheticreality.wos;

import android.content.Intent;
import android.os.Bundle;

import org.libsdl.app.SDLActivity;

import java.io.File;

/**
 * The game host.
 *
 * SDL is always brought up (super.onCreate) so the native boot (wos_android_paths) can
 * provision data: if data is already installed, or the user supplies an installer via
 * the "WoS Install" form (SupplyActivity), the game decodes and plays. If there is no
 * data on a fresh launch, this hands off to the SupplyActivity form; the native thread
 * waits (up to 5 min) for installer-supplied.bin while the user supplies it, then boots.
 *
 * Skipping super.onCreate() (to show UI first) leaves the Activity without a window and
 * crashes, so SDL is always started unconditionally.
 */
public final class WosActivity extends SDLActivity {
    private static final String SUPPLIED = "installer-supplied.bin";

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    private boolean dataReady() {
        return new File(getFilesDir(), "data/Souls.exe").isFile()
                || new File(getFilesDir(), SUPPLIED).isFile();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);   // always start SDL
        if (!dataReady()) {
            // Fresh install: open the supply form. The native thread waits in the
            // background; supplying the installer makes it decode and boot the game.
            startActivity(new Intent(this, SupplyActivity.class));
        }
    }
}
