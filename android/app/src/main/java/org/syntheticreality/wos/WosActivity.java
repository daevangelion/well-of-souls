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

    private void offerSupplyForm() {
        if (!dataReady()) startActivity(new Intent(this, SupplyActivity.class));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);   // always start SDL
        offerSupplyForm();                    // fresh install: supply form; native waits behind it
    }

    // WosActivity is singleTask, so relaunching the icon while data is absent delivers
    // onNewIntent (not onCreate) -> re-offer the form instead of showing an empty game.
    @Override
    protected void onNewIntent(android.content.Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        offerSupplyForm();
    }

    // While the native thread is blocked waiting for the installer, tearing SDL down
    // (Back -> nativeSendQuit + mSDLThread.join) ANRs for the rest of the wait. Ignore
    // Back until data is ready.
    @Override
    public void onBackPressed() {
        if (dataReady()) super.onBackPressed();
    }
}
