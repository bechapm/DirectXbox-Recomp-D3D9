/*
 * d3d8_display.c -- how big the host renders, and the resolve that puts
 * that image on the screen. See d3d8_display.h for why this works at all.
 *
 * The resolve is a box filter: at scale N every output pixel averages the
 * NxN scene pixels it was rasterised from, which is ordered-grid
 * supersampling and antialiases everything the scene contains --
 * geometry edges, alpha-tested cutouts and the title's own 2D layer
 * alike. The tap count is baked into the shader at compile time rather
 * than read from a constant, so the loop unrolls; the scale is fixed for
 * the life of the process, so there is nothing to recompile for.
 */
#include "d3d8_display.h"
#include "recomp_config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define D3D8_DISPLAY_MAX_SCALE 8

/* ================================================================
 * Policy
 * ================================================================ */

static D3D8DisplayPolicy g_policy;
static int               g_policy_read;

const D3D8DisplayPolicy *d3d8_display_policy(void)
{
    if (!g_policy_read) {
        const char *v = recomp_config_lookup("RECOMP_RES_SCALE", "resolution_scale");
        long n = 1;


        g_policy_read = 1;
        g_policy.scale = 1;

        if (v && *v) {
            char *endp = NULL;
            n = strtol(v, &endp, 10);
            if (endp == v || (endp && *endp) || n < 1 || n > D3D8_DISPLAY_MAX_SCALE) {
                fprintf(stderr, "D3D8 display: RECOMP_RES_SCALE=%s is not a whole "
                        "number from 1 to %d; rendering at the guest's own size\n",
                        v, D3D8_DISPLAY_MAX_SCALE);
                n = 1;
            }
            g_policy.scale = (UINT)n;
        }

        g_policy.anisotropy = 1;
        v = recomp_config_lookup("RECOMP_ANISO", "anisotropy");
        if (v && *v) {
            long a = strtol(v, NULL, 10);

            if (a < 1 || a > 16) {
                fprintf(stderr, "D3D8 display: RECOMP_ANISO=%s is not a whole number "
                        "from 1 to 16; leaving the title's own filtering\n", v);
                a = 1;
            }
            g_policy.anisotropy = (UINT)a;
        }

        g_policy.widescreen = recomp_config_bool("RECOMP_WIDESCREEN", "widescreen", 0);
        g_policy.aspect = g_policy.widescreen ? recomp_widescreen_aspect() : 0.0;

        v = recomp_config_lookup("RECOMP_WIDESCREEN_2D", "widescreen_2d");
        /* auto -- what a new settings file says -- is the title's own
         * default: centre where its project declared CENTRE_2D. */
        g_policy.centre_2d = (v && (strcmp(v, "centre") == 0 || strcmp(v, "center") == 0)) ||
                             ((!v || !*v || strcmp(v, "auto") == 0) &&
                              recomp_widescreen_title_centre_2d());
        if (v && *v && !g_policy.centre_2d && strcmp(v, "auto") != 0)
            fprintf(stderr, "D3D8 display: RECOMP_WIDESCREEN_2D=%s is neither auto nor "
                    "centre; using auto\n", v);

        if (g_policy.widescreen) {
            /* What else widescreen takes for this title is resolved beside
             * the settings (recomp_widescreen_resolve); said here because
             * a stretched picture is the first thing anyone asks about.
             * As known now: a title that widens its own camera says so
             * later, at its first camera (xbox_D3D8ClaimHorPlus). */
            RecompWidescreen w;
            const char *what;

            recomp_widescreen_resolve(&w);
            if (w.hor_plus_named)
                what = w.hor_plus != 1.0
                     ? "hor_plus is set, so the camera is widened by that."
                     : "hor_plus is set to leave the camera alone, so a title with no "
                       "16:9 mode of its own will look stretched.";
            else if (w.mode == RECOMP_WIDE_HOR_PLUS)
                what = "This title has no 16:9 mode of its own, so its camera is "
                       "widened to match (Hor+).";
            else if (w.mode == RECOMP_WIDE_NATIVE)
                what = "This title has a 16:9 mode of its own.";
            else
                what = "Nothing has said whether this title has a 16:9 mode of its own: "
                       "one without draws 4:3 and will look stretched until its project "
                       "says so (recomp_title_widescreen) or hor_plus is set.";
            g_policy.hor_plus_named = w.hor_plus_named && w.hor_plus != 1.0;
            fprintf(stderr, "D3D8 display: widescreen, so the title's frame is presented "
                    "at %.4g:1%s. %s%s\n", g_policy.aspect,
                    g_policy.aspect > RECOMP_ASPECT_16_9 + 1e-3 &&
                    w.mode != RECOMP_WIDE_HOR_PLUS && !g_policy.hor_plus_named
                        ? " (16:9 at most until the title says it widens its own camera)"
                        : "",
                    what,
                    g_policy.centre_2d ? " All 2D but whole-screen passes is kept at 4:3."
                                       : "");
        }
        if (g_policy.anisotropy > 1)
            fprintf(stderr, "D3D8 display: anisotropic filtering forced to %ux where the "
                    "title filters linearly\n", g_policy.anisotropy);
    }
    return &g_policy;
}

