/*
 * d3d8_overlay.c -- a few lines of text over the finished frame.
 *
 * See d3d8_overlay.h for what this touches on the device and what it
 * deliberately does not. The short version: no sampler, no vertex buffer, no
 * input layout worth restoring, and the render target and viewport are put
 * back, so a title's own drawing is unaffected.
 *
 * The text is drawn by GDI into a 32-bit DIB and uploaded to a texture when
 * it changes, which is roughly twice a second for a frame-rate counter. A
 * real font at a real size costs less code than an embedded glyph table and
 * reads better at any window size. The bitmap and the font are made at the
 * picture's own scale (a 1440-line picture gets a font three times the
 * 480-line size), so the text is drawn one texel to one pixel: it used to be
 * drawn for a 480-line frame and blown up, which made it blocky.
 *
 * Off Windows there is no GDI, so the same bitmap is filled from the public
 * domain 8x8 IBM VGA glyphs (as SDL3's debug text carries them), drawn at
 * twice their size times the scale. Everything after the bitmap is shared.
 */
#include "d3d8_internal.h"
#include "d3d8_overlay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Up to OVERLAY_LINES lines ('\n' between them), each OVERLAY_LINE_H
 * pixels: one line is the 28-pixel strip the frame-rate counter always was.
 * All of these are in 480-line pixels; the bitmap is OVERLAY_W x OVERLAY_H
 * times the picture's scale (g.scale). */
#define OVERLAY_W         640
#define OVERLAY_LINES      18
#define OVERLAY_LINE_H     22
#define OVERLAY_TOP         4
#define OVERLAY_H        (6 + OVERLAY_LINES * OVERLAY_LINE_H)
#define OVERLAY_PAD    8      /* from the picture's top-left corner */
#define OVERLAY_FONT_H 18     /* the font's character height */
#define OVERLAY_MAX_SCALE 6   /* 2880 lines; a bigger picture keeps this size */

static struct {
    int             tried;     /* creation attempted */
    int             failed;    /* and gave up: never try again */
    RhiImage       *texture;
    RhiView        *srv;
    RhiBuffer      *cb;
    RhiShader      *vs;
    RhiShader      *ps;
    RhiBlendState  *blend;
    RhiDepthState  *depth;
    RhiRasterState *raster;
#if defined(_WIN32)
    HDC             dc;
    HBITMAP         bitmap;
    HFONT           font;
#endif
    void           *bits;      /* the DIB's pixels, BGRA top-down */
    int             scale;     /* the bitmap's size over 480-line pixels; 0 none */
    int             w, h;      /* the bitmap's size, OVERLAY_W/H times scale */
    int             text_w;    /* what the last render measured, in bitmap pixels */
    int             text_h;
    char            text[1536];
} g;

/* b7 in both stages: the four texture stages a title can use are b0..b3 and
 * the vertex layers take the low slots. x,y,w,h are in clip space. */
typedef struct {
    float x, y, w, h;
    float tex_w, tex_h, pad0, pad1;
} OverlayConstants;

