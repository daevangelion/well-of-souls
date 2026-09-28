package org.syntheticreality.wos;

import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Bundle;
import android.text.InputType;
import android.util.Log;
import android.widget.EditText;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * First-run data supply.
 *
 * The game data is copyrighted and is NOT bundled in the APK. On first launch (no
 * Souls.exe in the internal "data" dir and no installer-supplied.bin) the app asks
 * the user to supply the original installer, either by picking a file (Storage Access
 * Framework) or by downloading a URL. Either way the installer bytes are written to
 * installer-supplied.bin in internal storage; the Activity is then recreated, and the
 * native boot (wos_android_paths) decodes that file in place with the bundled CIC
 * decoder and starts the game.
 *
 * SDL is only initialised (super.onCreate) once the data is present or a supplied
 * installer is waiting to be decoded, so the game never starts against missing data.
 */
public final class WosActivity extends SDLActivity {
    private static final String TAG = "WosActivity";
    private static final int REQUEST_PICK_INSTALLER = 0x5701;
    private static final String SUPPLIED = "installer-supplied.bin";

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    private File suppliedFile() {
        return new File(getFilesDir(), SUPPLIED);
    }

    private boolean readyToBoot() {
        return new File(getFilesDir(), "data/Souls.exe").isFile() || suppliedFile().isFile();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Only bring up SDL (and the native boot) once there is data to boot against.
        if (readyToBoot()) {
            super.onCreate(savedInstanceState);
            return;
        }
        // No data yet: ask the user to supply the installer instead of booting.
        showSupplyOptions();
    }

    private void showSupplyOptions() {
        String[] options = { "Choose installer file…", "Download from URL…", "Cancel" };
        new AlertDialog.Builder(this)
                .setTitle("Well of Souls — supply the installer")
                .setMessage("The game data is not bundled. Supply the original "
                        + "WellOfSouls.exe installer to install the game.")
                .setItems(options, (d, which) -> {
                    if (which == 0) launchPicker();
                    else if (which == 1) promptForUrl();
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
            startActivityForResult(pick, REQUEST_PICK_INSTALLER);
        } catch (Exception e) {
            Log.w(TAG, "no document picker", e);
            toast("No file picker available; download the installer and retry.");
        }
    }

    private void promptForUrl() {
        final EditText field = new EditText(this);
        field.setInputType(InputType.TYPE_TEXT_VARIATION_URI);
        field.setHint("https://…/WellOfSouls.exe");
        new AlertDialog.Builder(this)
                .setTitle("Installer URL")
                .setView(field)
                .setPositiveButton("Download", (d, which) -> downloadInBackground(field.getText().toString().trim()))
                .setNegativeButton("Cancel", null)
                .show();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_PICK_INSTALLER || !readyToBoot()) return;
        if (resultCode == RESULT_OK && data != null && data.getData() != null) {
            copyUriInBackground(data.getData());
        }
    }

    private void copyUriInBackground(Uri uri) {
        new Thread(() -> {
            boolean ok = writeSupplied(() -> getContentResolver().openInputStream(uri));
            onSupplied(ok, "Installer saved; decoding…");
        }).start();
    }

    private void downloadInBackground(String url) {
        if (url.isEmpty()) { toast("Enter a URL."); return; }
        new Thread(() -> {
            boolean ok = writeSupplied(() -> new URL(url).openStream());
            onSupplied(ok, "Downloaded; decoding…");
        }).start();
    }

    private interface StreamOp { InputStream open() throws Exception; }

    /** Streams src into installer-supplied.bin via a temporary file + atomic rename. */
    private boolean writeSupplied(StreamOp src) {
        File tmp = new File(getFilesDir(), SUPPLIED + ".part");
        try (InputStream in = src.open(); OutputStream out = new FileOutputStream(tmp)) {
            if (in == null) return false;
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            out.flush();
        } catch (Exception e) {
            Log.e(TAG, "supply failed", e);
            tmp.delete();
            return false;
        }
        if (!tmp.renameTo(suppliedFile())) { tmp.delete(); return false; }
        return true;
    }

    private void onSupplied(boolean ok, String successMsg) {
        runOnUiThread(() -> {
            if (ok) {
                toast(successMsg);
                recreate();           // re-enter onCreate; now boots and decodes
            } else {
                toast("Could not read the installer.");
                showSupplyOptions();
            }
        });
    }

    private void toast(String msg) {
        Toast.makeText(this, msg, Toast.LENGTH_LONG).show();
    }
}