/* Widescreen by screen. A 4:3-only title that has been widened can have
 * screens that should not be: TimeSplitters 2's front end is laid out
 * for 4:3 and mixes 3D backdrops with 2D panels, so neither stretching
 * it nor squeezing its 2D leaves it right, while its levels are fine at
 * 16:9. The title project knows which screen it is on and says so between
 * frames; the frames it calls 4:3 are shown between bars, their 2D left
 * alone. */
static int      g_title_narrow;
static int      g_title_said, g_last_wide = -1;
static unsigned g_frames;
/* What the title last said, kept apart from the frame-by-frame answer so the
 * automatic one (xbox_D3D8SetWideFramesAuto) can narrow a frame further but
 * never widen one the title called 4:3. Turok: Evolution needs both: its front
 * end is a 3D stage built for 4:3, which only the title can recognise, and
 * its movies are frames with no 3D at all. */
static int      g_title_says_narrow;

void xbox_D3D8SetWideFrames(BOOL wide)
{
    g_title_said = 1;
    g_title_says_narrow = wide ? 0 : 1;
    g_title_narrow = g_title_says_narrow;
}

int d3d8_display_wide_now(void)
{
    return d3d8_display_policy()->widescreen && !g_title_narrow;
}

/* Widescreen by what a frame draws (xbox_D3D8SetWideFramesAuto): whether
 * the frame in hand has drawn anything in perspective onto the screen. */
static int g_auto_wide, g_frame_drew_3d;

void xbox_D3D8SetWideFramesAuto(BOOL on)
{
    g_auto_wide = on ? 1 : 0;
}

void d3d8_display_note_3d_draw(void)
{
    g_frame_drew_3d = 1;
}

void d3d8_display_frame_done(void)
{
    int wide = d3d8_display_wide_now();

    g_frames++;
    if (g_title_said && d3d8_display_policy()->widescreen && wide != g_last_wide) {
        fprintf(stderr, "D3D8 display: frame %u on at %s\n", g_frames,
                wide ? "16:9" : "4:3");
        g_last_wide = wide;
    }
    /* The next frame takes the shape this one called for. A frame's draws
     * are not all in when it starts, and a screen lasts many frames, so the
     * cost is one frame at each change. */
    if (g_auto_wide) {
        g_title_said = 1;
        g_title_narrow = g_frame_drew_3d && !g_title_says_narrow ? 0 : 1;
    }
    g_frame_drew_3d = 0;
}

/* The guest's presentation size, from the scene size asked for at device
 * creation: a frame shown at the guest's own shape is shown at this. */
static UINT g_guest_w = 640, g_guest_h = 480;

void d3d8_display_scene_size(UINT guest_w, UINT guest_h,
                             UINT *scene_w, UINT *scene_h)
{
    const D3D8DisplayPolicy *p = d3d8_display_policy();
    static int said;
    int lines, w = 0, h = 0;

    if (guest_w && guest_h) {
        g_guest_w = guest_w;
        g_guest_h = guest_h;
    }
    /* As many lines as the settings ask (resolution, else the guest's
     * height times the scale), as wide as the picture is shown: square
     * pixels at any shape, so a 21:9 picture is rendered 21:9 rather than
     * a 4:3 one stretched. The guest's own shape at a whole-number scale is
     * exactly guest * scale, as it always was. */
    lines = recomp_render_height((int)guest_h);
    recomp_scene_size((int)guest_w, (int)guest_h, lines,
                      p->widescreen ? p->aspect : 0.0, &w, &h);
    if (!said++ && ((UINT)w != guest_w || (UINT)h != guest_h))
        fprintf(stderr, "D3D8 display: rendering at %dx%d for a %ux%u guest, filtered "
                "to the window at present\n", w, h, guest_w, guest_h);
    if (scene_w) *scene_w = (UINT)w;
    if (scene_h) *scene_h = (UINT)h;
}