static const char kShaderSource[] =
    /* Direct3D 9 (rhi_d3d9.c): b7 is the vertex stage's c216, the vertex
     * number comes from a stream, and the glyphs are fetched with the
     * sampler's default point filter at the texel, which size.zw (the
     * texture's own size) turns the coordinate into. */
    "#ifdef RHI_D3D9\n"
    "float4 rect : register(c216);\n"
    "float4 size : register(c217);\n"
    "struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
    "VSOut vs_main(float id : RHI_VERTEX_ID) {\n"
    "    float2 c = float2(fmod(id, 2.0), floor(id * 0.5));\n"
    "    VSOut o;\n"
    "    o.pos = float4(rect.x + c.x * rect.z, rect.y - c.y * rect.w, 0, 1);\n"
    "    o.uv  = c * size.xy / size.zw;\n"
    "    RHI_VP_ADJUST(o.pos);\n"
    "    return o;\n"
    "}\n"
    "sampler2D tex : register(s8);\n"
    "float4 ps_main(float2 uv : TEXCOORD0) : COLOR0 {\n"
    "    float a = tex2D(tex, uv).r;\n"
    "    float3 rgb = lerp(float3(0, 0, 0), float3(1, 1, 1), a);\n"
    "    return float4(rgb, max(a, 0.45));\n"
    "}\n"
    "#else\n"
    "cbuffer Overlay : register(b7) {\n"
    "    float4 rect;        // x, y, w, h in clip space\n"
    "    float4 size;        // texture width, height\n"
    "};\n"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut vs_main(uint id : SV_VertexID) {\n"
    "    float2 c = float2(id & 1, id >> 1);\n"   /* 0,0  1,0  0,1  1,1 */
    "    VSOut o;\n"
    "    o.pos = float4(rect.x + c.x * rect.z, rect.y - c.y * rect.w, 0, 1);\n"
    "    o.uv  = c * size.xy;\n"
    "    return o;\n"
    "}\n"
    "Texture2D<float4> tex : register(t8);\n"
    "float4 ps_main(VSOut i) : SV_Target {\n"
    "    int2 p = int2(i.uv);\n"
    "    float a = tex.Load(int3(p, 0)).r;\n"
    /* The glyphs are white on black, so the red channel is coverage. A dark
     * panel under them keeps the text readable over a bright frame. */
    "    float3 rgb = lerp(float3(0, 0, 0), float3(1, 1, 1), a);\n"
    "    return float4(rgb, max(a, 0.45));\n"
    "}\n"
    "#endif\n";

#if !defined(_WIN32)
/* ASCII 33..126, eight rows each, the least significant bit leftmost.
 * Marcel Sondaar's font8_8 from the IBM VGA fonts, public domain. */
static const uint8_t kGlyphs[94 * 8] = {
    0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00,  /* '!' */
    0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* '"' */
    0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00,  /* '#' */
    0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00,  /* '$' */
    0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00,  /* '%' */
    0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00,  /* '&' */
    0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,  /* "'" */
    0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00,  /* '(' */
    0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00,  /* ')' */
    0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00,  /* '*' (star) */
    0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00,  /* '+' */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06,  /* ',' */
    0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00,  /* '-' */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00,  /* '.' */
    0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00,  /* slash */
    0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00,  /* '0' */
    0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00,  /* '1' */
    0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00,  /* '2' */
    0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00,  /* '3' */
    0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00,  /* '4' */
    0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00,  /* '5' */
    0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00,  /* '6' */
    0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00,  /* '7' */
    0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00,  /* '8' */
    0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00,  /* '9' */
    0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00,  /* ':' */
    0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06,  /* ';' */
    0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00,  /* '<' */
    0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00,  /* '=' */
    0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00,  /* '>' */
    0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00,  /* '?' */
    0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00,  /* '@' */
    0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00,  /* 'A' */
    0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00,  /* 'B' */
    0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00,  /* 'C' */
    0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00,  /* 'D' */
    0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00,  /* 'E' */
    0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00,  /* 'F' */
    0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00,  /* 'G' */
    0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00,  /* 'H' */
    0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00,  /* 'I' */
    0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00,  /* 'J' */
    0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00,  /* 'K' */
    0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00,  /* 'L' */
    0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00,  /* 'M' */
    0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00,  /* 'N' */
    0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00,  /* 'O' */
    0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00,  /* 'P' */
    0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00,  /* 'Q' */
    0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00,  /* 'R' */
    0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00,  /* 'S' */
    0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00,  /* 'T' */
    0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00,  /* 'U' */
    0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00,  /* 'V' */
    0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00,  /* 'W' */
    0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00,  /* 'X' */
    0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00,  /* 'Y' */
    0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00,  /* 'Z' */
    0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00,  /* '[' */
    0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00,  /* '\\' */
    0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00,  /* ']' */
    0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00,  /* '^' */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF,  /* '_' */
    0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00,  /* '`' */
    0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00,  /* 'a' */
    0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00,  /* 'b' */
    0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00,  /* 'c' */
    0x38, 0x30, 0x30, 0x3E, 0x33, 0x33, 0x6E, 0x00,  /* 'd' */
    0x00, 0x00, 0x1E, 0x33, 0x3F, 0x03, 0x1E, 0x00,  /* 'e' */
    0x1C, 0x36, 0x06, 0x0F, 0x06, 0x06, 0x0F, 0x00,  /* 'f' */
    0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F,  /* 'g' */
    0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00,  /* 'h' */
    0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00,  /* 'i' */
    0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E,  /* 'j' */
    0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00,  /* 'k' */
    0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00,  /* 'l' */
    0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00,  /* 'm' */
    0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00,  /* 'n' */
    0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00,  /* 'o' */
    0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F,  /* 'p' */
    0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78,  /* 'q' */
    0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00,  /* 'r' */
    0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00,  /* 's' */
    0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00,  /* 't' */
    0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00,  /* 'u' */
    0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00,  /* 'v' */
    0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00,  /* 'w' */
    0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00,  /* 'x' */
    0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F,  /* 'y' */
    0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00,  /* 'z' */
    0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00,  /* '{' */
    0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00,  /* '|' */
    0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00,  /* '}' */
    0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* '~' */
};

/* Fill the bitmap (w x h) with `text`, white on black, each glyph at 2x
 * times s: 14 pixels across (the glyphs leave their eighth column blank) and
 * 16 down, a line every OVERLAY_LINE_H, all times s. Returns the widest line
 * drawn, padding included; *lines_out the number of lines. */
static int glyph_render(uint32_t *bits, int w, int h, int s,
                        const char *text, int len, int *lines_out)
{
    const int scale = 2 * s, advance = 7 * scale, left = 6 * s, line_h = OVERLAY_LINE_H * s;
    const int first = OVERLAY_TOP * s + (line_h - 8 * scale) / 2;
    int i, row, col, x = left, line = 0, widest = 0;

    for (i = 0; i < w * h; i++)
        bits[i] = 0xFF000000u;                  /* opaque black, as ETO_OPAQUE */
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        const int top = first + line * line_h;
        const uint8_t *g;

        if (c == '\n') {
            if (x > widest) widest = x;
            if (++line >= OVERLAY_LINES) break;
            x = left;
            continue;
        }
        if (x + advance > w)
            continue;                           /* past the edge: clipped */
        x += advance;
        if (c < 33 || c > 126)
            continue;                           /* space, or nothing to draw */
        g = &kGlyphs[(c - 33) * 8];
        for (row = 0; row < 8 * scale; row++)
            for (col = 0; col < 8 * scale; col++)
                if (g[row / scale] & (1u << (col / scale)) && x - advance + col < w)
                    bits[(top + row) * w + x - advance + col] = 0xFFFFFFFFu;
    }
    if (x > widest) widest = x;
    *lines_out = line < OVERLAY_LINES ? line + 1 : OVERLAY_LINES;
    return widest + left > w ? w : widest + left;
}
#endif

