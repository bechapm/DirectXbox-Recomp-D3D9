package org.xboxrecomp.app;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

import java.util.HashMap;
import java.util.Map;

/*
 * The in-game menu: a panel down the right of the screen, opened with Back.
 * Resume, Settings and Quit game; Settings are the ones the runtime can
 * change while the title runs, plus two read at start, named "(requires
 * restart)".
 *
 * Plain views, built in code (the APK has no layout resources), focusable so
 * a pad's d-pad moves through them, A chooses and B goes back. While it is
 * open the title is held at its next frame (RecompActivity.nativeMenu), so
 * the last frame stays on screen behind it.
 */
final class GameMenu {
    interface Host {
        String settings();                    // key=value;...
        void set(String key, String value);
        void opened(boolean open);
        void quit();
    }

    // The launcher's colours (Theme).
    private static final int PANEL = 0xF00A1208;
    private static final int TEXT = Theme.TEXT;
    private static final int MUTED = Theme.TEXT_DIM;
    private static final int ACCENT = Theme.GREEN;

    private static final String[] CAPS = { "adaptive", "60", "30", "off" };
    private static final String[] CAP_NAMES = { "Adaptive", "60 fps", "30 fps", "Off" };
    private static final String[] TOUCH = { "auto", "on", "off" };
    private static final String[] TOUCH_NAMES = { "Automatic", "On", "Off" };

    private final Activity activity;
    private final Host host;
    private final FrameLayout root;
    private final LinearLayout panel;
    private boolean open;
    private boolean inSubPage;      // Settings: Back returns to the main page
    private View modal;             // the quit question, over everything; null when not shown
    private View quitButton;
    private Map<String, String> values = new HashMap<>();