double d3d8_display_shown_aspect(void)
{
    const D3D8DisplayPolicy *p = d3d8_display_policy();
    double a;

    if (!d3d8_display_wide_now() || !(p->aspect > 0.0))
        return (double)g_guest_w / (double)g_guest_h;
    a = p->aspect;
    /* A title with a 16:9 mode of its own draws 16:9 and nothing wider:
     * past that it is shown between bars rather than stretched. Asked
     * every frame, since a title that widens its camera itself says so
     * only at its first camera (xbox_D3D8ClaimHorPlus). */
    if (a > RECOMP_ASPECT_16_9 && recomp_widescreen_title_mode() != RECOMP_WIDE_HOR_PLUS &&
        !p->hor_plus_named)
        a = RECOMP_ASPECT_16_9;
    return a;
}

void d3d8_display_output_shape(UINT scene_w, UINT scene_h,
                               UINT *shape_w, UINT *shape_h)
{
    UINT w, h;

    if (d3d8_display_wide_now()) {
        /* The frame is drawn for a picture of the widescreen shape, into
         * whatever buffer it renders to, and the display is meant to show
         * it at that shape. Presenting it at the buffer's own shape would
         * leave a squeeze in where the two differ. */
        double a = d3d8_display_shown_aspect();

        if (a > RECOMP_ASPECT_16_9 - 1e-6 && a < RECOMP_ASPECT_16_9 + 1e-6) {
            w = 16;
            h = 9;
        } else if (scene_w && scene_h &&
                   (double)scene_w / scene_h > a - 1e-3 && (double)scene_w / scene_h < a + 1e-3) {
            w = scene_w;
            h = scene_h;
        } else {
            w = (UINT)(a * 1000.0 + 0.5);
            h = 1000;
        }
    } else {
        /* The guest's own shape, which is the scene's unless the scene is
         * rendered wider for widescreen and this frame was called 4:3. */
        w = g_guest_w;
        h = g_guest_h;
        if (scene_w && scene_h && (uint64_t)scene_w * g_guest_h == (uint64_t)scene_h * g_guest_w) {
            w = scene_w;
            h = scene_h;
        }
    }
    if (shape_w) *shape_w = w;
    if (shape_h) *shape_h = h;
}

/* Packed width << 16 | height, so a reader on another thread never sees
 * one frame's width with another's height. */
static volatile long g_shown;

void d3d8_display_note_shown(UINT shape_w, UINT shape_h)
{
    g_shown = (long)(((shape_w & 0xFFFFu) << 16) | (shape_h & 0xFFFFu));
}

BOOL xbox_D3D8PictureShape(UINT *width, UINT *height)
{
    long v = g_shown;

    if (width)  *width = (UINT)((unsigned long)v >> 16) & 0xFFFFu;
    if (height) *height = (UINT)v & 0xFFFFu;
    return v != 0;
}

/* The widescreen setting, not d3d8_display_wide_now: a window lasts the
 * whole run, and the screens a title calls 4:3 are shown in it between
 * bars. As tall as the lines rendered (resolution), so a 1440-line picture
 * is not shown squeezed into a 480-line window; the window code fits it to
 * the screen it opens on. */