static void overlay_fail(const char *what)
{
    g.failed = 1;
    fprintf(stderr, "D3D8 overlay: %s failed; the overlay is off for "
            "this run\n", what);
    fflush(stderr);
    d3d8_overlay_shutdown();
}

static RhiShader *compile_one(uint32_t stage, const char *entry, const char *target)
{
    RhiShaderSource src;
    RhiShader *s;
    char err[2048];

    memset(&src, 0, sizeof src);
    src.hlsl = kShaderSource;
    src.len = sizeof kShaderSource - 1;
    src.name = "overlay";
    src.entry = entry;
    src.target = target;
    s = rhi_shader_create(stage, &src, err, sizeof err);
    if (!s && err[0])
        fprintf(stderr, "D3D8 overlay: %s\n", err);
    return s;
}

/* The pieces whose size follows the picture: the texture, and the bitmap and
 * font the text is drawn with. */
static void overlay_release_sized(void)
{
    rhi_view_destroy(g.srv);             g.srv = NULL;
    rhi_image_destroy(g.texture);        g.texture = NULL;
#if defined(_WIN32)
    if (g.bitmap)  { DeleteObject(g.bitmap);                     g.bitmap = NULL; }
    if (g.font)    { DeleteObject(g.font);                       g.font = NULL; }
    if (g.dc)      { DeleteDC(g.dc);                             g.dc = NULL; }
#else
    free(g.bits);
#endif
    g.bits = NULL;
    g.scale = g.w = g.h = 0;
    g.text[0] = '\0';                   /* drawn again at the new size */
    g.text_w = 0;
}

