/*
 * launcher_theme.c -- see launcher_theme.h.
 *
 * The background and the panels are written pixel by pixel into a 32-bit
 * buffer rather than drawn with the host's shapes. That is not perversity:
 * GDI has no antialiasing and no alpha on its primitives, and this look is
 * made of soft edges and washes. Arithmetic over a buffer gives both, costs
 * a few milliseconds on a window this size, and is the same on every host.
 *
 * The buffer itself, and text, which wants a font engine that knows how to
 * shape and hint, are the host's: launcher_theme_gdi.c and
 * launcher_theme_coretext.c, with launcher_theme_buf.h between them and this.
 *
 * Everything is laid out in window units and drawn in pixels, theme_scale
 * to the unit: each shape is worked out where a pixel falls in window units,
 * so a Retina display gets the same panel with four times the pixels in it
 * rather than the same pixels at four times the size. At a scale of 1 that
 * is the pixel itself, and nothing differs from drawing in pixels.
 */
#include "launcher_theme.h"
#include "launcher_theme_buf.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

uint32_t *theme_px;
int       theme_w, theme_h;
double    theme_scale = 1.0;

/* ------------------------------------------------------------- pixels */

static double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static void blend(int x, int y, int r, int g, int b, double a)
{
    uint32_t *p;
    int sr, sg, sb;

    if (x < 0 || y < 0 || x >= theme_w || y >= theme_h || a <= 0)
        return;
    if (a > 1) a = 1;
    p = &theme_px[(size_t)y * theme_w + x];
    sb = (int)(*p & 0xFF);
    sg = (int)((*p >> 8) & 0xFF);
    sr = (int)((*p >> 16) & 0xFF);
    sr = (int)(sr + (r - sr) * a);
    sg = (int)(sg + (g - sg) * a);
    sb = (int)(sb + (b - sb) * a);
    *p = ((uint32_t)sr << 16) | ((uint32_t)sg << 8) | (uint32_t)sb;
}

/* What of the background does not move: each pixel's distance from the
 * glow's centre, and how much vignette is over it. Worked out once per
 * buffer size, since the glow breathing changes neither -- two square roots
 * a pixel every frame is most of a frame on a Retina display. */
static float *g_bg_dist, *g_bg_vignette;
static int    g_bg_w, g_bg_h;

static int background_cache(void)
{
    const double cx = theme_w * 0.5, cy = theme_h * 0.52;
    size_t n = (size_t)theme_w * theme_h;
    int x, y;

    if (g_bg_dist && g_bg_w == theme_w && g_bg_h == theme_h)
        return 1;
    free(g_bg_dist);
    free(g_bg_vignette);
    g_bg_dist = malloc(n * sizeof *g_bg_dist);
    g_bg_vignette = malloc(n * sizeof *g_bg_vignette);
    if (!g_bg_dist || !g_bg_vignette) {
        free(g_bg_dist);
        free(g_bg_vignette);
        g_bg_dist = g_bg_vignette = NULL;
        return 0;
    }
    for (y = 0; y < theme_h; y++)
        for (x = 0; x < theme_w; x++) {
            double dx = x - cx, dy = (y - cy) * 1.25;
            double nx = (x - cx) / (theme_w * 0.5), ny = (y - cy) / (theme_h * 0.5);
            size_t i = (size_t)y * theme_w + x;

            g_bg_dist[i] = (float)sqrt(dx * dx + dy * dy);
            g_bg_vignette[i] =
                (float)(clamp01((sqrt(nx * nx + ny * ny) - 0.75) / 0.75) * 0.85);
        }
    g_bg_w = theme_w;
    g_bg_h = theme_h;
    return 1;
}