void xbox_D3D8DefaultWindowSize(UINT guest_w, UINT guest_h, UINT *width, UINT *height)
{
    int w, h;
    int lines = recomp_render_height((int)guest_h);

    const D3D8DisplayPolicy *p = d3d8_display_policy();
    double a = p->widescreen ? p->aspect : 0.0;

    if (guest_w && guest_h && lines > 0) {
        guest_w = (UINT)(((unsigned long long)guest_w * (unsigned)lines + guest_h / 2) / guest_h);
        guest_h = (UINT)lines;
    }

    /* What d3d8_display_shown_aspect gives a widescreen frame: the shape,
     * no wider than 16:9 for a title with a mode of its own. */
    if (a > RECOMP_ASPECT_16_9 && recomp_widescreen_title_mode() != RECOMP_WIDE_HOR_PLUS &&
        !p->hor_plus_named)
        a = RECOMP_ASPECT_16_9;
    recomp_window_size_aspect((int)guest_w, (int)guest_h, a, &w, &h);
    if (width)  *width = (UINT)w;
    if (height) *height = (UINT)h;
}

D3D8DisplayFit d3d8_display_fit(UINT shape_w, UINT shape_h, UINT bb_w, UINT bb_h)
{
    D3D8DisplayFit f;

    f.x = f.y = 0;
    f.w = bb_w;
    f.h = bb_h;
    if (!shape_w || !shape_h || !bb_w || !bb_h)
        return f;

    /* Compare shapes in integers: shape_w/shape_h against bb_w/bb_h. */
    if ((uint64_t)shape_w * bb_h > (uint64_t)bb_w * shape_h) {
        /* The window is taller than the picture: bars above and below. */
        f.h = (UINT)(((uint64_t)bb_w * shape_h) / shape_w);
        if (!f.h) f.h = 1;
        f.y = (bb_h - f.h) / 2;
    } else if ((uint64_t)shape_w * bb_h < (uint64_t)bb_w * shape_h) {
        /* The window is wider: bars to the left and right. */
        f.w = (UINT)(((uint64_t)bb_h * shape_w) / shape_h);
        if (!f.w) f.w = 1;
        f.x = (bb_w - f.w) / 2;
    }
    return f;
}

/* ================================================================
 * Resolve
 * ================================================================ */

/* origin: where the picture starts inside the back buffer, subtracted
 * because SV_Position counts from the buffer's corner and not the
 * viewport's. ratio: scene texels per output pixel. texel: one over the
 * scene size, for normalised sampling.
 *
 * TAPS x TAPS bilinear samples spread across each output pixel's
 * footprint. Bilinear rather than Load so a ratio that is not a whole
 * number -- which is most window sizes -- still resolves smoothly, and so
 * a window larger than the scene magnifies cleanly instead of blocking
 * up. At a whole-number ratio with taps to match, this is the box filter
 * that makes the supersampling exact. The taps are counted for each axis:
 * a scene rendered wide for widescreen and shown at 4:3 for a frame the
 * title called 4:3 is squeezed more across than down. */
typedef struct {
    float origin_x, origin_y, ratio_x, ratio_y;
    float texel_x, texel_y, inv_taps, pad;
} ResolveConstants;

static const char kSource[] =
    "cbuffer Resolve : register(b7) { float4 p; float4 q; };\n"
    "struct VSOut { float4 pos : SV_Position; };\n"
    "VSOut vs_main(uint id : SV_VertexID) {\n"
    "    float2 c = float2((id << 1) & 2, id & 2);\n"   /* one oversized triangle */
    "    VSOut o;\n"
    "    o.pos = float4(c * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n"
    "Texture2D<float4> scene : register(t9);\n"
    "SamplerState smp : register(s9);\n"
    "float4 ps_main(VSOut i) : SV_Target {\n"
    /* The footprint's corner: SV_Position is the pixel's centre (x + 0.5),
     * and the taps below already step to their own centres, so counting
     * from the centre put every tap half an output pixel right and down --
     * at scale 1 the presented frame was each 2x2 block of the scene
     * averaged, a half-pixel blur, and at 2x the box filter read the wrong
     * four texels. */
    "    float2 base = (i.pos.xy - 0.5 - p.xy) * p.zw;\n"
    "    float2 step = p.zw / float2(TAPS_X, TAPS_Y);\n"
    "    float4 sum = 0;\n"
    "    [unroll] for (int y = 0; y < TAPS_Y; y++)\n"
    "        [unroll] for (int x = 0; x < TAPS_X; x++)\n"
    "            sum += scene.SampleLevel(smp,\n"
    "                       (base + (float2(x, y) + 0.5) * step) * q.xy, 0);\n"
    "    return sum * q.z;\n"
    "}\n";

