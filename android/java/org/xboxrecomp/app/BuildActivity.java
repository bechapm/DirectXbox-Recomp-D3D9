package org.xboxrecomp.app;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/*
 * Makes the game from the player's own copy, on the device, then starts it.
 *
 * The APK carries no game code. What it carries to build one -- Python, the
 * pipeline, the title's recipe, the headers and flags, zig's library and the
 * NDK sysroot -- is assets/buildkit.zip, unpacked once per install into the
 * app's internal folder. The build itself is tools/android/device_build.py,
 * run by the APK's Python (libpyexec.so); its progress comes back as JSON
 * lines. The result is libgame.so in the internal folder, which the engine
 * loads (src/host/android_title.c), and a stamp: a later start with the same
 * copy and the same kit goes straight to the game.
 */
public class BuildActivity extends Activity {
    static final String EXTRA_SOURCE = "source";

    private TextView stepText, detailText;
    private ProgressBar bar;
    private LinearLayout buttons;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private String source;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        source = getIntent().getStringExtra(EXTRA_SOURCE);

        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setGravity(Gravity.CENTER);
        box.setBackground(Theme.background());
        int pad = dp(32);
        box.setPadding(pad, pad, pad, pad);

        TextView title = text(getApplicationInfo().loadLabel(getPackageManager()).toString(), 26, true);
        box.addView(title);
        stepText = text("Getting ready", 18, false);
        stepText.setTextColor(Theme.GREEN);
        stepText.setPadding(0, dp(16), 0, dp(8));
        box.addView(stepText);
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setProgressTintList(android.content.res.ColorStateList.valueOf(Theme.GREEN));
        bar.setProgressBackgroundTintList(android.content.res.ColorStateList.valueOf(Theme.GREEN_DIM));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(520), dp(12));
        box.addView(bar, lp);
        detailText = text("", 13, false);
        detailText.setTextColor(Theme.TEXT_DIM);
        detailText.setPadding(0, dp(8), 0, 0);
        box.addView(detailText);
        buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.CENTER);
        buttons.setPadding(0, dp(20), 0, 0);
        box.addView(buttons);
        setContentView(box);

        new Thread(this::work, "game-build").start();
    }

    private void work() {
        try {
            File kit = unpackKit();
            File out = new File(getFilesDir(), "libgame.so");
            JSONObject spec = new JSONObject(readAll(new File(kit, "android-build.json")));
            File stamp = new File(getFilesDir(), "build/stamp.json");
            if (out.isFile() && stamp.isFile()) {
                JSONObject s = new JSONObject(readAll(stamp));
                if (source.equals(s.optString("source")) &&
                        spec.optString("kit_version").equals(s.optString("kit"))) {
                    play(s.optString("game_dir", source));
                    return;
                }
            }
            build(kit);
        } catch (Exception e) {
            fail("The game could not be prepared: " + e.getMessage());
        }
    }

    /* assets/buildkit.zip into <internal>/kit, once for each install. */
    private File unpackKit() throws Exception {
        File kit = new File(getFilesDir(), "kit");
        PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
        String want = String.valueOf(pi.lastUpdateTime);
        File mark = new File(kit, ".unpacked-from");
        if (mark.isFile() && want.equals(readAll(mark).trim()))
            return kit;
        status("Unpacking the build tools", 0, 1, "once for each install of the app");
        deleteTree(kit);
        kit.mkdirs();
        long total = getAssets().openFd("buildkit.zip").getLength(), seen = 0;
        byte[] buf = new byte[1 << 16];
        try (InputStream raw = getAssets().open("buildkit.zip");
             ZipInputStream zip = new ZipInputStream(raw)) {
            ZipEntry e;
            int n = 0;
            while ((e = zip.getNextEntry()) != null) {
                File f = new File(kit, e.getName());
                if (!f.getCanonicalPath().startsWith(kit.getCanonicalPath() + File.separator))
                    throw new Exception("a bad path in the build kit: " + e.getName());
                if (e.isDirectory()) {
                    f.mkdirs();
                    continue;
                }
                f.getParentFile().mkdirs();
                try (OutputStream o = new FileOutputStream(f)) {
                    int r;
                    while ((r = zip.read(buf)) > 0) o.write(buf, 0, r);
                }
                seen += Math.max(0, e.getCompressedSize());
                if (++n % 100 == 0)
                    status("Unpacking the build tools", seen, total, e.getName());
            }
        }
        try (OutputStream o = new FileOutputStream(mark)) {
            o.write(want.getBytes());
        }
        return kit;
    }

    private void build(File kit) throws Exception {
        String libdir = getApplicationInfo().nativeLibraryDir;
        File work = new File(getFilesDir(), "build");
        work.mkdirs();
        File gameOut = new File(getExternalFilesDir(null), "game-from-image");
        ProcessBuilder pb = new ProcessBuilder(
            libdir + "/libpyexec.so", "-u",
            new File(kit, "toolkit/tools/android/device_build.py").getPath(),
            "--kit", kit.getPath(), "--source", source,
            "--work", work.getPath(), "--game-out", gameOut.getPath(),
            "--libdir", libdir, "--out", new File(getFilesDir(), "libgame.so").getPath());
        Map<String, String> env = pb.environment();
        env.put("PYTHONHOME", new File(kit, "python").getPath());
        env.put("LD_LIBRARY_PATH", libdir);
        env.put("LIBCAPSTONE_PATH", libdir);
        env.put("HOME", getFilesDir().getPath());
        pb.redirectErrorStream(true);
        Process p = pb.start();
        String result = null;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()))) {
            String line;
            while ((line = r.readLine()) != null) {
                // Also in logcat (tag xboxrecomp-build): the log itself is in
                // the internal folder, which adb cannot read.
                Log.i("xboxrecomp-build", line);
                if (!line.startsWith("{"))
                    continue;
                JSONObject o = new JSONObject(line);
                if (o.has("step")) {
                    onStep(o.getString("step"), o.optInt("done"), o.optInt("total"), o.optString("what"));
                } else if (o.has("result")) {
                    result = line;
                }
            }
        }
        p.waitFor();
        if (result == null) {
            fail("The build stopped without saying why (exit " + p.exitValue() + ").");
            return;
        }
        JSONObject o = new JSONObject(result);
        if ("ok".equals(o.getString("result")))
            play(o.optString("game_dir", source));
        else
            fail(o.optString("message", "The build failed."));
    }

    private void onStep(String step, int done, int total, String what) {
        // Each step's share of the bar, roughly its share of the time.
        double base, span;
        String label;
        switch (step) {
        case "game":    base = 0.00; span = 0.15; label = "Unpacking the disc image"; break;
        case "check":   base = 0.15; span = 0.01; label = "Checking the game"; break;
        case "lift":    base = 0.16; span = 0.34; label = "Translating the game to C (about half a minute)"; break;
        case "compile": base = 0.50; span = 0.46; label = "Compiling the game"; break;
        case "link":    base = 0.96; span = 0.04; label = "Linking"; break;
        default:        base = 0; span = 0; label = step;
        }
        double f = total > 0 ? (double) done / total : 0;
        String detail = step.equals("compile") && total > 0 ? done + " of " + total + (what.isEmpty() ? "" : " · " + what) : what;
        status(label, (long) ((base + span * f) * 1000), 1000, detail);
    }

    private void status(String step, long done, long total, String detail) {
        ui.post(() -> {
            stepText.setText(step);
            bar.setProgress(total > 0 ? (int) (done * 1000 / total) : 0);
            detailText.setText(detail);
        });
    }

    private void play(String gameDir) {
        ui.post(() -> {
            Intent i = new Intent(this, RecompActivity.class);
            i.putExtra(SetupActivity.KEY_GAME, gameDir);
            startActivity(i);
            finish();
        });
    }

    private void fail(String message) {
        File log = new File(getFilesDir(), "build/build.log");
        String tail = log.isFile() ? tail(log, 25) : "";
        ui.post(() -> {
            stepText.setText("The game could not be built");
            detailText.setText(message + (tail.isEmpty() ? "" : "\n\n" + tail));
            detailText.setTypeface(Typeface.MONOSPACE);
            detailText.setGravity(Gravity.START);
            buttons.removeAllViews();
            View retry = button("Try again", v -> recreate());
            buttons.addView(retry);
            View other = button("Choose another copy", v -> {
                getSharedPreferences(SetupActivity.PREFS, MODE_PRIVATE).edit()
                    .remove(SetupActivity.KEY_GAME).apply();
                startActivity(new Intent(this, SetupActivity.class));
                finish();
            });
            buttons.addView(other);
            retry.requestFocus();
        });
    }

    // ---- small helpers ----

    private static String readAll(File f) throws Exception {
        try (InputStream in = new FileInputStream(f)) {
            byte[] b = new byte[(int) f.length()];
            int off = 0, r;
            while (off < b.length && (r = in.read(b, off, b.length - off)) > 0) off += r;
            return new String(b, 0, off, "UTF-8");
        }
    }

    private static String tail(File f, int lines) {
        try {
            String[] all = readAll(f).split("\n");
            StringBuilder sb = new StringBuilder();
            for (int i = Math.max(0, all.length - lines); i < all.length; i++)
                sb.append(all[i]).append('\n');
            return sb.toString();
        } catch (Exception e) {
            return "";
        }
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null)
            for (File k : kids) deleteTree(k);
        f.delete();
    }

    private TextView text(String s, int sp, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(Theme.TEXT);
        t.setGravity(Gravity.CENTER);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    /* A launcher panel as a button, lit with the focus or the press. */
    private View button(String label, View.OnClickListener click) {
        TextView b = text(label, 16, true);
        b.setPadding(dp(22), dp(12), dp(22), dp(12));
        b.setFocusable(true);
        b.setClickable(true);
        b.setBackground(Theme.panel(this));
        b.setOnClickListener(click);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMargins(dp(8), 0, dp(8), 0);
        b.setLayoutParams(lp);
        return b;
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