void theme_background(double pulse)
{
    /* Two glows behind the content, the larger one breathing. The
     * dashboard's light came from behind and below; this is that, not a
     * copy of it. */
    const double big = theme_w * (0.62 + 0.03 * sin(pulse * 6.2831853));
    const double small = theme_w * 0.26;
    size_t i, n;

    if (!theme_px || !background_cache())
        return;

    n = (size_t)theme_w * theme_h;
    for (i = 0; i < n; i++) {
        double d = g_bg_dist[i];
        double a = clamp01(1.0 - d / big);
        double b = clamp01(1.0 - d / small);
        double v = a * a * 0.22 + b * b * 0.28;
        double k = g_bg_vignette[i];
        int r = (int)(0x18 + v * 0x30);
        int g = (int)(0x1E + v * 0xB0);
        int bl = (int)(0x12 + v * 0x22);

        /* A vignette, so the edges fall away and the eye goes to the
         * middle: black over it, as blend() would put it. */
        if (k > 0) {
            r = (int)(r - r * k);
            g = (int)(g - g * k);
            bl = (int)(bl - bl * k);
        }
        theme_px[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
    }
}

/* Distance from a point to a rounded rectangle: negative inside,
 * positive outside, and the magnitude is how far. Soft edges and the
 * glow around them both fall out of it. */
static double rrect_distance(double px, double py, ThemeRect r, double radius)
{
    double hx = r.w * 0.5, hy = r.h * 0.5;
    double cx = r.x + hx, cy = r.y + hy;
    double dx = fabs(px - cx) - (hx - radius);
    double dy = fabs(py - cy) - (hy - radius);
    double ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
    double inside = (dx > dy ? dx : dy);

    if (inside > 0)
        inside = 0;
    return sqrt(ox * ox + oy * oy) + inside - radius;
}

void theme_panel(ThemeRect r, double lit)
{
    const double radius = 10.0, s = theme_scale;
    double falloff, strength, reach;
    int pad, x0, y0, x1, y1, x, y;

    if (!theme_px)
        return;
    lit = clamp01(lit);
    falloff = 4.0 + 7.0 * lit;
    strength = 0.07 + 0.42 * lit;

    /* Room for the glow to reach nothing. Too little and it is cut off
     * square, which reads as a box drawn round the selected row rather
     * than light coming off it -- the falloff below has to have faded
     * to nothing by the time it reaches this edge. Nothing is where the
     * glow drops under 1/255, which blend() rounds to no change at all:
     * 52 for a panel fully lit, and far less for one at rest, which is
     * most of them. */
    reach = falloff * log(strength * 255.0) + 2.0;
    pad = reach < 52.0 ? (int)ceil(reach) : 52;

    x0 = (int)floor((r.x - pad) * s);
    y0 = (int)floor((r.y - pad) * s);
    x1 = (int)ceil((r.x + r.w + pad) * s);
    y1 = (int)ceil((r.y + r.h + pad) * s);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > theme_w) x1 = theme_w;
    if (y1 > theme_h) y1 = theme_h;

    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            /* In window units, from the pixel's centre. */
            double d = rrect_distance((x + 0.5) / s, (y + 0.5) / s, r, radius);

            if (d < 0) {
                /* Inside: a dark slab that greens as it lights up. */
                double edge = clamp01(-d * s);        /* antialias the rim */
                double fill = 0.55 + 0.20 * lit;

                blend(x, y, (int)(0x0A + 0x18 * lit),
                            (int)(0x12 + 0x3C * lit),
                            (int)(0x08 + 0x0E * lit), fill * edge);
            } else {
                /* Outside: the glow, which only exists when lit. */
                double g = exp(-d / falloff) * strength;

                blend(x, y, 0x9B, 0xCE, 0x1E, g);
            }
            /* The rim itself, brightest right on the boundary. */
            if (d > -2.0 && d < 1.0) {
                double rim = 1.0 - fabs(d + 0.5) / 1.5;

                if (rim > 0)
                    blend(x, y, 0x9B, 0xCE, 0x1E, rim * (0.25 + 0.6 * lit));
            }
        }
    }
}