static struct {
    RhiShader      *vs;
    RhiShader      *ps;
    RhiBuffer      *cb;
    RhiBlendState  *blend;
    RhiDepthState  *depth;
    RhiRasterState *raster;
    RhiSampler     *sampler;
    UINT            taps_x, taps_y;      /* what the shaders were built for */
    int             tried, failed;
} g;

static void fail(const char *what)
{
    g.failed = 1;
    fprintf(stderr, "D3D8 display: %s failed; the scene cannot be "
            "resolved and the window will stay blank\n", what);
    fflush(stderr);
}

static RhiShader *compile_one(uint32_t stage, const char *entry, const char *target,
                              const RhiMacro *macros)
{
    RhiShaderSource src;
    RhiShader *s;
    char err[2048];

    memset(&src, 0, sizeof src);
    src.hlsl = kSource;
    src.len = sizeof kSource - 1;
    src.name = "display_resolve";
    src.macros = macros;
    src.entry = entry;
    src.target = target;
    s = rhi_shader_create(stage, &src, err, sizeof err);
    if (!s) {
        if (err[0])
            fprintf(stderr, "D3D8 display: %s\n", err);
        fail("shader creation");
    }
    return s;
}

static int create(UINT taps_x, UINT taps_y)
{
    RhiMacro macros[3];
    char tx_text[16], ty_text[16];
    RhiBufferDesc bd;
    RhiBlendDesc bl;
    RhiDepthDesc ds;
    RhiRasterDesc rd;
    RhiSamplerDesc sm;

    if (g.failed) return 0;
    if (g.tried && g.taps_x == taps_x && g.taps_y == taps_y) return g.ps != NULL;
    if (!rhi_device_ready()) { g.failed = 1; return 0; }

    /* The tap count follows the window, not the scale: a window of a
     * different shape from the scene gets a different ratio, and one
     * that is resized gets a new one mid-run. Rebuild the two shaders
     * for it and keep everything else. */
    if (g.tried) {
        rhi_shader_destroy(g.vs); g.vs = NULL;
        rhi_shader_destroy(g.ps); g.ps = NULL;
    }
    g.tried = 1;
    g.taps_x = taps_x;
    g.taps_y = taps_y;

    snprintf(tx_text, sizeof tx_text, "%u", taps_x);
    snprintf(ty_text, sizeof ty_text, "%u", taps_y);
    macros[0].name = "TAPS_X";
    macros[0].value = tx_text;
    macros[1].name = "TAPS_Y";
    macros[1].value = ty_text;
    macros[2].name = NULL;
    macros[2].value = NULL;

    if (!(g.vs = compile_one(RHI_STAGE_VERTEX, "vs_main", "vs_4_0", macros))) return 0;
    if (!(g.ps = compile_one(RHI_STAGE_PIXEL, "ps_main", "ps_4_0", macros))) return 0;

    if (g.cb)
        return 1;                       /* rebuild: the rest already exists */

    memset(&bd, 0, sizeof bd);
    bd.size = sizeof(ResolveConstants);
    bd.usage = RHI_USAGE_DYNAMIC;
    bd.bind = RHI_BIND_UNIFORM;
    bd.cpu_access = RHI_CPU_WRITE;
    if (!(g.cb = rhi_buffer_create(&bd, NULL))) { fail("CreateBuffer"); return 0; }

    /* Opaque, depthless, unculled: this pass replaces the target. */
    memset(&bl, 0, sizeof bl);
    bl.write_mask = RHI_WRITE_ALL;
    if (!(g.blend = rhi_blend_state_create(&bl))) { fail("CreateBlendState"); return 0; }

    memset(&ds, 0, sizeof ds);
    if (!(g.depth = rhi_depth_state_create(&ds))) { fail("CreateDepthStencilState"); return 0; }

    memset(&rd, 0, sizeof rd);
    rd.fill = RHI_FILL_SOLID;
    rd.cull = RHI_CULL_NONE;
    rd.depth_clip = 1;
    if (!(g.raster = rhi_raster_state_create(&rd))) { fail("CreateRasterizerState"); return 0; }

    memset(&sm, 0, sizeof sm);
    sm.filter = RHI_FILTER_LINEAR;
    sm.address_u = sm.address_v = sm.address_w = RHI_ADDRESS_CLAMP;
    sm.compare = RHI_CMP_NEVER;
    sm.max_lod = 3.402823466e+38f;          /* D3D11_FLOAT32_MAX */
    if (!(g.sampler = rhi_sampler_create(&sm))) { fail("CreateSamplerState"); return 0; }

    return 1;
}