    GameMenu(Activity activity, Host host) {
        this.activity = activity;
        this.host = host;
        root = new FrameLayout(activity);
        root.setBackgroundColor(0x66000000);
        root.setVisibility(View.GONE);
        root.setOnClickListener(v -> close());      // a tap beside the panel closes it
        // Clickable views take the focus by default (Android 8 on); the
        // shade and the panel are only there to be tapped, never focused.
        root.setFocusable(false);
        panel = new LinearLayout(activity);
        panel.setOrientation(LinearLayout.VERTICAL);
        {
            // A launcher slab down the right, rimmed in the green on its open side.
            GradientDrawable bg = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                                                       new int[] { 0xF0101A0C, PANEL });
            panel.setBackground(bg);
        }
        panel.setClickable(true);                    // taps on the panel stay there
        panel.setFocusable(false);
        int pad = dp(20);
        panel.setPadding(pad, pad, pad, pad);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
            Math.min(dp(420), (int)(activity.getResources().getDisplayMetrics().widthPixels * 0.42f)),
            ViewGroup.LayoutParams.MATCH_PARENT, Gravity.END);
        root.addView(panel, lp);
        activity.addContentView(root, new ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    boolean isOpen() { return open; }

    void toggle() {
        if (open) {
            if (modal != null) closeModal();
            else if (inSubPage) showMain();
            else close();
        } else {
            openMenu();
        }
    }

    void openMenu() {
        open = true;
        host.opened(true);
        showMain();
        root.setVisibility(View.VISIBLE);
        root.bringToFront();
    }

    void close() {
        if (!open) return;
        closeModal();
        open = false;
        root.setVisibility(View.GONE);
        host.opened(false);
    }

    /* A key while the menu is open (a pad's, or a keyboard's). The menu moves
     * its own focus: SDL's activity takes the arrow keys before Android's
     * focus movement would see them. Up/down (and left/right outside a
     * slider) go to the next row that way, A or Enter presses the focused
     * row, B goes back a page or closes, Start closes. Always used here. */
    boolean onKey(KeyEvent e) {
        int code = e.getKeyCode();
        boolean up = e.getAction() == KeyEvent.ACTION_UP;
        View focus = root.findFocus();

        switch (code) {
        case KeyEvent.KEYCODE_BUTTON_B:
            if (up) toggle();
            return true;
        case KeyEvent.KEYCODE_BUTTON_START:
            if (up) close();
            return true;
        case KeyEvent.KEYCODE_BUTTON_A:
        case KeyEvent.KEYCODE_DPAD_CENTER:
        case KeyEvent.KEYCODE_ENTER:
            if (up && focus != null) focus.performClick();
            else if (up) focusFirst();
            return true;
        case KeyEvent.KEYCODE_DPAD_UP:
        case KeyEvent.KEYCODE_DPAD_DOWN:
        case KeyEvent.KEYCODE_DPAD_LEFT:
        case KeyEvent.KEYCODE_DPAD_RIGHT: {
            if (up) return true;
            boolean sideways = code == KeyEvent.KEYCODE_DPAD_LEFT || code == KeyEvent.KEYCODE_DPAD_RIGHT;
            if (focus instanceof SeekBar && sideways) {
                SeekBar bar = (SeekBar)focus;
                int step = Math.max(1, bar.getMax() / 18);
                bar.setProgress(bar.getProgress() + (code == KeyEvent.KEYCODE_DPAD_RIGHT ? step : -step), true);
                return true;
            }
            if (focus == null) {
                focusFirst();
                return true;
            }
            int dir = code == KeyEvent.KEYCODE_DPAD_UP ? View.FOCUS_UP :
                      code == KeyEvent.KEYCODE_DPAD_DOWN ? View.FOCUS_DOWN :
                      code == KeyEvent.KEYCODE_DPAD_LEFT ? View.FOCUS_LEFT : View.FOCUS_RIGHT;
            View next = focus.focusSearch(dir);
            // Stay inside what is showing: the question while it is up.
            if (next != null && (modal == null || isInside(next, modal)))
                next.requestFocus();
            return true;
        }
        default:
            return true;                // nothing reaches the game while the menu is open
        }
    }

    private void focusFirst() {
        View top = modal != null ? modal : panel;
        java.util.ArrayList<View> all = top.getFocusables(View.FOCUS_DOWN);
        if (!all.isEmpty()) all.get(0).requestFocus();
    }

    private static boolean isInside(View v, View parent) {
        for (android.view.ViewParent p = v.getParent(); p != null; p = p.getParent())
            if (p == parent) return true;
        return false;
    }

    // ---- pages ----

    private void showMain() {
        inSubPage = false;
        panel.removeAllViews();
        panel.addView(title("Paused"));
        View resume = button("Resume", v -> close());
        panel.addView(resume);
        panel.addView(button("Settings", v -> showSettings()));
        quitButton = button("Quit game", v -> showQuit());
        panel.addView(quitButton);
        resume.requestFocus();
    }

    /* Asked first, in a box over the middle of the screen: a quit loses
     * whatever the game has not saved, and Quit game sits one press below
     * Settings. No has the focus, so a stray A press is harmless; B, Back or
     * a tap outside the box is No as well. */
    private void showQuit() {
        if (modal != null) return;
        FrameLayout shade = new FrameLayout(activity);
        shade.setBackgroundColor(0x99000000);
        shade.setClickable(true);
        shade.setOnClickListener(v -> closeModal());
        shade.setFocusable(false);

        LinearLayout card = new LinearLayout(activity);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setClickable(true);                     // taps on the box stay there
        card.setFocusable(false);
        int pad = dp(24);
        card.setPadding(pad, pad, pad, dp(16));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(PANEL);
        bg.setCornerRadius(dp(16));
        card.setBackground(bg);

        TextView ask = text("Are you sure?", 22, TEXT, true);
        ask.setGravity(Gravity.CENTER_HORIZONTAL);
        card.addView(ask);
        TextView why = text("Quitting loses anything you haven't saved in the game.", 15, MUTED, false);
        why.setGravity(Gravity.CENTER_HORIZONTAL);
        why.setPadding(0, dp(8), 0, dp(20));
        card.addView(why);

        LinearLayout buttons = new LinearLayout(activity);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        View no = button("No", v -> closeModal());
        View yes = button("Yes", v -> host.quit());
        ((TextView)no).setGravity(Gravity.CENTER);
        ((TextView)yes).setGravity(Gravity.CENTER);
        no.setBackground(boxBackground());
        yes.setBackground(boxBackground());
        LinearLayout.LayoutParams half = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        half.setMargins(dp(6), 0, dp(6), 0);
        buttons.addView(no, half);
        buttons.addView(yes, new LinearLayout.LayoutParams(half));
        card.addView(buttons);

        int width = Math.min(dp(440), (int)(activity.getResources().getDisplayMetrics().widthPixels * 0.8f));
        shade.addView(card, new FrameLayout.LayoutParams(width, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER));
        root.addView(shade, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        // The menu behind cannot take the focus while the question is up.
        panel.setDescendantFocusability(ViewGroup.FOCUS_BLOCK_DESCENDANTS);
        modal = shade;
        no.requestFocus();
    }

    private void closeModal() {
        if (modal == null) return;
        root.removeView(modal);
        modal = null;
        panel.setDescendantFocusability(ViewGroup.FOCUS_AFTER_DESCENDANTS);
        if (quitButton != null) quitButton.requestFocus();
    }

    private void showSettings() {
        inSubPage = true;
        values = parse(host.settings());
        panel.removeAllViews();
        panel.addView(title("Settings"));
        ScrollView scroll = new ScrollView(activity);
        LinearLayout list = new LinearLayout(activity);
        list.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(list);
        panel.addView(scroll, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        list.addView(section("Display"));
        View first = toggleRow(list, "Frame rate counter", null, "fps_overlay");
        choiceRow(list, "Frame rate limit", null, "frame_cap", CAPS, CAP_NAMES);
        // Read when the game starts, so a change shows from the next start.
        toggleRow(list, "Widescreen (requires restart)", null, "widescreen");
        choiceRow(list, "Resolution (requires restart)", null,
                  "resolution_scale", new String[] { "1", "2", "3", "4" },
                  new String[] { "Native (1x)", "2x", "3x", "4x" });

        list.addView(section("Touch controls"));
        choiceRow(list, "Show", "Automatic: shown with no controller connected",
                  "touch_controls", TOUCH, TOUCH_NAMES);
        sliderRow(list, "Opacity", "touch_opacity", 10, 100);
        sliderRow(list, "Size", "touch_scale", 50, 200);

        panel.addView(button("Back", v -> showMain()));
        first.requestFocus();
    }

    // ---- rows ----

    private View toggleRow(LinearLayout list, String name, String note, final String key) {
        final TextView value = valueText(on(key) ? "On" : "Off");
        View row = row(name, note, value);
        row.setOnClickListener(v -> {
            boolean now = !on(key);
            values.put(key, now ? "1" : "0");
            value.setText(now ? "On" : "Off");
            host.set(key, now ? "1" : "0");
        });
        list.addView(row);
        return row;
    }

    private View choiceRow(LinearLayout list, String name, String note, final String key,
                           final String[] keys, final String[] names) {
        final TextView value = valueText(names[indexOf(keys, values.get(key))]);
        View row = row(name, note, value);
        row.setOnClickListener(v -> {
            int next = (indexOf(keys, values.get(key)) + 1) % keys.length;
            values.put(key, keys[next]);
            value.setText(names[next]);
            host.set(key, keys[next]);
        });
        list.addView(row);
        return row;
    }

    private void sliderRow(LinearLayout list, String name, final String key, final int min, final int max) {
        LinearLayout box = new LinearLayout(activity);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(12), dp(8), dp(12), dp(8));
        final TextView label = text(name, 16, TEXT, false);
        box.addView(label);
        final SeekBar bar = new SeekBar(activity);
        bar.setMax(max - min);
        int now = Math.round(parseFloat(values.get(key), 1f) * 100f);
        bar.setProgress(Math.max(0, Math.min(max - min, now - min)));
        label.setText(name + "  " + now + "%");
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean fromUser) {
                int pct = p + min;
                label.setText(name + "  " + pct + "%");
                String v = String.valueOf(pct / 100f);
                values.put(key, v);
                host.set(key, v);
            }
            @Override public void onStartTrackingTouch(SeekBar s) { }
            @Override public void onStopTrackingTouch(SeekBar s) { }
        });
        box.addView(bar);
        list.addView(box);
    }

    private View row(String name, String note, TextView value) {
        LinearLayout row = new LinearLayout(activity);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(12), dp(10), dp(12), dp(10));
        row.setFocusable(true);
        row.setClickable(true);
        row.setBackground(rowBackground());
        LinearLayout words = new LinearLayout(activity);
        words.setOrientation(LinearLayout.VERTICAL);
        words.addView(text(name, 16, TEXT, false));
        if (note != null) words.addView(text(note, 12, MUTED, false));
        row.addView(words, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        row.addView(value);
        return row;
    }

    private View button(String name, View.OnClickListener click) {
        TextView b = text(name, 18, TEXT, true);
        b.setPadding(dp(14), dp(14), dp(14), dp(14));
        b.setFocusable(true);
        b.setClickable(true);
        b.setBackground(rowBackground());
        b.setOnClickListener(click);
        return b;
    }

    private TextView title(String s) {
        TextView t = text(s, 26, TEXT, true);
        t.setPadding(dp(12), 0, dp(12), dp(14));
        return t;
    }

    private TextView section(String s) {
        TextView t = text(s.toUpperCase(), 12, MUTED, true);
        t.setLetterSpacing(0.08f);
        t.setPadding(dp(12), dp(16), dp(12), dp(4));
        return t;
    }

    private TextView valueText(String s) {
        return text(s, 15, ACCENT, true);
    }

    private TextView text(String s, int sp, int color, boolean bold) {
        TextView t = new TextView(activity);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    /* A button that looks like one: a launcher panel, lit with the focus. */
    private StateListDrawable boxBackground() {
        return Theme.panel(activity);
    }

    /* A row: nothing at rest, a lit launcher panel with the focus. */
    private StateListDrawable rowBackground() {
        StateListDrawable d = new StateListDrawable();
        d.addState(new int[] { android.R.attr.state_focused }, Theme.slab(activity, true));
        d.addState(new int[] { android.R.attr.state_pressed }, Theme.slab(activity, true));
        d.addState(new int[] { }, new GradientDrawable());
        return d;
    }

    // ---- values ----

    private boolean on(String key) { return "1".equals(values.get(key)); }

    private static int indexOf(String[] keys, String v) {
        for (int i = 0; i < keys.length; i++)
            if (keys[i].equals(v)) return i;
        return 0;
    }

    private static float parseFloat(String s, float fallback) {
        try { return s == null ? fallback : Float.parseFloat(s); } catch (NumberFormatException e) { return fallback; }
    }

    private static Map<String, String> parse(String s) {
        Map<String, String> m = new HashMap<>();
        if (s == null) return m;
        for (String kv : s.split(";")) {
            int eq = kv.indexOf('=');
            if (eq > 0) m.put(kv.substring(0, eq), kv.substring(eq + 1));
        }
        return m;
    }

    private int dp(int v) {
        return Math.round(v * activity.getResources().getDisplayMetrics().density);
    }
}
