package org.xboxrecomp.app;

import android.content.Context;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.TextView;

/*
 * The launcher's look (src/launcher/launcher_theme.h, .c), for the app's own
 * screens: setup, the build, the in-game menu. The same colours, so the app
 * reads as the same thing on every platform:
 *
 *   the Xbox green and its dim shade, the green-white text and its dim shade;
 *   a black background with a green glow behind where the content sits;
 *   panels that are dark translucent slabs at rest and, selected (a pad's
 *   focus, a finger's press), edged and washed with the green.
 */
final class Theme {
    // launcher_theme.h
    static final int GREEN     = 0xFF9BCE1E;
    static final int GREEN_DIM = 0xFF4E6C14;
    static final int TEXT      = 0xFFE8F2D8;
    static final int TEXT_DIM  = 0xFF8A977C;
    static final int WARN      = 0xFFE0B04A;

    private Theme() { }

    /* The background: black, with the glow behind the content
     * (theme_background, without its slow pulse). */
    static Drawable background() {
        GradientDrawable g = new GradientDrawable();
        g.setGradientType(GradientDrawable.RADIAL_GRADIENT);
        g.setColors(new int[] { 0xFF26491A, 0xFF121A0E, 0xFF050704 });
        g.setGradientCenter(0.5f, 0.42f);
        g.setGradientRadius(1400f);
        return g;
    }

    /* A panel (theme_panel): a dark slab at rest, edged and washed with the
     * green when it is selected. */
    static StateListDrawable panel(Context c) {
        StateListDrawable d = new StateListDrawable();
        d.addState(new int[] { android.R.attr.state_focused }, slab(c, true));
        d.addState(new int[] { android.R.attr.state_pressed }, slab(c, true));
        d.addState(new int[] { }, slab(c, false));
        return d;
    }

    static GradientDrawable slab(Context c, boolean lit) {
        GradientDrawable g = new GradientDrawable();
        g.setCornerRadius(dp(c, 10));
        if (lit) {
            g.setColor(0xD8224E16);
            g.setStroke(dp(c, 2), GREEN);
        } else {
            g.setColor(0x8C0A1208);
            g.setStroke(dp(c, 1), 0x409BCE1E);
        }
        return g;
    }

    static TextView text(Context c, String s, int sp, int color, boolean bold) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        t.setGravity(Gravity.CENTER);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    static int dp(Context c, int v) {
        return Math.round(v * c.getResources().getDisplayMetrics().density);
    }
}
