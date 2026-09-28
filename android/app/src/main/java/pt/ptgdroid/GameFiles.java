package pt.ptgdroid;

import android.content.Context;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * The user's own copy of the game lives in app-private storage:
 *   files/card  the N-Gage memory card (drive E:), extracted from the game ZIP
 *   files/c     writable drive C: (settings and saved games)
 */
final class GameFiles {
    private GameFiles() {}

    static File card(Context ctx) { return new File(ctx.getFilesDir(), "card"); }

    static File cDrive(Context ctx) {
        File c = new File(ctx.getFilesDir(), "c");
        c.mkdirs();
        return c;
    }

    /** system/apps/6r72/6r72.app under the imported card, or null if the game is not imported. */
    static File findApp(Context ctx) {
        return findApp(card(ctx), 0);
    }

    private static File findApp(File dir, int depth) {
        File[] list = dir.listFiles();
        if (list == null || depth > 6) return null;
        for (File f : list) {
            if (f.isFile() && f.getName().equalsIgnoreCase("6r72.app") && hasFile(f.getParentFile(), "data.pak")) {
                return f;
            }
        }
        for (File f : list) {
            if (f.isDirectory()) {
                File found = findApp(f, depth + 1);
                if (found != null) return found;
            }
        }
        return null;
    }

    private static boolean hasFile(File dir, String name) {
        File[] list = dir.listFiles();
        if (list == null) return false;
        for (File f : list) if (f.getName().equalsIgnoreCase(name)) return true;
        return false;
    }

    interface Progress { void onBytes(long total); }

    /** Extracts the game ZIP into files/card, replacing any previous import. */
    static void importZip(Context ctx, InputStream in, Progress progress) throws IOException {
        File tmp = new File(ctx.getFilesDir(), "card_import");
        deleteTree(tmp);
        tmp.mkdirs();
        String root = tmp.getCanonicalPath() + File.separator;
        byte[] buf = new byte[1 << 16];
        long total = 0;
        try (ZipInputStream zip = new ZipInputStream(in)) {
            ZipEntry e;
            while ((e = zip.getNextEntry()) != null) {
                File out = new File(tmp, e.getName());
                if (!out.getCanonicalPath().startsWith(root)) {
                    throw new IOException("invalid path in ZIP: " + e.getName());
                }
                if (e.isDirectory()) {
                    out.mkdirs();
                    continue;
                }
                out.getParentFile().mkdirs();
                try (OutputStream os = new FileOutputStream(out)) {
                    int n;
                    while ((n = zip.read(buf)) > 0) {
                        os.write(buf, 0, n);
                        total += n;
                        progress.onBytes(total);
                    }
                }
            }
        }
        File app = findApp(tmp, 0);
        if (app == null) {
            deleteTree(tmp);
            throw new IOException("the ZIP does not contain the game (system/apps/6r72/6r72.app and data.pak)");
        }
        // card root = the directory containing system/apps/6r72/6r72.app
        File cardRoot = app.getParentFile().getParentFile().getParentFile().getParentFile();
        File card = card(ctx);
        deleteTree(card);
        if (!cardRoot.renameTo(card)) throw new IOException("could not move the imported files");
        deleteTree(tmp);
    }

    static void deleteTree(File f) {
        File[] list = f.listFiles();
        if (list != null) for (File c : list) deleteTree(c);
        f.delete();
    }
}