void theme_bar(ThemeRect r, double fraction, double phase)
{
    const double s = theme_scale, radius = r.h / 2.0;
    double fill_to = r.x + r.w * clamp01(fraction);
    /* The highlight: a soft band a fifth of the bar wide, entering at the
     * left edge and leaving past the right one. */
    const double band = r.w * 0.2;
    double centre = r.x - band + (r.w + 2 * band) * (phase < 0 ? 0 : phase - floor(phase));
    int x0, y0, x1, y1, x, y;

    if (!theme_px)
        return;
    x0 = (int)floor(r.x * s);
    y0 = (int)floor(r.y * s);
    x1 = (int)ceil((r.x + r.w) * s);
    y1 = (int)ceil((r.y + r.h) * s);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > theme_w) x1 = theme_w;
    if (y1 > theme_h) y1 = theme_h;
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            double px = (x + 0.5) / s, py = (y + 0.5) / s;
            double d = rrect_distance(px, py, r, radius);
            double edge = clamp01(-d * s);

            if (edge <= 0)
                continue;
            double u = (px - centre) / band;
            double glow = phase < 0 ? 0 : exp(-u * u * 4.0);

            if (px < fill_to) {
                /* Brighter along the top, as the panels' rims are. */
                double t = (py - r.y) / (double)r.h;

                blend(x, y, (int)(0x9B - 0x30 * t), (int)(0xCE - 0x30 * t),
                            (int)(0x1E - 0x0A * t), edge);
                blend(x, y, 0xE4, 0xF5, 0xB0, 0.45 * glow * edge);
            } else {
                blend(x, y, 0x10, 0x1A, 0x0C, 0.85 * edge);
                blend(x, y, 0x9B, 0xCE, 0x1E, 0.12 * glow * edge);
            }
        }
    }
}

/* One window-unit row of the underline, `row` below its top: every pixel
 * row in it, faded at the ends. */
static void underline_row(int x0, int x1, int y, int row, double alpha)
{
    const double s = theme_scale;
    int py0 = (int)floor((y + row) * s), py1 = (int)floor((y + row + 1) * s);
    int px0 = (int)floor(x0 * s), px1 = (int)floor(x1 * s);
    int px, py;

    for (px = px0; px < px1; px++) {
        double u = px / s;                      /* window units */
        double edge = 1.0, run = u - x0, left = x1 - u;

        if (run < 10) edge = run / 10.0;        /* fade the ends */
        if (left < 10) edge = left / 10.0;
        for (py = py0; py < py1; py++)
            blend(px, py, 0x9B, 0xCE, 0x1E, alpha * edge);
    }
}

void theme_tab_underline(ThemeRect from, ThemeRect to, double t)
{
    double e = clamp01(t);
    int x0, x1;

    /* Ease, so it settles rather than stopping dead. */
    e = e * e * (3.0 - 2.0 * e);
    x0 = (int)(from.x + (to.x - from.x) * e);
    x1 = x0 + (int)(from.w + (to.w - from.w) * e);

    /* Drawn rather than panelled: a panel brings a panel's glow, and a
     * three pixel bar wearing a twenty-six pixel halo sits under the
     * whole strip instead of under one tab. */
    underline_row(x0, x1, from.y,  0, 0.95);
    underline_row(x0, x1, from.y,  1, 0.95);
    underline_row(x0, x1, from.y,  2, 0.45);
    underline_row(x0, x1, from.y,  3, 0.18);
    underline_row(x0, x1, from.y, -1, 0.25);
}

void theme_arrows(ThemeRect r, int can_left, int can_right, double lit)
{
    const double s = theme_scale;
    const int size = (int)(7 * s);
    const double a = 0.25 + 0.65 * lit;
    int cy = (int)((r.y + r.h / 2) * s);
    int left = (int)(r.x * s), right = (int)((r.x + r.w) * s) - 1;
    int i;

    for (i = 0; i < size; i++) {
        int run = i + 1;                 /* tip at the outside edge */
        int j;

        for (j = -run / 2; j <= run / 2; j++) {
            if (can_left)
                blend(left + i, cy + j, 0x9B, 0xCE, 0x1E, a);
            if (can_right)
                blend(right - i, cy + j, 0x9B, 0xCE, 0x1E, a);
        }
    }
}
