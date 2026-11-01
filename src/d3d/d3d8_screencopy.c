/*
 * d3d8_screencopy.c -- the finished frame, copied into a texture.
 *
 * A title that post-processes its own image reads the screen back: it copies
 * the back buffer into a texture and draws that texture over the scene again,
 * for a glow, a soften, a heat haze. On the console the GPU does the copy and
 * the texture's memory is the framebuffer's.
 *
 * Under shadow mode the title's own copy still happens, but it happens in the
 * guest's world, where nothing is rendered -- so the texture it reads back is
 * all zeros, and drawing it over the scene blends black over everything.
 * TimeSplitters 2 does exactly this, three times a frame, and it cost the
 * picture 62% of its brightness before this existed.
 *
 * So the copy is done here as well, from the host's back buffer into the host
 * texture standing in for the title's. It is a draw rather than a resource
 * copy because the two rarely agree on format: the swap chain is RGBA and a
 * title's texture is usually BGRA, which D3D11 will not copy between. Reading
 * with Load at integer pixels also means no sampler is bound, so a texture
 * stage the title set is left alone -- the same rule d3d8_overlay.c follows
 * and for the same reason. Under D3D9 it is a StretchRect, which converts
 * the format itself, and which RTX Remix does not see as a draw.
 *
 * At the scene's resolution, not the texture's. The title's texture is the
 * size of its own screen (TimeSplitters 2's is 640x480), and the scene is
 * rendered larger (resolution, aspect_ratio): copied at the texture's size,
 * the frame drawn back over itself is a 640x480 picture stretched up, and a
 * title that blends its copy back every frame (TimeSplitters 2's glow,
 * three passes at up to a quarter each) shows a 480-line picture whatever
 * the resolution. So each texture a frame is copied into also gets an image
 * as many times larger as the scene is than the guest's screen
 * (hires_for), the copy goes into both, and while the last full copy is in
 * the large one it is what the texture samples: the same coordinates, the
 * same picture, more pixels. Anything else that writes the texture -- an
 * upload, a draw into it -- hands it back its own image
 * (d3d8_screencopy_forget_image). RECOMP_SCREEN_COPY_SCALE=0 keeps the
 * copy at the texture's size, as before.
 */
#include "d3d8_internal.h"
#include "d3d8_xbox.h"
#include "rhi_d3d9.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    int             tried, failed;
    RhiView        *back_srv;       /* the scene, which the device owns */
    RhiShader      *vs;
    RhiShader      *ps;
    RhiBuffer      *cb;
    RhiBlendState  *blend;
    RhiDepthState  *depth;
    RhiRasterState *raster;
    unsigned long   copies;
} g;

/* The large images (see the top of the file). A frame copies into one to
 * three textures, so a few entries are plenty; a texture past them keeps
 * its own size. */
#define HIRES_MAX 8
typedef struct {
    D3D8Texture *tex;
    RhiImage    *image;
    RhiView     *srv, *rtv;
    RhiView     *own_srv;          /* the texture's, while this one stands in */
    UINT         w, h;
    int          active;
} Hires;

static Hires g_hires[HIRES_MAX];

static int hires_enabled(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("RECOMP_SCREEN_COPY_SCALE");

        on = !(e && e[0] == '0');
    }
    return on;
}

static void hires_release(Hires *h)
{
    if (h->active && h->tex) {
        h->tex->srv = h->own_srv;
        h->active = 0;
        d3d8_rebind_stage_textures();   /* no stage left on the view below */
    }
    rhi_view_destroy(h->srv);
    rhi_view_destroy(h->rtv);
    rhi_image_destroy(h->image);
    memset(h, 0, sizeof *h);
}

/* The texture samples its own image again. */
static void hires_deactivate(Hires *h)
{
    if (!h->active)
        return;
    h->tex->srv = h->own_srv;
    h->active = 0;
    d3d8_rebind_stage_textures();
}

/* Something other than a copy wrote this image: a texture's own image is
 * what it holds now. */
void d3d8_screencopy_forget_image(RhiImage *image)
{
    int i;

    if (!image)
        return;
    for (i = 0; i < HIRES_MAX; i++)
        if (g_hires[i].tex && g_hires[i].tex->image == image)
            hires_deactivate(&g_hires[i]);
}

/* The texture is going away. */
void d3d8_screencopy_forget_texture(D3D8Texture *tex)
{
    int i;

    for (i = 0; i < HIRES_MAX; i++)
        if (g_hires[i].tex == tex)
            hires_release(&g_hires[i]);
}

