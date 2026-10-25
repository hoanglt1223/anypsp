// SPDX-License-Identifier: GPL-2.0-or-later
package io.anyport.anypsp;

import android.app.Activity;
import android.content.Intent;
import android.content.res.AssetManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.util.Log;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.util.Properties;

/**
 * Puts the game files where the native host (libmain.so) reads them, then starts GameActivity:
 * files/anypsp/ (game.properties, nids.toml) and files/game/ (an embedded EBOOT or disc) from
 * the APK's assets, and, for a disc the APK does not carry, the player's own copy picked through
 * the storage access framework: an ISO image to files/game.iso (the host extracts it on first
 * start) or an extracted disc folder to files/game-disc/.
 */
public class SetupActivity extends Activity {
    static final String TAG = "anypsp";
    private static final int REQUEST_ISO = 1;
    private static final int REQUEST_FOLDER = 2;

    private Properties game = new Properties();
    private TextView status;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        layout.setPadding(pad, pad, pad, pad);
        status = new TextView(this);
        status.setTextSize(18);
        status.setGravity(Gravity.CENTER);
        layout.addView(status);
        setContentView(layout);
        status.setText("Preparing…");
        new Thread(() -> {
            try {
                installAssets();
                runOnUiThread(this::next);
            } catch (IOException e) {
                Log.e(TAG, "installing the game files failed", e);
                runOnUiThread(() -> status.setText("Installing the game files failed: " + e.getMessage()));
            }
        }).start();
    }

    private boolean needsDisc() { return "1".equals(game.getProperty("needs_disc", "0")); }

    private boolean hasDisc() {
        File files = getFilesDir();
        return new File(files, "game.iso").isFile() || new File(files, "game-disc/PSP_GAME").isDirectory();
    }

    private void next() {
        if (needsDisc() && !hasDisc()) {
            showPicker(null);
        } else {
            startGame();
        }
    }

    private void startGame() {
        Intent intent = new Intent(this, GameActivity.class);
        String[] args = getIntent().getStringArrayExtra(GameActivity.EXTRA_ARGS);
        if (args != null) intent.putExtra(GameActivity.EXTRA_ARGS, args);
        startActivity(intent);
        finish();
    }

    private void showPicker(String problem) {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        layout.setPadding(pad, pad, pad, pad);
        TextView text = new TextView(this);
        text.setTextSize(18);
        text.setGravity(Gravity.CENTER);
        String title = game.getProperty("title", "the game");
        String id = game.getProperty("disc_id", "");
        text.setText((problem != null ? problem + "\n\n" : "") + "Choose your copy of " + title + (id.isEmpty() ? "" : " (" + id + ")")
                + ": an ISO image, or the extracted disc folder (the one holding PSP_GAME). It is copied into the app once; "
                + "its executable must be the decrypted one this app was built from.");
        layout.addView(text);
        Button iso = new Button(this);
        iso.setText("ISO image");
        iso.setOnClickListener(v -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("*/*");
            startActivityForResult(pick, REQUEST_ISO);
        });
        layout.addView(iso);
        Button folder = new Button(this);
        folder.setText("Disc folder");
        folder.setOnClickListener(v -> startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQUEST_FOLDER));
        layout.addView(folder);
        setContentView(layout);
        status = text;
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        status.setText("Copying the game files…");
        new Thread(() -> {
            try {
                File files = getFilesDir();
                if (request == REQUEST_ISO) {
                    File part = new File(files, "game.iso.part");
                    try (InputStream in = getContentResolver().openInputStream(uri)) {
                        if (in == null) throw new IOException("cannot open " + uri);
                        copy(in, part, true);
                    }
                    if (!part.renameTo(new File(files, "game.iso"))) throw new IOException("cannot rename " + part);
                } else {
                    File part = new File(files, "game-disc.part");
                    deleteTree(part);
                    copyTree(uri, DocumentsContract.getTreeDocumentId(uri), part, 0);
                    File disc = new File(part, "PSP_GAME");
                    if (!disc.isDirectory()) {
                        // The PSP_GAME folder itself was picked.
                        File root = new File(files, "game-disc.root");
                        deleteTree(root);
                        if (!root.mkdirs() || !part.renameTo(new File(root, "PSP_GAME"))) throw new IOException("cannot arrange " + part);
                        part = root;
                        if (!new File(part, "PSP_GAME/SYSDIR").isDirectory()) {
                            deleteTree(part);
                            throw new IOException("that folder holds no PSP_GAME/SYSDIR");
                        }
                    }
                    File target = new File(files, "game-disc");
                    deleteTree(target);
                    if (!part.renameTo(target)) throw new IOException("cannot rename " + part);
                }
                runOnUiThread(this::startGame);
            } catch (IOException | RuntimeException e) {
                Log.e(TAG, "copying the game failed", e);
                runOnUiThread(() -> showPicker("Copying failed: " + e.getMessage()));
            }
        }).start();
    }

    // Recreates the APK's assets in files/ when this build of the app has not done so yet.
    private void installAssets() throws IOException {
        AssetManager assets = getAssets();
        try (InputStream in = assets.open("anypsp/game.properties")) {
            game.load(in);
        }
        File files = getFilesDir();
        String version = game.getProperty("version", "0");
        File marker = new File(files, "anypsp/installed-version");
        if (marker.isFile() && version.equals(readText(marker).trim())) return;
        for (String name : new String[] {"game.properties", "nids.toml"}) {
            try (InputStream in = assets.open("anypsp/" + name)) {
                copy(in, new File(files, "anypsp/" + name), false);
            }
        }
        File gameDir = new File(files, "game");
        deleteTree(gameDir);
        String listing;
        try (InputStream in = assets.open("anypsp/files.txt")) {
            listing = readStream(in);
        }
        for (String name : listing.split("\n")) {
            if (name.isEmpty()) continue;
            File target = new File(gameDir, name);
            if (name.startsWith("disc/")) target = new File(files, "game-disc.part/" + name.substring(5));
            try (InputStream in = assets.open("game/" + name)) {
                copy(in, target, false);
            }
        }
        File embeddedDisc = new File(files, "game-disc.part");
        if (embeddedDisc.isDirectory()) {
            File target = new File(files, "game-disc");
            deleteTree(target);
            if (!embeddedDisc.renameTo(target)) throw new IOException("cannot rename " + embeddedDisc);
        }
        try (OutputStream out = new FileOutputStream(marker)) {
            out.write(version.getBytes("UTF-8"));
        }
    }

    private void copyTree(Uri tree, String documentId, File target, int depth) throws IOException {
        if (depth > 32) throw new IOException("folder nesting too deep");
        if (!target.isDirectory() && !target.mkdirs()) throw new IOException("cannot create " + target);
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, documentId);
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                            DocumentsContract.Document.COLUMN_MIME_TYPE};
        try (Cursor c = getContentResolver().query(children, columns, null, null, null)) {
            if (c == null) throw new IOException("cannot list " + documentId);
            while (c.moveToNext()) {
                String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                if (name == null || name.contains("/") || name.equals("..") || name.equals(".")) continue;
                File out = new File(target, name);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    copyTree(tree, id, out, depth + 1);
                } else {
                    try (InputStream in = getContentResolver().openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, id))) {
                        if (in == null) throw new IOException("cannot open " + name);
                        copy(in, out, false);
                    }
                }
            }
        }
    }

    private void copy(InputStream in, File target, boolean progress) throws IOException {
        File parent = target.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
        byte[] buffer = new byte[1 << 16];
        long done = 0, shown = 0;
        try (OutputStream out = new FileOutputStream(target)) {
            for (int n; (n = in.read(buffer)) > 0; ) {
                out.write(buffer, 0, n);
                done += n;
                if (progress && done - shown >= (8 << 20)) {
                    shown = done;
                    final long mb = done >> 20;
                    runOnUiThread(() -> status.setText("Copying the game files… " + mb + " MB"));
                }
            }
        }
    }

    static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) for (File c : children) deleteTree(c);
        if (f.exists() && !f.delete()) Log.w(TAG, "cannot delete " + f);
    }

    private static String readStream(InputStream in) throws IOException {
        StringBuilder text = new StringBuilder();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(in, "UTF-8"))) {
            for (String line; (line = r.readLine()) != null; ) text.append(line).append('\n');
        }
        return text.toString();
    }

    private static String readText(File f) throws IOException {
        try (InputStream in = new java.io.FileInputStream(f)) {
            return readStream(in);
        }
    }
}
