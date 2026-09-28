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
 * Setup form (single launcher entry "WoS Setup").
 *
 * Handles both runtime inputs in one screen:
 *   - Game data: pick the original WellOfSouls.exe installer (SAF) or download it from a
 *     URL, stored as installer-supplied.bin; the game decodes it on-device with the
 *     bundled CIC decoder and boots.
 *   - Soundfont (optional): pick or download a SoundFont2 bank, stored as
 *     user-soundfont.sf2; find_soundfont() prefers it over the built-in bank.
 *
 * The soundfont URL is pre-filled with the official upstream TimGM6mb bank. The game
 * installer is a commercial title with no official free-download URL, so that field is
 * left empty for the user to paste a source they have the right to use.
 */
public final class SupplyActivity extends Activity {
    private static final String TAG = "SupplyActivity";
    private static final int REQUEST_PICK_INSTALLER = 0x5703;
    private static final int REQUEST_PICK_SOUNDFONT = 0x5704;
    private static final String SUPPLIED = "installer-supplied.bin";
    private static final String USER_BANK = "user-soundfont.sf2";
    private static final String TIMGM6MB_URL =
        "https://raw.githubusercontent.com/arbruijn/TimGM6mb/"
        + "d6ad4ed72dce1fd3d67f17b74e08cd7ae7941a96/TimGM6mb.sf2";

    private TextView dataStatus, soundStatus;
    private ProgressBar dataProgress;
    private Button gameButton;

    private File installerFile() { return new File(getFilesDir(), SUPPLIED); }
    private File soundfontFile() { return new File(getFilesDir(), USER_BANK); }
    private boolean dataReady() {
        return new File(getFilesDir(), "data/Souls.exe").isFile() || installerFile().isFile();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_supply);

        dataStatus = findViewById(R.id.dataStatus);
        soundStatus = findViewById(R.id.soundStatus);
        dataProgress = findViewById(R.id.dataProgress);
        gameButton = findViewById(R.id.gameButton);
        final EditText installerUrl = findViewById(R.id.installerUrl);
        final EditText soundfontUrl = findViewById(R.id.soundfontUrl);
        soundfontUrl.setText(TIMGM6MB_URL);   // official upstream bank, pre-filled

        findViewById(R.id.pickInstallerButton).setOnClickListener(v -> pick(REQUEST_PICK_INSTALLER));
        findViewById(R.id.downloadInstallerButton).setOnClickListener(v -> {
            String u = installerUrl.getText().toString().trim();
            if (u.isEmpty()) { setDataStatus("Paste an installer URL first.", false); return; }
            download(u, true);
        });
        findViewById(R.id.pickSoundfontButton).setOnClickListener(v -> pick(REQUEST_PICK_SOUNDFONT));
        findViewById(R.id.downloadSoundfontButton).setOnClickListener(v -> {
            String u = soundfontUrl.getText().toString().trim();
            if (u.isEmpty()) { setSoundStatus("Paste a soundfont URL first."); return; }
            download(u, false);
        });
        gameButton.setOnClickListener(v -> openGame());
        refresh();
    }

    private void refresh() {
        boolean ready = dataReady();
        gameButton.setEnabled(ready);
        if (ready) setDataStatus("Installer ready. Tap Continue to game.", false);
        else setDataStatus("Waiting for the installer…", true);
        setSoundStatus(soundfontFile().isFile()
                ? "Custom soundfont installed." : "Using the built-in soundfont.");
    }

    private void setDataStatus(String msg, boolean busy) {
        dataStatus.setText(msg);
        dataProgress.setVisibility(busy ? View.VISIBLE : View.GONE);
    }
    private void setSoundStatus(String msg) { soundStatus.setText(msg); }

    private void openGame() {
        if (!dataReady()) { setDataStatus("Supply the installer first.", false); refresh(); return; }
        startActivity(new Intent(this, WosActivity.class));
        finish();
    }

    private void pick(int request) {
        Intent p = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        p.addCategory(Intent.CATEGORY_OPENABLE);
        p.setType("*/*");
        p.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try { startActivityForResult(p, request); }
        catch (Exception e) {
            Log.w(TAG, "no document picker", e);
            if (request == REQUEST_PICK_INSTALLER) setDataStatus("No file picker; use the URL field.", false);
            else setSoundStatus("No file picker; use the URL field.");
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        final boolean isInstaller = requestCode == REQUEST_PICK_INSTALLER;
        new Thread(() -> {
            boolean ok = write(isInstaller ? installerFile() : soundfontFile(),
                               () -> getContentResolver().openInputStream(uri));
            runOnUiThread(() -> finishWrite(ok, isInstaller));
        }).start();
    }

    private void download(String url, boolean isInstaller) {
        new Thread(() -> {
            boolean ok = write(isInstaller ? installerFile() : soundfontFile(), () -> new URL(url).openStream());
            runOnUiThread(() -> finishWrite(ok, isInstaller));
        }).start();
    }

    private interface StreamOp { InputStream open() throws Exception; }

    /** Streams src into dest atomically (temp file + rename). */
    private boolean write(File dest, StreamOp src) {
        File tmp = new File(dest.getParentFile(), dest.getName() + ".part");
        try (InputStream in = src.open(); OutputStream out = new FileOutputStream(tmp)) {
            if (in == null) return false;
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            out.flush();
        } catch (Exception e) {
            Log.e(TAG, "write failed", e);
            tmp.delete();
            return false;
        }
        if (!tmp.renameTo(dest)) { tmp.delete(); return false; }
        return true;
    }

    private void finishWrite(boolean ok, boolean isInstaller) {
        if (ok) {
            if (isInstaller) setDataStatus("Installer saved. Decoding…", true);
            else setSoundStatus("Soundfont saved.");
            Toast.makeText(this, isInstaller ? "Installer saved." : "Soundfont saved.", Toast.LENGTH_LONG).show();
        } else {
            if (isInstaller) setDataStatus("Could not read the file. Try again.", false);
            else setSoundStatus("Could not read the soundfont.");
        }
        refresh();
        if (isInstaller && ok) openGame();
    }
}