/* The large image for `tex`, made the first time: its size times the
 * scene's over the guest's, in each direction. NULL when that is no larger
 * than the texture (nothing is scaled), or it cannot be made. */
static Hires *hires_for(D3D8Texture *tex, UINT back_w, UINT back_h)
{
    UINT guest_w = d3d8_GetGuestWidth(), guest_h = d3d8_GetGuestHeight();
    UINT w, h;
    Hires *free_slot = NULL;
    RhiImageDesc d;
    RhiViewDesc vd;
    int i;

    if (!hires_enabled() || !guest_w || !guest_h || !tex->width || !tex->height)
        return NULL;
    w = (UINT)(((uint64_t)tex->width * back_w + guest_w / 2) / guest_w);
    h = (UINT)(((uint64_t)tex->height * back_h + guest_h / 2) / guest_h);
    if (w > back_w) w = back_w;
    if (h > back_h) h = back_h;
    if (w <= tex->width && h <= tex->height)
        return NULL;
    for (i = 0; i < HIRES_MAX; i++) {
        if (g_hires[i].tex == tex) {
            if (g_hires[i].w == w && g_hires[i].h == h)
                return &g_hires[i];
            hires_release(&g_hires[i]);   /* the scene changed size */
        }
        if (!g_hires[i].tex && !free_slot)
            free_slot = &g_hires[i];
    }
    if (!free_slot)
        return NULL;

    memset(&d, 0, sizeof d);
    d.type = RHI_IMAGE_2D;
    d.width = w;
    d.height = h;
    d.depth = 1;
    d.mip_levels = 1;
    d.format = tex->host_format;
    d.samples = 1;
    d.usage = RHI_USAGE_DEFAULT;
    d.bind = RHI_BIND_RENDER_TARGET | RHI_BIND_SAMPLED;
    free_slot->image = rhi_image_create(&d, NULL);
    memset(&vd, 0, sizeof vd);
    vd.format = tex->host_format;
    vd.dim = RHI_VIEW_DIM_2D;
    if (free_slot->image)
        free_slot->rtv = rhi_view_create(free_slot->image, RHI_VIEW_RENDER_TARGET, &vd);
    vd.mip_count = 1;
    if (free_slot->rtv)
        free_slot->srv = rhi_view_create(free_slot->image, RHI_VIEW_SAMPLED, &vd);
    if (!free_slot->srv) {
        static int said;

        if (!said++)
            fprintf(stderr, "D3D8 screen copy: a %ux%u copy of the frame could not be "
                    "made; screen copies stay at the texture's size\n", w, h);
        hires_release(free_slot);
        return NULL;
    }
    free_slot->tex = tex;
    free_slot->w = w;
    free_slot->h = h;
    {
        static int said;

        if (!said++)
            fprintf(stderr, "D3D8 screen copy: the frame is copied at %ux%u for a %ux%u "
                    "texture, the scene's resolution (RECOMP_SCREEN_COPY_SCALE=0 for "
                    "the texture's own)\n", w, h, tex->width, tex->height);
    }
    return free_slot;
}

/* Source pixel = destination pixel * scale + offset. Scale is one when the
 * sizes agree, which they do whenever a title reads back its whole screen;
 * the offset places a rectangle (xbox_D3D8CopyBackBufferRectToTexture). */
typedef struct { float scale_x, scale_y, off_x, off_y; } ScreenCopyConstants;

static const char kSource[] =
    "cbuffer ScreenCopy : register(b7) { float4 scale; };\n"
    "struct VSOut { float4 pos : SV_Position; };\n"
    "VSOut vs_main(uint id : SV_VertexID) {\n"
    "    float2 c = float2((id << 1) & 2, id & 2);\n"   /* one oversized triangle */
    "    VSOut o;\n"
    "    o.pos = float4(c * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n"
    "Texture2D<float4> screen : register(t9);\n"
    "float4 ps_main(VSOut i) : SV_Target {\n"
    "    int2 p = int2(i.pos.xy * scale.xy + scale.zw);\n"
    "    return screen.Load(int3(p, 0));\n"
    "}\n";

static void fail(const char *what)
{
    g.failed = 1;
    fprintf(stderr, "D3D8 screen copy: %s failed; a title that reads its "
            "own screen back will see black\n", what);
    fflush(stderr);
}

static RhiShader *compile_one(uint32_t stage, const char *entry, const char *target)
{
    RhiShaderSource src;
    RhiShader *s;
    char err[2048];

    memset(&src, 0, sizeof src);
    src.hlsl = kSource;
    src.len = sizeof kSource - 1;
    src.name = "screencopy";
    src.entry = entry;
    src.target = target;
    s = rhi_shader_create(stage, &src, err, sizeof err);
    if (!s) {
        if (err[0])
            fprintf(stderr, "D3D8 screen copy: %s\n", err);
        fail(stage == RHI_STAGE_VERTEX ? "the vertex shader" : "the pixel shader");
    }
    return s;
}