HRESULT d3d8_display_resolve(RhiView *scene, UINT scene_w, UINT scene_h,
                             RhiView *out, D3D8DisplayFit fit)
{
    RhiOutputState saved;
    RhiViewport vp;
    float blend_factor[4] = { 1, 1, 1, 1 };
    UINT taps_x, taps_y;
    void *mapped;

    if (!scene || !out || !fit.w || !fit.h || !scene_h || !scene_w)
        return E_INVALIDARG;

    /* Taps for each axis from its own ratio: the same whenever the fit
     * kept the scene's shape, and more across than down for a wide scene
     * shown at 4:3. Rounded rather than truncated so a ratio just under a
     * whole number keeps the taps that ratio deserves. */
    taps_x = (scene_w + fit.w / 2) / fit.w;
    taps_y = (scene_h + fit.h / 2) / fit.h;
    if (taps_x < 1) taps_x = 1;
    if (taps_x > 8) taps_x = 8;
    if (taps_y < 1) taps_y = 1;
    if (taps_y > 8) taps_y = 8;
    if (!create(taps_x, taps_y)) return E_FAIL;

    rhi_output_save(&saved);

    if ((mapped = rhi_buffer_map(g.cb, RHI_MAP_WRITE_DISCARD)) != NULL) {
        ResolveConstants c;
        c.origin_x = (float)fit.x;
        c.origin_y = (float)fit.y;
        c.ratio_x = (float)scene_w / (float)fit.w;
        c.ratio_y = (float)scene_h / (float)fit.h;
        c.texel_x = 1.0f / (float)scene_w;
        c.texel_y = 1.0f / (float)scene_h;
        c.inv_taps = 1.0f / (float)(taps_x * taps_y);
        c.pad = 0.0f;
        memcpy(mapped, &c, sizeof c);
        rhi_buffer_unmap(g.cb);
    }

    vp.x = (float)fit.x;
    vp.y = (float)fit.y;
    vp.width = (float)fit.w;
    vp.height = (float)fit.h;
    vp.min_depth = 0.0f;
    vp.max_depth = 1.0f;
    rhi_set_render_target(out, NULL);
    /* The bars. Cheaper than tracking whether the window changed shape,
     * and it costs one clear of a buffer that is about to be presented. */
    {
        float black[4] = { 0, 0, 0, 1 };
        rhi_clear_color(out, black);
    }
    rhi_set_viewports(1, &vp);
    rhi_set_vertex_layout(NULL);
    rhi_set_topology(RHI_TOPOLOGY_TRIANGLES);
    rhi_set_shader(RHI_STAGE_VERTEX, g.vs);
    rhi_set_shader(RHI_STAGE_PIXEL, g.ps);
    rhi_set_uniform_buffers(RHI_STAGE_PIXEL, 7, 1, &g.cb);
    rhi_set_textures(9, 1, &scene);
    rhi_set_samplers(9, 1, &g.sampler);
    rhi_set_blend_state(g.blend, blend_factor, 0xFFFFFFFF);
    rhi_set_depth_state(g.depth, 0);
    rhi_set_raster_state(g.raster);
    rhi_draw(3, 0);

    /* The scene must not stay bound as a shader input: it is the render
     * target again as soon as the next frame starts. */
    {
        RhiView *none = NULL;
        rhi_set_textures(9, 1, &none);
    }
    rhi_output_restore(&saved);
    return S_OK;
}

void d3d8_display_shutdown(void)
{
    rhi_shader_destroy(g.vs);            g.vs = NULL;
    rhi_shader_destroy(g.ps);            g.ps = NULL;
    rhi_buffer_destroy(g.cb);            g.cb = NULL;
    rhi_blend_state_destroy(g.blend);    g.blend = NULL;
    rhi_depth_state_destroy(g.depth);    g.depth = NULL;
    rhi_raster_state_destroy(g.raster);  g.raster = NULL;
    rhi_sampler_destroy(g.sampler);      g.sampler = NULL;
    g.tried = 0;
    g.failed = 0;
}

