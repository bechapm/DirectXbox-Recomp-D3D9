/*
 * launcher_theme_freetype.c -- the Linux half of launcher_theme.h: the back
 * buffer, plain memory, and the text, which FreeType rasterises and this
 * file blends into the same pixels the shapes are written into
 * (launcher_theme.c). launcher_sdl.c puts the buffer on screen.
 *
 * The buffer is the display's own resolution, theme_scale pixels to the
 * window unit, and glyphs are rasterised at that scale, so text is laid out
 * in window units like everything else and drawn at full resolution.
 *
 * The face is the desktop's own sans, as fontconfig names it (Noto Sans on
 * SteamOS, DejaVu Sans on many others), at the size and weight GDI is asked
 * for: a point size at 96 dots an inch, 400 normal, 600 and up bold. Text is
 * laid out left to right one code point at a time, with no shaping, which
 * is all the launcher's Latin strings need.
 *
 * The launcher redraws every string sixty times a second, so each glyph is
 * rasterised once per size and kept (glyph_get).
 */
#include "launcher_theme.h"
#include "launcher_theme_buf.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double g_want_scale = 1.0;

void theme_set_scale(double scale)
{
    g_want_scale = scale > 0.0 ? scale : 1.0;
}

/* The buffer itself: launcher_sdl.c copies it to the screen. */
void *theme_image(void)
{
    return theme_px;
}

int theme_begin(int width, int height)
{
    int pw = (int)lround(width * g_want_scale);
    int ph = (int)lround(height * g_want_scale);

    if (theme_px && pw == theme_w && ph == theme_h)
        return 1;
    free(theme_px);
    theme_px = calloc((size_t)pw * ph, sizeof *theme_px);
    if (!theme_px) {
        theme_w = theme_h = 0;
        return 0;
    }
    theme_w = pw;
    theme_h = ph;
    theme_scale = (double)pw / width;
    return 1;
}

/* --------------------------------------------------------------- fonts */

static FT_Library g_ft;
static FT_Face    g_face[2];            /* regular, bold */
static int        g_font_tried;