static int create(void)
{
    RhiBufferDesc bd;
    RhiBlendDesc bl;
    RhiDepthDesc ds;
    RhiRasterDesc rd;

    if (g.failed) return 0;
    if (g.tried) return g.back_srv != NULL;
    g.tried = 1;
    if (!rhi_device_ready()) { g.failed = 1; return 0; }

    /* The scene the title has been drawing into, which is the swap chain's
     * own back buffer only while nothing is scaled. Reading the back
     * buffer directly would be wrong above scale 1: the resolve that fills
     * it has not run yet this frame, so it still holds the last one. */
    g.back_srv = d3d8_GetSceneView();
    if (!g.back_srv) { fail("GetSceneView"); return 0; }

    if (!(g.vs = compile_one(RHI_STAGE_VERTEX, "vs_main", "vs_4_0"))) return 0;
    if (!(g.ps = compile_one(RHI_STAGE_PIXEL, "ps_main", "ps_4_0"))) return 0;

    memset(&bd, 0, sizeof bd);
    bd.size = sizeof(ScreenCopyConstants);
    bd.usage = RHI_USAGE_DYNAMIC;
    bd.bind = RHI_BIND_UNIFORM;
    bd.cpu_access = RHI_CPU_WRITE;
    if (!(g.cb = rhi_buffer_create(&bd, NULL))) { fail("CreateBuffer"); return 0; }

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
    return 1;
}

/* RECOMP_D3D8_SCREENCOPY_PROBE=n: what the source holds at the moment of the
 * first n copies, and whether drawing was going to the scene target at all.
 * The texture layer's own probe reads the destination; when that says "not
 * arriving" this says which side is empty. It reads the scene through
 * rhi_image_readback, so it reports the same under either backend. */
static void probe_source(const RhiOutputState *saved, const D3D8Texture *tex,
                         UINT back_w, UINT back_h)
{
    static int probe = -1;
    static unsigned long said;
    RhiImage *scene;
    RhiImageDesc sd;
    unsigned long long sum = 0;
    unsigned nonzero = 0, samples = 0;
    uint8_t *pixels;

    if (probe < 0) {
        const char *v = getenv("RECOMP_D3D8_SCREENCOPY_PROBE");
        probe = (v && atoi(v) > 0) ? atoi(v) : 0;   /* how many copies to report */
    }
    if (!probe || said++ >= (unsigned long)probe)
        return;

    memset(&sd, 0, sizeof sd);
    scene = d3d8_GetSceneImage();
    if (scene) {
        rhi_image_get_desc(scene, &sd);
        pixels = malloc((size_t)sd.width * sd.height * 4u);
        if (pixels && rhi_image_readback(scene, 0, pixels, sd.width * 4u) == 0) {
            UINT x, y;
            for (y = 0; y < sd.height; y += 16) {
                const uint8_t *row = pixels + (size_t)y * sd.width * 4u;
                for (x = 0; x < sd.width; x += 16) {
                    const uint8_t *p = row + (size_t)x * 4u;
                    sum += (unsigned)p[0] + p[1] + p[2];
                    samples += 3;
                    if (p[0] || p[1] || p[2]) nonzero++;
                }
            }
        }
        free(pixels);
    }
    fprintf(stderr, "D3D8 screen copy probe: scene %ux%u holds mean %.1f/255, %u of %u "
            "non-zero; current target %s the scene; dst %ux%u scale %g x %g\n",
            sd.width, sd.height,
            samples ? (double)sum / samples : 0.0, nonzero, samples / 3u,
            rhi_output_color_is(saved, d3d8_GetDefaultTargetView()) ? "is" : "is NOT",
            tex->width, tex->height,
            tex->width ? (double)back_w / tex->width : 0.0,
            tex->height ? (double)back_h / tex->height : 0.0);
    fflush(stderr);
}

/* The copy itself. With src NULL: the whole frame, scaled to the whole
 * texture. With src: that rectangle of the frame, in the title's own
 * pixels, to the same size at `at` in the texture -- CopyRects does not
 * scale. */
static HRESULT copy_to_texture(IDirect3DTexture8 *dst, const RECT *src, const POINT *at);

HRESULT xbox_D3D8CopyBackBufferToTexture(IDirect3DTexture8 *dst)
{
    return copy_to_texture(dst, NULL, NULL);
}

