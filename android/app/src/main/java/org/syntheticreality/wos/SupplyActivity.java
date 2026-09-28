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
 * Game-data supply (launcher entry "WoS Install").
 *
 * Lets the user supply the original WellOfSouls.exe installer at any time — pick it via
 * the Storage Access Framework, or download it from a URL — and stores it as
 * installer-supplied.bin in internal storage. On the next launch of the game, the native
 * boot decodes it in place with the bundled CIC decoder and plays. After supplying, the
 * game is launched automatically.
 */
public final class SupplyActivity extends Activity {
    private static final String TAG = "SupplyActivity";
    private static final int REQUEST_PICK = 0x5703;
    private static final String SUPPLIED = "installer-supplied.bin";

    private File target() {
        return new File(getFilesDir(), SUPPLIED);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        String[] options = { "Choose installer file…", "Download from URL…", "Cancel" };
        new AlertDialog.Builder(this, android.R.style.Theme_Material_Light_Dialog_Alert)
                .setTitle("Well of Souls — supply the installer")
                .setMessage("Supply the original WellOfSouls.exe installer to install the game. "
                        + "It is unpacked on-device; the game data is never bundled in the app.")
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
                        download(field.getText().toString().trim()))
                .setNegativeButton("Cancel", null)
                .show();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_PICK) return;
        if (resultCode == RESULT_OK && data != null && data.getData() != null) {
            final Uri uri = data.getData();
            new Thread(() -> done(write(() -> getContentResolver().openInputStream(uri)))).start();
        }
    }

    private void download(String url) {
        if (url.isEmpty()) { toast("Enter a URL."); return; }
        new Thread(() -> done(write(() -> new URL(url).openStream()))).start();
    }

    private interface StreamOp { InputStream open() throws Exception; }

    private boolean write(StreamOp src) {
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
        if (!tmp.renameTo(target())) { tmp.delete(); return false; }
        return true;
    }

    private void done(boolean ok) {
        runOnUiThread(() -> {
            if (ok) {
                startActivity(new Intent(this, WosActivity.class));
                finish();
            } else {
                toast("Could not read the installer. Try again.");
            }
        });
    }

    private void toast(String msg) {
        Toast.makeText(this, msg, Toast.LENGTH_LONG).show();
    }
}