/* The file fontconfig chooses for the desktop's sans at a weight. */
static FT_Face open_sans(int bold)
{
    FcPattern *pat, *match;
    FcResult res;
    FcChar8 *file = NULL;
    int index = 0;
    FT_Face face = NULL;

    pat = FcNameParse((const FcChar8 *)"sans-serif");
    if (!pat)
        return NULL;
    FcPatternAddInteger(pat, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    match = FcFontMatch(NULL, pat, &res);
    if (match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
        FcPatternGetInteger(match, FC_INDEX, 0, &index);
        if (FT_New_Face(g_ft, (const char *)file, index, &face) != 0)
            face = NULL;
    }
    if (!face)
        fprintf(stderr, "launcher: no %s sans-serif font from fontconfig\n",
                bold ? "bold" : "regular");
    if (match)
        FcPatternDestroy(match);
    FcPatternDestroy(pat);
    return face;
}

static int fonts_ready(void)
{
    if (!g_font_tried) {
        g_font_tried = 1;
        if (FT_Init_FreeType(&g_ft) != 0) {
            fprintf(stderr, "launcher: FreeType did not start\n");
            return 0;
        }
        FcInit();
        g_face[0] = open_sans(0);
        g_face[1] = open_sans(1);
        if (!g_face[1])
            g_face[1] = g_face[0];      /* bold drawn regular, rather than not at all */
    }
    return g_face[0] != NULL;
}

/* ---------------------------------------------------------- glyph cache */

typedef struct Glyph {
    uint32_t cp;
    uint16_t px;                        /* pixel size it was rasterised at */
    uint8_t  bold, used;
    int      left, top;                 /* bitmap offset from the pen, y up */
    int      w, h;
    int      advance;                   /* 26.6 */
    uint8_t *bits;                      /* w * h coverage */
} Glyph;

#define GLYPH_SLOTS 4096                /* a power of two */
static Glyph g_glyphs[GLYPH_SLOTS];
static int   g_glyph_count;

static void glyphs_clear(void)
{
    int i;

    for (i = 0; i < GLYPH_SLOTS; i++)
        free(g_glyphs[i].bits);
    memset(g_glyphs, 0, sizeof g_glyphs);
    g_glyph_count = 0;
}

static FT_Face face_at(int bold, int px)
{
    FT_Face f = g_face[bold];

    if (f && f->size && f->size->metrics.y_ppem != px)
        FT_Set_Pixel_Sizes(f, 0, (FT_UInt)px);
    return f;
}

static const Glyph *glyph_get(uint32_t cp, int bold, int px)
{
    uint32_t h = (cp * 2654435761u) ^ (uint32_t)(px * 40503) ^ (uint32_t)bold;
    FT_Face f;
    FT_GlyphSlot s;
    Glyph *g;
    int i;

    for (i = 0; i < GLYPH_SLOTS; i++) {
        g = &g_glyphs[(h + i) & (GLYPH_SLOTS - 1)];
        if (!g->used)
            break;
        if (g->cp == cp && g->px == px && g->bold == bold)
            return g;
    }
    /* Full, which a launcher only gets to by being resized through every
     * scale there is: start again. */
    if (g_glyph_count >= GLYPH_SLOTS * 3 / 4) {
        glyphs_clear();
        g = &g_glyphs[h & (GLYPH_SLOTS - 1)];
    }
    f = face_at(bold, px);
    if (!f || FT_Load_Char(f, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0)
        return NULL;
    s = f->glyph;
    g->cp = cp;
    g->px = (uint16_t)px;
    g->bold = (uint8_t)bold;
    g->used = 1;
    g->left = s->bitmap_left;
    g->top = s->bitmap_top;
    g->w = (int)s->bitmap.width;
    g->h = (int)s->bitmap.rows;
    g->advance = (int)s->advance.x;
    g->bits = NULL;
    if (g->w > 0 && g->h > 0 && s->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
        int y;

        g->bits = malloc((size_t)g->w * g->h);
        if (g->bits)
            for (y = 0; y < g->h; y++)
                memcpy(g->bits + (size_t)y * g->w,
                       s->bitmap.buffer + (ptrdiff_t)y * s->bitmap.pitch, (size_t)g->w);
    }
    g_glyph_count++;
    return g;
}

void theme_shutdown(void)
{
    free(theme_px);
    theme_px = NULL;
    theme_w = theme_h = 0;
    glyphs_clear();
}

/* ---------------------------------------------------------------- text */

/* The next code point of UTF-8 text; a malformed byte is taken as Latin-1,
 * which is what an old settings file's text would be. */
static uint32_t next_cp(const unsigned char **p)
{
    const unsigned char *s = *p;
    uint32_t c = s[0];
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0, i;

    for (i = 1; i <= n; i++)
        if ((s[i] & 0xC0) != 0x80)
            n = 0;
    if (n == 0) {
        *p = s + 1;
        return c;
    }
    c &= 0x3F >> n;
    for (i = 1; i <= n; i++)
        c = (c << 6) | (s[i] & 0x3F);
    *p = s + 1 + n;
    return c;
}

/* The pixel size GDI would use: points at 96 dots an inch, at the scale. */
static int px_for(int size)
{
    int px = (int)lround(size * 96.0 / 72.0 * theme_scale);
    return px < 1 ? 1 : px;
}

/* Width in 26.6 of text[0..len), on one line. */
static long measure(const char *text, size_t len, int bold, int px)
{
    const unsigned char *p = (const unsigned char *)text, *end = p + len;
    long w = 0;

    while (p < end) {
        const Glyph *g = glyph_get(next_cp(&p), bold, px);
        if (g)
            w += g->advance;
    }
    return w;
}

/* Blend one glyph into the buffer at pen (x, baseline), in pixels, inside
 * the clip [cx0, cx1) x [cy0, cy1). */
static void blit(const Glyph *g, int x, int baseline, COLORREF colour,
                 int cx0, int cy0, int cx1, int cy1)
{
    int r = colour & 0xFF, gr = (colour >> 8) & 0xFF, b = (colour >> 16) & 0xFF;
    int x0 = x + g->left, y0 = baseline - g->top, i, j;

    if (!g->bits)
        return;
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 > theme_w) cx1 = theme_w;
    if (cy1 > theme_h) cy1 = theme_h;
    for (j = 0; j < g->h; j++) {
        int y = y0 + j;
        uint32_t *row;

        if (y < cy0 || y >= cy1)
            continue;
        row = theme_px + (size_t)y * theme_w;
        for (i = 0; i < g->w; i++) {
            int xx = x0 + i, a = g->bits[(size_t)j * g->w + i];
            uint32_t d;
            int dr, dg, db;

            if (!a || xx < cx0 || xx >= cx1)
                continue;
            d = row[xx];
            dr = (d >> 16) & 0xFF;
            dg = (d >> 8) & 0xFF;
            db = d & 0xFF;
            dr += (r - dr) * a / 255;
            dg += (gr - dg) * a / 255;
            db += (b - db) * a / 255;
            row[xx] = ((uint32_t)dr << 16) | ((uint32_t)dg << 8) | (uint32_t)db;
        }
    }
}

/* Draw text[0..len) with its pen starting at x (26.6) on `baseline`. */
static void draw_run(const char *text, size_t len, long x, int baseline, int bold, int px,
                     COLORREF colour, int cx0, int cy0, int cx1, int cy1)
{
    const unsigned char *p = (const unsigned char *)text, *end = p + len;

    while (p < end) {
        const Glyph *g = glyph_get(next_cp(&p), bold, px);
        if (!g)
            continue;
        blit(g, (int)((x + 32) >> 6), baseline, colour, cx0, cy0, cx1, cy1);
        x += g->advance;
    }
}

/* A window-unit rectangle in pixels. */
static void px_rect(ThemeRect r, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = (int)lround(r.x * theme_scale);
    *y0 = (int)lround(r.y * theme_scale);
    *x1 = (int)lround((r.x + r.w) * theme_scale);
    *y1 = (int)lround((r.y + r.h) * theme_scale);
}

void theme_text(ThemeRect r, const char *text, int size, int weight,
                COLORREF colour, ThemeAlign align)
{
    int bold = weight >= 600, px, x0, y0, x1, y1, asc, desc, baseline;
    long width, x;
    FT_Face f;

    if (!theme_px || !text || !*text || !fonts_ready())
        return;
    px = px_for(size);
    if (!(f = face_at(bold, px)))
        return;
    px_rect(r, &x0, &y0, &x1, &y1);
    width = measure(text, strlen(text), bold, px);
    x = (long)x0 << 6;
    if (align == THEME_CENTRE)
        x += (((long)(x1 - x0) << 6) - width) / 2;
    else if (align == THEME_RIGHT)
        x += ((long)(x1 - x0) << 6) - width;
    /* Centred on the line's height, as GDI's DT_VCENTER does. */
    asc = (int)(f->size->metrics.ascender >> 6);
    desc = (int)(-f->size->metrics.descender >> 6);
    baseline = y0 + ((y1 - y0) - (asc + desc)) / 2 + asc;
    draw_run(text, strlen(text), x, baseline, bold, px, colour, x0, y0, x1, y1);
}

void theme_text_wrapped(ThemeRect r, const char *text, int size, COLORREF colour)
{
    int px, x0, y0, x1, y1, asc, line_h, y;
    long room;
    const char *p;
    FT_Face f;

    if (!theme_px || !text || !*text || !fonts_ready())
        return;
    px = px_for(size);
    if (!(f = face_at(0, px)))
        return;
    px_rect(r, &x0, &y0, &x1, &y1);
    room = (long)(x1 - x0) << 6;
    asc = (int)(f->size->metrics.ascender >> 6);
    line_h = (int)(f->size->metrics.height >> 6);

    /* Word-wrapped and top aligned, a newline starting a line, and a line
     * that does not fit in the rectangle left out: GDI's
     * DT_WORDBREAK | DT_TOP, and what CoreText's frame does. */
    p = text;
    for (y = y0; *p && y + line_h <= y1; y += line_h) {
        const char *end = p, *fit = NULL, *q;

        /* The longest run of whole words that fits; a single word too
         * long for the line is broken where it stops fitting. */
        while (*end && *end != '\n') {
            q = end;
            while (*q == ' ')
                q++;
            while (*q && *q != ' ' && *q != '\n')
                q++;
            if (measure(p, (size_t)(q - p), 0, px) > room)
                break;
            fit = end = q;
        }
        if (!fit) {
            if (*end == '\n' || !*end)
                fit = end;
            else {
                const unsigned char *u = (const unsigned char *)p;
                fit = p;
                while (*u && *u != '\n' && *u != ' ') {
                    const unsigned char *n = u;
                    next_cp(&n);
                    if (measure(p, (size_t)((const char *)n - p), 0, px) > room && fit > p)
                        break;
                    u = n;
                    fit = (const char *)u;
                }
            }
        }
        draw_run(p, (size_t)(fit - p), (long)x0 << 6, y + asc, 0, px, colour,
                 x0, y0, x1, y1);
        p = fit;
        while (*p == ' ')
            p++;
        if (*p == '\n')
            p++;
    }
}
