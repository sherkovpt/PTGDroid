package pt.ptgdroid;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.io.InputStream;

/**
 * Starts the game when its files are present; otherwise asks for the ZIP with the user's own copy
 * of Pathway to Glory (N-Gage) and imports it.
 */
public class LauncherActivity extends Activity {
    private static final int PICK_ZIP = 1;
    private TextView status;
    private Button pick;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        File app = GameFiles.findApp(this);
        if (app != null) {
            startGame(app);
            return;
        }
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        layout.setPadding(pad, pad, pad, pad);
        layout.setBackgroundColor(Color.BLACK);

        TextView info = new TextView(this);
        info.setTextColor(Color.WHITE);
        info.setTextSize(16);
        info.setText("PTGDroid\n\nTo play, select the ZIP file with your own copy of "
                + "Pathway to Glory for the N-Gage "
                + "(for example Nokia_N-Gage_Pathway_To_Glory_v1.03_Files.zip).\n\n"
                + "The game files are copied into the app's private storage. "
                + "This is only needed once.");
        layout.addView(info);

        pick = new Button(this);
        pick.setText("Select game ZIP");
        pick.setOnClickListener(v -> chooseZip());
        layout.addView(pick);

        status = new TextView(this);
        status.setTextColor(Color.LTGRAY);
        status.setPadding(0, pad, 0, 0);
        layout.addView(status);

        setContentView(layout);
    }

    private void chooseZip() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {
                "application/zip", "application/x-zip-compressed", "application/octet-stream" });
        startActivityForResult(i, PICK_ZIP);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_ZIP || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        pick.setEnabled(false);
        status.setText("Importing...");
        new Thread(() -> {
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                final long[] lastShown = { 0 };
                GameFiles.importZip(this, in, total -> {
                    if (total - lastShown[0] >= (4 << 20)) {
                        lastShown[0] = total;
                        runOnUiThread(() -> status.setText("Importing... " + (total >> 20) + " MB"));
                    }
                });
                File app = GameFiles.findApp(this);
                runOnUiThread(() -> startGame(app));
            } catch (Exception e) {
                runOnUiThread(() -> {
                    status.setText("Error: " + e.getMessage());
                    pick.setEnabled(true);
                });
            }
        }).start();
    }

    private void startGame(File app) {
        Intent i = new Intent(this, GameActivity.class);
        i.putExtra(GameActivity.EXTRA_APP_PATH, app.getPath());
        startActivity(i);
        finish();
    }
}