static int overlay_size(int scale)
{
    RhiImageDesc td;
#if defined(_WIN32)
    BITMAPINFO bi;
#endif

    if (g.scale == scale)
        return 1;
    overlay_release_sized();
    g.w = OVERLAY_W * scale;
    g.h = OVERLAY_H * scale;

    memset(&td, 0, sizeof td);
    td.type = RHI_IMAGE_2D;
    td.width = (uint32_t)g.w;
    td.height = (uint32_t)g.h;
    td.depth = 1;
    td.mip_levels = 1;
    td.format = RHI_FORMAT_B8G8R8A8_UNORM;
    td.samples = 1;
    td.usage = RHI_USAGE_DYNAMIC;
    td.bind = RHI_BIND_SAMPLED;
    td.cpu_access = RHI_CPU_WRITE;
    if (!(g.texture = rhi_image_create(&td, NULL))) { overlay_fail("CreateTexture2D"); return 0; }
    if (!(g.srv = rhi_view_create(g.texture, RHI_VIEW_SAMPLED, NULL))) {
        overlay_fail("CreateShaderResourceView");
        return 0;
    }

#if defined(_WIN32)
    /* GDI side: a top-down 32-bit DIB the text is drawn into. */
    g.dc = CreateCompatibleDC(NULL);
    if (!g.dc) { overlay_fail("CreateCompatibleDC"); return 0; }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = g.w;
    bi.bmiHeader.biHeight = -g.h;                /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g.bitmap = CreateDIBSection(g.dc, &bi, DIB_RGB_COLORS, &g.bits, NULL, 0);
    if (!g.bitmap) { overlay_fail("CreateDIBSection"); return 0; }
    SelectObject(g.dc, g.bitmap);
    /* Greyscale antialiasing, not ClearType: the shader reads one channel as
     * coverage, and ClearType's per-channel edges would come out as a blur
     * on one side of every stem. */
    g.font = CreateFontW(-OVERLAY_FONT_H * scale, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                         ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (g.font) SelectObject(g.dc, g.font);
    SetBkMode(g.dc, OPAQUE);
    SetBkColor(g.dc, RGB(0, 0, 0));
    SetTextColor(g.dc, RGB(255, 255, 255));
#else
    g.bits = calloc((size_t)g.w * (size_t)g.h, 4);
    if (!g.bits) { overlay_fail("the text bitmap"); return 0; }
#endif
    g.scale = scale;
    return 1;
}

static int overlay_create(void)
{
    RhiBufferDesc bd;
    RhiBlendDesc bl;
    RhiDepthDesc ds;
    RhiRasterDesc rd;

    if (g.failed) return 0;
    if (g.tried) return g.cb != NULL;
    g.tried = 1;
    if (!rhi_device_ready()) { g.failed = 1; return 0; }

    memset(&bd, 0, sizeof bd);
    bd.size = sizeof(OverlayConstants);
    bd.usage = RHI_USAGE_DYNAMIC;
    bd.bind = RHI_BIND_UNIFORM;
    bd.cpu_access = RHI_CPU_WRITE;
    if (!(g.cb = rhi_buffer_create(&bd, NULL))) { overlay_fail("CreateBuffer"); return 0; }

    if (!(g.vs = compile_one(RHI_STAGE_VERTEX, "vs_main", "vs_4_0"))) {
        overlay_fail("the vertex shader");
        return 0;
    }
    if (!(g.ps = compile_one(RHI_STAGE_PIXEL, "ps_main", "ps_4_0"))) {
        overlay_fail("the pixel shader");
        return 0;
    }

    memset(&bl, 0, sizeof bl);
    bl.enable = 1;
    bl.src = RHI_BLEND_SRC_ALPHA;
    bl.dst = RHI_BLEND_INV_SRC_ALPHA;
    bl.op = RHI_BLEND_OP_ADD;
    bl.src_alpha = RHI_BLEND_ONE;
    bl.dst_alpha = RHI_BLEND_INV_SRC_ALPHA;
    bl.op_alpha = RHI_BLEND_OP_ADD;
    bl.write_mask = RHI_WRITE_ALL;
    if (!(g.blend = rhi_blend_state_create(&bl))) { overlay_fail("CreateBlendState"); return 0; }

    memset(&ds, 0, sizeof ds);
    if (!(g.depth = rhi_depth_state_create(&ds))) { overlay_fail("CreateDepthStencilState"); return 0; }

    memset(&rd, 0, sizeof rd);
    rd.fill = RHI_FILL_SOLID;
    rd.cull = RHI_CULL_NONE;
    rd.depth_clip = 1;
    if (!(g.raster = rhi_raster_state_create(&rd))) { overlay_fail("CreateRasterizerState"); return 0; }
    return 1;
}

/* Draw the text into the DIB and upload it. Only when it has changed. */
static int overlay_set_text(const char *text)
{
#if defined(_WIN32)
    wchar_t wide[1536];
    RECT rect;
    SIZE extent;
    int i, start;
    const int line_h = OVERLAY_LINE_H * g.scale, top = OVERLAY_TOP * g.scale;
#endif
    const int s = g.scale;
    int len, lines = 0;

    if (strcmp(text, g.text) == 0)
        return g.text_w > 0;

    len = (int)strlen(text);
    if (len > (int)(sizeof g.text - 1)) len = (int)(sizeof g.text - 1);
    memcpy(g.text, text, (size_t)len);
    g.text[len] = '\0';
#if defined(_WIN32)
    for (i = 0; i < len; i++)
        wide[i] = (wchar_t)(unsigned char)g.text[i];

    memset(g.bits, 0, (size_t)g.w * (size_t)g.h * 4);
    g.text_w = 0;
    for (start = 0; start <= len && lines < OVERLAY_LINES; lines++) {
        int end = start;

        while (end < len && wide[end] != L'\n')
            end++;
        rect.left = 0;
        rect.top = lines ? top + lines * line_h : 0;
        rect.right = g.w;
        rect.bottom = top + (lines + 1) * line_h + 2 * s;
        if (!GetTextExtentPoint32W(g.dc, wide + start, end - start, &extent))
            extent.cx = g.w;
        if ((int)extent.cx + 12 * s > g.text_w)
            g.text_w = (int)extent.cx + 12 * s > g.w ? g.w : (int)extent.cx + 12 * s;
        ExtTextOutW(g.dc, 6 * s, top + lines * line_h, ETO_OPAQUE, &rect,
                    wide + start, (UINT)(end - start), NULL);
        start = end + 1;
    }
    GdiFlush();
#else
    g.text_w = glyph_render((uint32_t *)g.bits, g.w, g.h, s, g.text, len, &lines);
#endif
    g.text_h = (6 + lines * OVERLAY_LINE_H) * s;

    rhi_image_update(g.texture, 0, NULL, g.bits, (uint32_t)g.w * 4, 0);
    return g.text_w > 0;
}

static char g_want[1536];       /* the text asked for, drawn by each present */

void d3d8_overlay_set_text(const char *text)
{
    snprintf(g_want, sizeof g_want, "%s", text ? text : "");
}

void d3d8_overlay_present(RhiView *out, unsigned width, unsigned height,
                          unsigned pic_x, unsigned pic_y, unsigned pic_h)
{
    RhiOutputState saved;
    RhiViewport vp;
    OverlayConstants c;
    float blend_factor[4] = { 1, 1, 1, 1 };
    void *mapped;
    /* The size it had drawn into a 480-line frame and scaled with it, in
     * whole steps: the bitmap is made at that scale and drawn 1:1. */
    unsigned scale = (pic_h + 240) / 480;
    const char *text = g_want;

    if (scale < 1) scale = 1;
    if (scale > OVERLAY_MAX_SCALE) scale = OVERLAY_MAX_SCALE;
    if (!text[0] || !out || !width || !height)
        return;
    if (!overlay_create() || !overlay_size((int)scale) || !overlay_set_text(text))
        return;

    rhi_output_save(&saved);

    if (!(mapped = rhi_buffer_map(g.cb, RHI_MAP_WRITE_DISCARD)))
        goto restore;
    c.x = -1.0f + 2.0f * (float)(pic_x + OVERLAY_PAD * scale) / (float)width;
    c.y =  1.0f - 2.0f * (float)(pic_y + OVERLAY_PAD * scale) / (float)height;
    c.w =  2.0f * (float)g.text_w / (float)width;
    c.h =  2.0f * (float)g.text_h / (float)height;
    c.tex_w = (float)g.text_w;
    c.tex_h = (float)g.text_h;
    c.pad0 = (float)g.w;           /* the texture's own size, for D3D9's shader */
    c.pad1 = (float)g.h;
    memcpy(mapped, &c, sizeof c);
    rhi_buffer_unmap(g.cb);

    vp.x = vp.y = 0.0f;
    vp.width = (float)width;
    vp.height = (float)height;
    vp.min_depth = 0.0f;
    vp.max_depth = 1.0f;
    rhi_set_render_target(out, NULL);
    rhi_set_viewports(1, &vp);
    rhi_set_vertex_layout(NULL);
    rhi_set_topology(RHI_TOPOLOGY_TRIANGLE_STRIP);
    rhi_set_shader(RHI_STAGE_VERTEX, g.vs);
    rhi_set_shader(RHI_STAGE_PIXEL, g.ps);
    rhi_set_uniform_buffers(RHI_STAGE_VERTEX, 7, 1, &g.cb);
    rhi_set_uniform_buffers(RHI_STAGE_PIXEL, 7, 1, &g.cb);
    rhi_set_textures(8, 1, &g.srv);
    rhi_set_blend_state(g.blend, blend_factor, 0xFFFFFFFF);
    rhi_set_depth_state(g.depth, 0);
    rhi_set_raster_state(g.raster);
    rhi_draw(4, 0);

restore:
    rhi_output_restore(&saved);
}

void d3d8_overlay_shutdown(void)
{
    overlay_release_sized();
    rhi_buffer_destroy(g.cb);            g.cb = NULL;
    rhi_shader_destroy(g.vs);            g.vs = NULL;
    rhi_shader_destroy(g.ps);            g.ps = NULL;
    rhi_blend_state_destroy(g.blend);    g.blend = NULL;
    rhi_depth_state_destroy(g.depth);    g.depth = NULL;
    rhi_raster_state_destroy(g.raster);  g.raster = NULL;
    g.tried = 0;
}
