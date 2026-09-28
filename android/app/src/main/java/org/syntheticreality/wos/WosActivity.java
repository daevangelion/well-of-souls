package org.syntheticreality.wos;

import android.app.AlertDialog;
import android.app.Dialog;
import android.content.Intent;
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
import java.net.URL;

/**
 * First-run data supply.
 *
 * The game data is copyrighted and is NOT bundled in the APK. On a fresh launch (no
 * Souls.exe in the internal "data" dir and no installer-supplied.bin) the native boot
 * (wos_android_paths) blocks waiting for the user to supply the original installer.
 * This Activity shows the supply UI (pick a file via the Storage Access Framework, or
 * download a URL), writes the installer bytes atomically to installer-supplied.bin, and
 * the waiting native thread decodes it in place with the bundled CIC decoder and starts
 * the game.
 *
 * SDL is always brought up (super.onCreate) unconditionally — skipping it leaves the
 * Activity without a window and crashes. The supply dialog is shown *after* SDL starts,
 * overlaying the game view while the native thread waits.
 */
public final class WosActivity extends SDLActivity {
    private static final String TAG = "WosActivity";
    private static final int REQUEST_PICK_INSTALLER = 0x5701;
    private static final String SUPPLIED = "installer-supplied.bin";

    private Dialog supplyDialog;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    private File suppliedFile() {
        return new File(getFilesDir(), SUPPLIED);
    }

    private boolean dataReady() {
        return new File(getFilesDir(), "data/Souls.exe").isFile() || suppliedFile().isFile();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Always start SDL; the native thread handles (and waits for) missing data.
        super.onCreate(savedInstanceState);
        if (!dataReady()) {
            // Let SDL start first, then overlay the supply prompt.
            getWindow().getDecorView().post(this::showSupplyOptions);
        }
    }

    private void showSupplyOptions() {
        if (isFinishing() || supplyDialog != null) return;
        String[] options = { "Choose installer file…", "Download from URL…", "Cancel" };
        // Explicit dialog theme: the Activity uses a fullscreen no-titlebar theme, and
        // inheriting it makes the dialog message and option buttons render invisible.
        AlertDialog dlg = new AlertDialog.Builder(this, android.R.style.Theme_Material_Light_Dialog_Alert)
                .setTitle("Well of Souls — supply the installer")
                .setMessage("The game data is not bundled. Supply the original "
                        + "WellOfSouls.exe installer to install the game.")
                .setItems(options, (d, which) -> {
                    if (which == 0) launchPicker();
                    else if (which == 1) promptForUrl();
                })
                .create();
        // On cancel, open the dedicated "WoS Install" supply screen (do NOT finish(),
        // which would tear down SDL while the native thread is still waiting). The game
        // keeps waiting in the background; supplying the installer there boots it.
        dlg.setOnCancelListener(d -> {
            try { startActivity(new Intent(this, SupplyActivity.class)); }
            catch (Exception e) { toast("Waiting for the installer…"); }
        });
        supplyDialog = dlg;
        dlg.show();
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
        new AlertDialog.Builder(this, android.R.style.Theme_Material_Light_Dialog_Alert)
                .setTitle("Installer URL")
                .setView(field)
                .setPositiveButton("Download", (d, which) ->
                        downloadInBackground(field.getText().toString().trim()))
                .setNegativeButton("Cancel", null)
                .show();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_PICK_INSTALLER) return;
        if (resultCode == RESULT_OK && data != null && data.getData() != null) {
            final Uri uri = data.getData();
            new Thread(() -> onSupplied(writeSupplied(() -> getContentResolver().openInputStream(uri))))
                    .start();
        }
    }

    private void downloadInBackground(String url) {
        if (url.isEmpty()) { toast("Enter a URL."); showSupplyOptions(); return; }
        new Thread(() -> onSupplied(writeSupplied(() -> new URL(url).openStream()))).start();
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

    private void onSupplied(boolean ok) {
        runOnUiThread(() -> {
            if (ok) {
                // The waiting native thread picks up installer-supplied.bin, decodes it,
                // and boots the game; drop the prompt so the game is visible.
                if (supplyDialog != null) { supplyDialog.dismiss(); supplyDialog = null; }
                toast("Installer saved; decoding…");
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