HRESULT xbox_D3D8CopyBackBufferRectToTexture(IDirect3DTexture8 *dst, const RECT *src,
                                             const POINT *at)
{
    return copy_to_texture(dst, src, at);
}

/* One copy into one target: the destination's rectangle `vp` from the
 * scene through `c`. */
static void copy_into(RhiView *rtv, RhiViewport vp, const ScreenCopyConstants *c,
                      UINT back_w, UINT back_h)
{
    float blend_factor[4] = { 1, 1, 1, 1 };
    void *mapped;

    if (rhi_d3d9_active()) {
        /* D3D9: a copy, not a draw, so RTX Remix does not take the frame
         * drawn back over itself for the scene. The same pixels: the
         * destination's rectangle from the source's through scale and
         * offset. */
        RhiRect dr, sr;

        dr.left = (int32_t)vp.x;
        dr.top = (int32_t)vp.y;
        dr.right = (int32_t)(vp.x + vp.width);
        dr.bottom = (int32_t)(vp.y + vp.height);
        sr.left = (int32_t)((float)dr.left * c->scale_x + c->off_x);
        sr.top = (int32_t)((float)dr.top * c->scale_y + c->off_y);
        sr.right = (int32_t)((float)dr.right * c->scale_x + c->off_x);
        sr.bottom = (int32_t)((float)dr.bottom * c->scale_y + c->off_y);
        if (sr.left < 0) sr.left = 0;
        if (sr.top < 0) sr.top = 0;
        if (sr.right > (int32_t)back_w) sr.right = (int32_t)back_w;
        if (sr.bottom > (int32_t)back_h) sr.bottom = (int32_t)back_h;
        if (sr.right > sr.left && sr.bottom > sr.top &&
            rhi_d3d9_copy_rect(d3d8_GetSceneView(), &sr, rtv, &dr) != 0) {
            static int said;
            if (!said++)
                fprintf(stderr, "D3D8 screen copy: StretchRect into the texture failed\n");
        }
        return;
    }
    if (g.cb && (mapped = rhi_buffer_map(g.cb, RHI_MAP_WRITE_DISCARD)) != NULL) {
        memcpy(mapped, c, sizeof *c);
        rhi_buffer_unmap(g.cb);
    }
    vp.min_depth = 0.0f;
    vp.max_depth = 1.0f;
    rhi_set_render_target(rtv, NULL);
    rhi_set_viewports(1, &vp);
    rhi_set_vertex_layout(NULL);
    rhi_set_topology(RHI_TOPOLOGY_TRIANGLES);
    rhi_set_shader(RHI_STAGE_VERTEX, g.vs);
    rhi_set_shader(RHI_STAGE_PIXEL, g.ps);
    rhi_set_uniform_buffers(RHI_STAGE_PIXEL, 7, 1, &g.cb);
    /* Asked for every time: while frame interpolation draws an in-between
     * frame, the scene is a second image (xbox_D3D8InterpBegin). */
    g.back_srv = d3d8_GetSceneView();
    rhi_set_textures(9, 1, &g.back_srv);
    rhi_set_blend_state(g.blend, blend_factor, 0xFFFFFFFF);
    rhi_set_depth_state(g.depth, 0);
    rhi_set_raster_state(g.raster);
    rhi_draw(3, 0);

    /* The back buffer must not stay bound as a shader input while it is the
     * render target again. */
    {
        RhiView *none = NULL;
        rhi_set_textures(9, 1, &none);
    }
}

