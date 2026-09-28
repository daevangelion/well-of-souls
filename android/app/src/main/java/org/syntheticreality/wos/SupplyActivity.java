package org.syntheticreality.wos;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.URL;

/**
 * Game-data supply — a real form screen (not popups).
 *
 * Lets the user supply the original WellOfSouls.exe installer at any time: pick it via
 * the Storage Access Framework, or paste a URL and download it. The installer is stored
 * as installer-supplied.bin in internal storage; the game (WosActivity) decodes it
 * on-device with the bundled CIC decoder and boots. If the game is already running and
 * waiting, "Continue to game" brings it forward so it picks the installer up.
 */
public final class SupplyActivity extends Activity {
    private static final String TAG = "SupplyActivity";
    private static final int REQUEST_PICK = 0x5703;
    private static final String SUPPLIED = "installer-supplied.bin";

    private TextView status;
    private ProgressBar progress;

    private File target() {
        return new File(getFilesDir(), SUPPLIED);
    }

    private boolean dataReady() {
        return new File(getFilesDir(), "data/Souls.exe").isFile() || target().isFile();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_supply);

        status = findViewById(R.id.status);
        progress = findViewById(R.id.progress);
        Button pick = findViewById(R.id.pickButton);
        Button download = findViewById(R.id.downloadButton);
        gameButton = findViewById(R.id.gameButton);
        EditText url = findViewById(R.id.urlField);

        pick.setOnClickListener(v -> launchPicker());
        download.setOnClickListener(v -> {
            String u = url.getText().toString().trim();
            if (u.isEmpty()) { setStatus("Enter a URL first.", false); return; }
            downloadInBackground(u);
        });
        gameButton.setOnClickListener(v -> openGame());
        refresh();
    }

    private Button gameButton;

    private void refresh() {
        boolean ready = dataReady();
        if (gameButton != null) gameButton.setEnabled(ready);
        if (ready) {
            setStatus("Installer ready. Tap Continue to game.", false);
        } else {
            setStatus("Waiting for the installer…", true);
        }
    }

    private void setStatus(String msg, boolean busy) {
        status.setText(msg);
        progress.setVisibility(busy ? View.VISIBLE : View.GONE);
    }

    private void openGame() {
        // Guard: launching the game with no data just triggers the 5-minute wait and
        // the failure box. Only proceed once the installer is present.
        if (!dataReady()) { setStatus("Supply the installer first.", false); refresh(); return; }
        startActivity(new Intent(this, WosActivity.class));
        finish();
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
            setStatus("No file picker available; use the URL field instead.", false);
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_PICK) return;
        if (resultCode == RESULT_OK && data != null && data.getData() != null) {
            final Uri uri = data.getData();
            new Thread(() -> done(write(() -> getContentResolver().openInputStream(uri)))).start();
        } else {
            setStatus("No file selected.", false);
        }
    }

    private void downloadInBackground(String url) {
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
                setStatus("Installer saved. Decoding…", true);
                Toast.makeText(this, "Installer saved.", Toast.LENGTH_LONG).show();
                openGame();
            } else {
                setStatus("Could not read the installer. Try again.", false);
            }
        });
    }
}
