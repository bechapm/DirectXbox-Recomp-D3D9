package org.xboxrecomp.app;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;

/*
 * The app's first screen: which copy of the game to build from. The APK
 * carries no game; the player's own copy is read where it is, and the game is
 * built from it on the device (BuildActivity).
 *
 * A copy chosen before, still there, goes straight on. Otherwise a full
 * screen in the launcher's look (Theme), with:
 *
 *   - Choose game folder / Choose disc image: Android's own file picker (the
 *     storage access framework), as other emulators use it. What it returns is
 *     a document link; with all-files access, one on the device's storage or an
 *     SD card is a path the pipeline and the engine can read, which is what is
 *     kept.
 *   - Found on this device: folders under the usual places whose boot XBE
 *     is one of the release's images (the manifest's xboxrecomp.xbe_sha1), and
 *     disc images (checked when they are built).
 *   - Allow access to files, while the app does not have it.
 *
 * Nothing is written to the game's folder: E: and the saves are the app's own
 * (android_main.c).
 */
public class SetupActivity extends Activity {
    static final String PREFS = "xboxrecomp";
    static final String KEY_GAME = "game_dir";
    private static final int MAX_DEPTH = 4;
    private static final int PICK_FOLDER = 1, PICK_IMAGE = 2;

    private LinearLayout found;
    private TextView foundTitle, message;
    private View folderButton, imageButton, accessButton;
    private volatile int scanGeneration;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        String saved = getSharedPreferences(PREFS, MODE_PRIVATE).getString(KEY_GAME, null);
        if (hasGame(saved)) {
            start(saved);
            return;
        }
        File own = getExternalFilesDir(null);
        if (own != null && hasGame(new File(own, "game").getPath())) {
            start(new File(own, "game").getPath());       // a copy pushed for development
            return;
        }
        build();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (found != null)
            refresh();                                    // back from the access screen
    }

    // ---- the screen ----

    private void build() {
        ScrollView scroll = new ScrollView(this);
        scroll.setBackground(Theme.background());
        scroll.setFillViewport(true);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setGravity(Gravity.CENTER_HORIZONTAL);
        int pad = dp(32);
        box.setPadding(pad, dp(28), pad, pad);
        scroll.addView(box);

        box.addView(Theme.text(this, getApplicationInfo().loadLabel(getPackageManager()).toString(),
                               28, Theme.TEXT, true));
        TextView sub = Theme.text(this, "Choose your copy of the game", 18, Theme.GREEN, false);
        sub.setPadding(0, dp(10), 0, dp(4));
        box.addView(sub);
        TextView how = Theme.text(this, "A folder holding " + bootXbe() + ", or a disc image (.iso). "
                                  + "It is only read: the game is built from it on this device, once.",
                                  14, Theme.TEXT_DIM, false);
        how.setPadding(0, 0, 0, dp(18));
        box.addView(how);

        message = Theme.text(this, "", 14, Theme.WARN, false);
        message.setPadding(0, 0, 0, dp(12));
        message.setVisibility(View.GONE);
        box.addView(message);

        accessButton = button("Allow access to files",
                              "Needed to read your game where it is", v -> askAccess());
        LinearLayout.LayoutParams alp = wide();
        alp.setMargins(dp(6), dp(6), dp(6), dp(6));
        box.addView(accessButton, alp);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER);
        folderButton = button("Choose game folder", "The folder with " + bootXbe(), v -> pick(PICK_FOLDER));
        imageButton = button("Choose disc image", "An .iso of the game", v -> pick(PICK_IMAGE));
        LinearLayout.LayoutParams half = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        half.setMargins(dp(6), dp(6), dp(6), dp(6));
        row.addView(folderButton, half);
        row.addView(imageButton, new LinearLayout.LayoutParams(half));
        box.addView(row, wide());

        foundTitle = Theme.text(this, "", 12, Theme.TEXT_DIM, true);
        foundTitle.setLetterSpacing(0.08f);
        foundTitle.setGravity(Gravity.START);
        foundTitle.setPadding(dp(6), dp(22), 0, dp(6));
        box.addView(foundTitle, wide());
        found = new LinearLayout(this);
        found.setOrientation(LinearLayout.VERTICAL);
        box.addView(found, wide());

        setContentView(scroll);
        refresh();
    }

    private LinearLayout.LayoutParams wide() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            Math.min(dp(760), getResources().getDisplayMetrics().widthPixels - dp(64)),
            ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.gravity = Gravity.CENTER_HORIZONTAL;
        return lp;
    }

    private boolean haveAccess() {
        return Build.VERSION.SDK_INT < 30 || Environment.isExternalStorageManager();
    }

    /* Shows what fits the access the app has now, and looks for copies. */
    private void refresh() {
        boolean access = haveAccess();
        accessButton.setVisibility(access ? View.GONE : View.VISIBLE);
        folderButton.setEnabled(access);
        imageButton.setEnabled(access);
        folderButton.setAlpha(access ? 1f : 0.4f);
        imageButton.setAlpha(access ? 1f : 0.4f);
        found.removeAllViews();
        (access ? folderButton : accessButton).requestFocus();
        if (!access) {
            foundTitle.setText("");
            return;
        }
        foundTitle.setText("LOOKING FOR COPIES ON THIS DEVICE…");
        final int gen = ++scanGeneration;
        new Thread(() -> {
            List<String> copies = scan();
            runOnUiThread(() -> {
                if (gen != scanGeneration) return;
                foundTitle.setText(copies.isEmpty()
                    ? "NO COPIES FOUND IN roms/, Download/, games/ OR ON AN SD CARD"
                    : "FOUND ON THIS DEVICE");
                for (String c : copies) {
                    File f = new File(c);
                    View b = button(f.getName(), (f.isFile() ? "Disc image · " : "Folder · ") + f.getParent(),
                                    v -> choose(c));
                    LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                    lp.setMargins(dp(6), dp(4), dp(6), dp(4));
                    found.addView(b, lp);
                }
            });
        }, "find-copies").start();
    }

    private void askAccess() {
        startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                 Uri.parse("package:" + getPackageName())));
    }

    private void say(String s) {
        message.setText(s);
        message.setVisibility(s.isEmpty() ? View.GONE : View.VISIBLE);
    }

    // ---- Android's file picker ----

    private void pick(int what) {
        say("");
        Intent i;
        if (what == PICK_FOLDER) {
            i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        } else {
            i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            // An .iso has no agreed MIME type; providers give it several.
            i.setType("*/*");
        }
        try {
            startActivityForResult(i, what);
        } catch (Exception e) {
            say("This device has no file picker. Choose a copy found below.");
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null)
            return;
        Uri uri = data.getData();
        String path = request == PICK_FOLDER ? treePath(uri) : documentPath(uri);
        if (path == null) {
            say("That place does not give the app a file path. In the picker, choose it from "
                + "the device's own storage or SD card (its menu lists them), not from Recent or Downloads.");
            return;
        }
        if (request == PICK_FOLDER && !new File(path, bootXbe()).isFile()) {
            say("There is no " + bootXbe() + " in " + path + ". Choose the game's own folder, the one holding it.");
            return;
        }
        if (request == PICK_IMAGE && !new File(path).isFile()) {
            say("Cannot read " + path + ".");
            return;
        }
        choose(path);
    }

    /* A folder chosen in the picker, as a path: the external storage
     * provider names it volume:relative ("primary:roms/xbox/Game"). */
    private static String treePath(Uri uri) {
        try {
            return idPath(DocumentsContract.getTreeDocumentId(uri));
        } catch (Exception e) {
            return null;
        }
    }

    private static String documentPath(Uri uri) {
        try {
            String id = DocumentsContract.getDocumentId(uri);
            if (id.startsWith("raw:"))                      // the Downloads provider, sometimes
                return id.substring(4);
            if ("com.android.externalstorage.documents".equals(uri.getAuthority()))
                return idPath(id);
        } catch (Exception e) {
            // fall through
        }
        return null;
    }

    private static String idPath(String id) {
        int colon = id.indexOf(':');
        if (colon < 0)
            return null;
        String volume = id.substring(0, colon), rel = id.substring(colon + 1);
        String root = "primary".equalsIgnoreCase(volume)
            ? Environment.getExternalStorageDirectory().getPath() : "/storage/" + volume;
        return rel.isEmpty() ? root : root + "/" + rel;
    }

    // ---- copies found without asking ----

    private String meta(String key) {
        try {
            ApplicationInfo ai = getPackageManager().getApplicationInfo(
                getPackageName(), PackageManager.GET_META_DATA);
            if (ai.metaData != null)
                return ai.metaData.getString(key);
        } catch (PackageManager.NameNotFoundException e) {
            // fall through
        }
        return null;
    }

    private String expectedSha1() {
        // The release's images, comma separated (PAL, USA, ...).
        String s = meta("xboxrecomp.xbe_sha1");
        return s != null && s.length() >= 40 ? s.toLowerCase() : null;
    }

    /* The XBE the game boots: default.xbe unless the recipe names another
     * (Gladius's gladius_eng.xbe beside a language launcher). */
    private String bootXbe() {
        if (bootXbe == null) {
            String s = meta("xboxrecomp.xbe");
            bootXbe = s != null && !s.isEmpty() ? s : "default.xbe";
        }
        return bootXbe;
    }

    private String bootXbe;

    private List<String> scan() {
        String want = expectedSha1();
        List<String> out = new ArrayList<>();
        File root = Environment.getExternalStorageDirectory();
        // Shared storage ignores case: roms/ and ROMs/ are one folder.
        for (String d : new String[] { "roms", "Download", "games" })
            look(new File(root, d), 0, want, out);
        File[] cards = new File("/storage").listFiles();
        if (cards != null)
            for (File c : cards)
                if (!c.getName().equals("emulated") && !c.getName().equals("self"))
                    look(c, 0, want, out);                 // an SD card
        return out;
    }

    private void look(File dir, int depth, String want, List<String> out) {
        if (depth > MAX_DEPTH || dir == null || !dir.isDirectory())
            return;
        File xbe = new File(dir, bootXbe());
        if (xbe.isFile()) {
            String digest = sha1(xbe);
            if (want == null || (!digest.isEmpty() && want.contains(digest)))
                add(out, dir.getPath());
            return;
        }
        File[] kids = dir.listFiles();
        if (kids == null)
            return;
        for (File k : kids) {
            if (k.isDirectory() && !k.getName().startsWith("."))
                look(k, depth + 1, want, out);
            else if (k.isFile() && isImage(k) && isXboxImage(k))
                add(out, k.getPath());
        }
    }

    private static void add(List<String> out, String path) {
        for (String p : out)
            if (p.equalsIgnoreCase(path))
                return;
        out.add(path);
    }

    private static String sha1(File f) {
        try (InputStream in = new FileInputStream(f)) {
            MessageDigest md = MessageDigest.getInstance("SHA-1");
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0)
                md.update(buf, 0, n);
            StringBuilder sb = new StringBuilder();
            for (byte b : md.digest())
                sb.append(String.format("%02x", b));
            return sb.toString();
        } catch (Exception e) {
            return "";
        }
    }

    /* A folder holding the game, or a disc image of it. */
    private boolean hasGame(String dir) {
        if (dir == null)
            return false;
        File f = new File(dir);
        if (f.isFile())
            return isImage(f);
        return new File(f, bootXbe()).isFile();
    }

    /* Whether a disc image is an Xbox one: its XDVDFS volume descriptor, at
     * sector 32 of the game partition, starts with the magic. The bases are
     * the ones tools/xiso/xdvdfs.py tries, for bare and full-disc dumps. */
    private static final long[] XISO_BASES = { 0x00000000L, 0x0000FD90L, 0x00030600L,
                                               0x0FD90000L, 0x18300000L };
    private static final byte[] XISO_MAGIC = "MICROSOFT*XBOX*MEDIA".getBytes();

    private static boolean isXboxImage(File f) {
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
            byte[] b = new byte[XISO_MAGIC.length];
            for (long base : XISO_BASES) {
                long at = base + 32L * 2048L;
                if (at + b.length > r.length())
                    continue;
                r.seek(at);
                r.readFully(b);
                if (java.util.Arrays.equals(b, XISO_MAGIC))
                    return true;
            }
        } catch (Exception e) {
            // not readable: not offered
        }
        return false;
    }

    private static boolean isImage(File f) {
        String n = f.getName().toLowerCase();
        return n.endsWith(".iso") || n.endsWith(".xiso");
    }

    // ---- going on ----

    private void choose(String path) {
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(KEY_GAME, path).apply();
        start(path);
    }

    /* The game is built from this copy on the device first (BuildActivity),
     * or found built already, and then started. */
    private void start(String path) {
        Intent i = new Intent(this, BuildActivity.class);
        i.putExtra(BuildActivity.EXTRA_SOURCE, path);
        startActivity(i);
        finish();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        // A pad's B leaves, as Back does.
        if (e.getKeyCode() == KeyEvent.KEYCODE_BUTTON_B) {
            if (e.getAction() == KeyEvent.ACTION_UP)
                finish();
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    // ---- views ----

    /* A big button: a title and a line under it, on a launcher panel that
     * lights with the focus (a pad) or the press (a finger). */
    private View button(String title, String line, View.OnClickListener click) {
        LinearLayout b = new LinearLayout(this);
        b.setOrientation(LinearLayout.VERTICAL);
        b.setPadding(dp(18), dp(14), dp(18), dp(14));
        b.setFocusable(true);
        b.setClickable(true);
        b.setOnClickListener(click);
        b.setBackground(Theme.panel(this));
        TextView t = Theme.text(this, title, 17, Theme.TEXT, true);
        t.setGravity(Gravity.START);
        b.addView(t);
        TextView l = Theme.text(this, line, 13, Theme.TEXT_DIM, false);
        l.setGravity(Gravity.START);
        l.setSingleLine(true);
        l.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        b.addView(l);
        return b;
    }

    private int dp(int v) {
        return Theme.dp(this, v);
    }
}