static HRESULT copy_to_texture(IDirect3DTexture8 *dst, const RECT *src, const POINT *at)
{
    D3D8Texture *tex = (D3D8Texture *)dst;
    RhiImage *dst_image;
    RhiView *rtv;
    RhiViewDesc rtvd;
    RhiOutputState saved;
    ScreenCopyConstants c;
    RhiViewport vp;
    UINT back_w = d3d8_GetBackBufferWidth(), back_h = d3d8_GetBackBufferHeight();
    Hires *hi;
    int whole;

    if (!rhi_device_ready() || !tex || !tex->image || !back_w || !back_h)
        return E_INVALIDARG;
    if (!rhi_d3d9_active() && !create())
        return E_FAIL;

    dst_image = tex->image;
    memset(&rtvd, 0, sizeof rtvd);
    rtvd.format = tex->host_format;
    rtvd.dim = RHI_VIEW_DIM_2D;
    rtv = rhi_view_create(dst_image, RHI_VIEW_RENDER_TARGET, &rtvd);
    if (!rtv) {
        static int said;
        if (!said++)
            fprintf(stderr, "D3D8 screen copy: the destination texture is not a render "
                    "target; nothing copied\n");
        return E_FAIL;
    }

    rhi_output_save(&saved);
    probe_source(&saved, tex, back_w, back_h);

    memset(&vp, 0, sizeof vp);
    vp.width = (float)tex->width;
    vp.height = (float)tex->height;
    c.scale_x = tex->width ? (float)back_w / (float)tex->width : 1.0f;
    c.scale_y = tex->height ? (float)back_h / (float)tex->height : 1.0f;
    c.off_x = c.off_y = 0.0f;
    whole = 1;
    if (src) {
        /* The rectangle is in the title's pixels and the frame may be
         * rendered larger (resolution): one texture pixel is back/guest
         * frame pixels. A title drawing its handheld's map into a corner of
         * the back buffer and copying that corner out got the whole frame
         * squeezed into the texture, map and all. */
        UINT guest_w = d3d8_GetGuestWidth(), guest_h = d3d8_GetGuestHeight();
        float kx = guest_w ? (float)back_w / (float)guest_w : 1.0f;
        float ky = guest_h ? (float)back_h / (float)guest_h : 1.0f;
        LONG ax = at ? at->x : 0, ay = at ? at->y : 0;
        LONG w = src->right - src->left, h = src->bottom - src->top;

        if (w <= 0 || h <= 0 || ax >= (LONG)tex->width || ay >= (LONG)tex->height) {
            rhi_output_restore(&saved);
            rhi_view_destroy(rtv);
            return S_OK;                /* nothing of it lands in the texture */
        }
        if (ax < 0) { w += ax; ax = 0; }
        if (ay < 0) { h += ay; ay = 0; }
        if (ax + w > (LONG)tex->width)  w = (LONG)tex->width - ax;
        if (ay + h > (LONG)tex->height) h = (LONG)tex->height - ay;
        vp.x = (float)ax;
        vp.y = (float)ay;
        vp.width = (float)w;
        vp.height = (float)h;
        c.scale_x = kx;
        c.scale_y = ky;
        c.off_x = (float)src->left * kx - (float)ax * kx;
        c.off_y = (float)src->top * ky - (float)ay * ky;
        whole = ax == 0 && ay == 0 && w == (LONG)tex->width && h == (LONG)tex->height;
    }
    copy_into(rtv, vp, &c, back_w, back_h);

    /* And at the scene's resolution (see the top of the file): the same
     * rectangle, as many times larger as the large image is; source pixels
     * per destination pixel shrink by as much, and the offset, in scene
     * pixels, stays. */
    hi = hires_for(tex, back_w, back_h);
    if (hi) {
        float hx = (float)hi->w / (float)tex->width;
        float hy = (float)hi->h / (float)tex->height;
        ScreenCopyConstants ch = c;
        RhiViewport hv = vp;

        hv.x = vp.x * hx;
        hv.y = vp.y * hy;
        hv.width = vp.width * hx;
        hv.height = vp.height * hy;
        ch.scale_x = c.scale_x / hx;
        ch.scale_y = c.scale_y / hy;
        copy_into(hi->rtv, hv, &ch, back_w, back_h);
        /* A part copied into an image that has not had a whole frame yet
         * leaves the rest of it stale, so only a whole copy makes it the
         * one sampled; once it is, a part keeps both in step. */
        if (whole && !hi->active) {
            hi->own_srv = tex->srv;
            tex->srv = hi->srv;
            hi->active = 1;
        }
    }
    rhi_output_restore(&saved);
    rhi_view_destroy(rtv);
    /* D3D11 unbinds a stage reading an image bound as a target, and only
     * SetTexture binds stages again: put them back, the large image where
     * it stands in. */
    d3d8_rebind_stage_textures();
    g.copies++;
    /* A draw sampling this is re-presenting the frame, and has to cover
     * the whole of it; see d3d8_draw_escapes_squeeze. */
    tex->screen_copy = TRUE;
    return S_OK;
}

unsigned long xbox_D3D8ScreenCopyCount(void) { return g.copies; }

void xbox_D3D8ScreenCopyShutdown(void)
{
    g.back_srv = NULL;
    rhi_shader_destroy(g.vs);            g.vs = NULL;
    rhi_shader_destroy(g.ps);            g.ps = NULL;
    rhi_buffer_destroy(g.cb);            g.cb = NULL;
    rhi_blend_state_destroy(g.blend);    g.blend = NULL;
    rhi_depth_state_destroy(g.depth);    g.depth = NULL;
    rhi_raster_state_destroy(g.raster);  g.raster = NULL;
    g.tried = 0;
}
