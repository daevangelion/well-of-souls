package org.syntheticreality.wos;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.text.InputType;
import android.util.Log;
import android.widget.EditText;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.URL;

/**
 * Optional soundfont supply.
 *
 * MIDI playback is optional: without a bank the game runs silently. This small screen
 * lets the user point the engine at any compatible SoundFont2 bank, picked from device
 * storage or downloaded from a URL, without rebuilding the app. The bytes are written
 * to user-soundfont.sf2 in the same internal files dir the app uses for game data;
 * find_soundfont() (audio_sdl2.c) checks that path first, so a supplied bank overrides
 * the bundled/default TimGM6mb.
 */
public final class SoundfontActivity extends Activity {
    private static final String TAG = "SoundfontActivity";
    private static final int REQUEST_PICK = 0x5702;
    private static final String USER_BANK = "user-soundfont.sf2";

    private File target() {
        return new File(getFilesDir(), USER_BANK);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        String[] options = { "Choose soundfont file…", "Download from URL…", "Cancel" };
        new AlertDialog.Builder(this)
                .setTitle("Well of Souls — soundfont")
                .setMessage(target().isFile()
                        ? "A custom soundfont is installed. Supply a new one, or cancel to keep it."
                        : "MIDI playback needs a SoundFont2 bank. Supply one, or cancel to run silently.")
                .setItems(options, (d, which) -> {
                    if (which == 0) launchPicker();
                    else if (which == 1) promptForUrl();
                    else finish();
                })
                .setOnCancelListener(d -> finish())
                .show();
    }

    private void launchPicker() {
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        pick.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try {
            startActivityForResult(pick, REQUEST_PICK);
        } catch (Exception e) {
            Log.w(TAG, "no document picker", e);
            toast("No file picker available.");
            finish();
        }
    }

    private void promptForUrl() {
        final EditText field = new EditText(this);
        field.setInputType(InputType.TYPE_TEXT_VARIATION_URI);
        field.setHint("https://…/font.sf2");
        new AlertDialog.Builder(this)
                .setTitle("SoundFont URL")
                .setView(field)
                .setPositiveButton("Download", (d, which) ->
                        download(field.getText().toString().trim()))
                .setNegativeButton("Cancel", (d, which) -> finish())
                .show();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_PICK) return;
        if (resultCode == RESULT_OK && data != null && data.getData() != null) {
            final Uri uri = data.getData();
            new Thread(() -> finish_with(write(() -> getContentResolver().openInputStream(uri)))).start();
        } else {
            finish();
        }
    }

    private void download(String url) {
        if (url.isEmpty()) { toast("Enter a URL."); finish(); return; }
        new Thread(() -> finish_with(write(() -> new URL(url).openStream()))).start();
    }

    private interface StreamOp { InputStream open() throws Exception; }

    private boolean write(StreamOp src) {
        File tmp = new File(getFilesDir(), USER_BANK + ".part");
        try (InputStream in = src.open(); OutputStream out = new FileOutputStream(tmp)) {
            if (in == null) return false;
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            out.flush();
        } catch (Exception e) {
            Log.e(TAG, "soundfont write failed", e);
            tmp.delete();
            return false;
        }
        if (!tmp.renameTo(target())) { tmp.delete(); return false; }
        return true;
    }

    private void finish_with(boolean ok) {
        runOnUiThread(() -> {
            toast(ok ? "Soundfont saved; relaunch the game to use it." : "Could not read the soundfont.");
            finish();
        });
    }

    private void toast(String msg) {
        Toast.makeText(this, msg, Toast.LENGTH_LONG).show();
    }
}
