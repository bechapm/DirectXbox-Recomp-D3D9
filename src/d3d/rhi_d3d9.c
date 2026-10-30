/**
 * rhi_d3d9.c -- the Direct3D 9 backend of rhi.h, for NVIDIA RTX Remix.
 *
 * Remix is a Direct3D 9 runtime (dxvk-remix): put its d3d9.dll beside the
 * executable and every D3D9 call this backend makes goes to it, and it
 * path-traces the scene those calls describe. The 64-bit runtime is used
 * directly, without the bridge that 32-bit titles need. Without Remix the
 * system's own d3d9.dll draws, which is how this backend is checked against
 * the others. RECOMP_D3D8_BACKEND=d3d9 chooses it.
 * docs/technical/d3d9-remix.md is the design; rhi_d3d9.h is what the
 * renderer does differently for it.
 *
 * What D3D9 lacks against D3D11, and what is done instead:
 *
 *  - No views. A sampled view is its image plus a first mip level (applied
 *    as D3DSAMP_MAXMIPLEVEL); a render-target or depth view is one surface.
 *  - No constant buffers. Each buffer bound with rhi_set_uniform_buffers is
 *    a CPU copy, uploaded before a draw to a fixed range of constant
 *    registers per (stage, slot) -- the ranges the Shader Model 3 HLSL in
 *    src/d3d declares (RHI_D3D9_*_BASE below).
 *  - Shaders are compiled to vs_3_0 / ps_3_0 with RHI_D3D9 defined, which
 *    the renderer's HLSL tests to give D3D9 its own declarations. A null
 *    shader is the fixed-function pipeline, which is how the renderer draws
 *    a fixed-function draw here (rhi_d3d9.h).
 *  - No SV_VertexID: a draw with no vertex layout reads a float vertex
 *    number from an internal stream (RHI_VERTEX_ID in the HLSL).
 *  - An index buffer's format belongs to the buffer, and its offset is in
 *    indices. Index data is kept on the CPU and each indexed draw copies the
 *    indices it uses into a streaming buffer of the right width.
 *  - Rasterisation is D3D9's: pixel centres half a pixel from D3D10's, and
 *    a viewport that must lie inside its target. Both are corrected in clip
 *    space, by a constant every vertex shader applies (RHI_VP_ADJUST, at
 *    c255) and by the projection a fixed-function draw is given.
 *  - No depth clamp: D3D9 always clips to 0 <= z <= w. Where the raster
 *    state asks for clamping (depth_clip 0, the title's draws by default)
 *    RHI_VP_ADJUST clamps each vertex's z into that range instead (flag at
 *    c254), so geometry past the far plane is drawn at it, not cut away.
 *    A vertex behind the eye is left as it is: giving it z = w / 2 kept an
 *    edge to it inside 0 <= z <= w only at that vertex, and the floor under
 *    the eye, which reaches w = 0 on screen, was cut away by the far plane.
 *  - Formats D3D9 has no equal of are converted on the way in (R8G8B8A8 to
 *    A8R8G8B8, R8G8 and the 16-bit one- and two-channel formats widened), so
 *    every image samples as it does under D3D11.
 *  - No device Reset: the back buffer stays the size it was made, which is
 *    the scene's size (rhi_d3d9_present_scene), and Present stretches it to
 *    the window.
 */

#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rhi_backend.h"
#include "../hle/recomp_mod.h"      /* what a mod is told of each draw */
#include "rhi_d3d9.h"
#include "rhi_shader_cache.h"

/* d3d8_compile.c: every backend shows each HLSL source to it first. */
void d3d8_hlsl_note(const char *src, size_t len, const char *name,
                    const RhiMacro *macros, const char *entry, const char *target);

#ifndef D3DFMT_INTZ
#define D3DFMT_INTZ ((D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'))
#endif
#ifndef D3DFMT_ATI2
#define D3DFMT_ATI2 ((D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '2'))
#endif

/* Constant registers each (stage, uniform slot) is uploaded to. The HLSL
 * under RHI_D3D9 declares its constants at these registers; see the
 * prelude below. vs_3_0 has 256 float registers, ps_3_0 224. */
#define VS_SLOTS 8
#define PS_SLOTS 8
static const int g_vs_slot_base[VS_SLOTS] = {
    /* b0: read by no vertex shader drawn here. The fixed-function setup
     * binds its transforms there all the same, and uploading them at c232
     * once ran over RHI_VP_ADJUST at c255, without it being sent again:
     * programs after a fixed-function draw were placed by its matrices. */
    -1,
    RHI_D3D9_VSH_CONST_REG,         /* b1: a vertex program's c0-c191 */
    RHI_D3D9_VSH_SCREENSPACE_REG,   /* b2: its screen-space undo (3 registers) */
    RHI_D3D9_VSH_VERTEXDATA_REG,    /* b3: its input registers' current values (20) */
    -1, -1, -1,
    216,    /* b7: the self-contained passes */
};
static const int g_ps_slot_base[PS_SLOTS] = {
    /* b0: the title's pixel stage -- the texture-stage shader (d3d8_shaders.c)
     * or the register combiners (d3d8_combiners.c). From c0: under fixed
     * function the combiners are ps_2_x, which has 32 registers. */
    0,
    -1, -1, -1, -1, -1, -1,
    32,     /* b7: the self-contained passes */
};
#define VS_ADJUST_REG 255   /* RHI_VP_ADJUST */
#define VS_CLAMP_REG  254   /* its depth clamp flag */

/* What every shader is compiled with in front of it. */
static const char kVsPrelude[] =
    "#define RHI_D3D9 1\n"
    "#define RHI_D3D9_VS 1\n"
    "#define RHI_VERTEX_ID TEXCOORD15\n"
    "float4 rhi_vp_adjust : register(c255);\n"
    "float4 rhi_depth_clamp : register(c254);\n";
/* After a mod's own vertex HLSL (rhi_d3d9_mod_vertex_hlsl), which may
 * define RHI_MOD_POSITION(p) to be run on every output position. */
static const char kVsPreludeEnd[] =
    "#ifndef RHI_MOD_POSITION\n"
    "#define RHI_MOD_POSITION(p)\n"
    "#endif\n"
    "void rhi_vp_adjust_pos(inout float4 p)\n"
    "{\n"
    "    p.xy = p.xy * rhi_vp_adjust.xy + rhi_vp_adjust.zw * p.w;\n"
    "    if (rhi_depth_clamp.x != 0 && p.w > 0)\n"
    "        p.z = clamp(p.z, 0, p.w);\n"
    "    RHI_MOD_POSITION(p);\n"
    "}\n"
    "#define RHI_VP_ADJUST(p) rhi_vp_adjust_pos(p)\n"
    "#line 1\n";
static const char kPsPrelude[] =
    "#define RHI_D3D9 1\n"
    "#define RHI_D3D9_PS 1\n"
    /* A pass keeps both stages in one source, and its vertex entry point is
     * parsed in the pixel compile too. */
    "#define RHI_VERTEX_ID TEXCOORD15\n"
    "#define RHI_VP_ADJUST(p)\n"
    "#line 1\n";

/* ---- the objects ------------------------------------------------------------------ */

struct RhiBuffer {
    RhiBufferDesc desc;
    IDirect3DVertexBuffer9 *vb;     /* RHI_BIND_VERTEX */
    uint8_t *cpu;                   /* uniform and index buffers: the contents */
    uint32_t version;               /* bumped each time cpu changes */
    void    *locked;                /* a vertex buffer between map and unmap */
};

/* How texels are converted between the RHI's format and the one stored. */
enum {
    CONV_NONE = 0,
    CONV_SWAP_RB,           /* R8G8B8A8 <-> A8R8G8B8 */
    CONV_RG8_TO_ARGB,       /* R8G8 -> A8R8G8B8 (r, g, 0, 1) */
    CONV_R16_TO_RGBA16,     /* R16 -> A16B16G16R16 (r, 0, 0, 1) */
    CONV_RG16_TO_RGBA16,    /* R16G16 -> A16B16G16R16 (r, g, 0, 1) */
    CONV_R11G11B10_TO_F16,  /* R11G11B10 float -> A16B16G16R16F */
    CONV_SINT_TO_FLOAT,     /* R32G32B32A32 signed integers -> floats */
    CONV_A8_TO_ARGB         /* A8 -> A8R8G8B8 (1, 1, 1, a) */
};

struct RhiImage {
    RhiImageDesc desc;
    LONG     refs;
    D3DFORMAT fmt;
    int      conv;
    uint32_t bpp_in, bpp_out;       /* bytes per texel before and after conv */
    int      block;                 /* block compressed: rows are 4 texels */
    D3DPOOL  pool;
    IDirect3DBaseTexture9 *tex;     /* 2D, cube or volume */
    IDirect3DSurface9     *surf;    /* a depth image that is not sampled */
    int      screen_copy;           /* rhi_d3d9_copy_rect has copied the screen into it */
    uint64_t id;                    /* what it holds, for kind_2d (image_id) */
    int      drawn;                 /* a draw has used it since it was made (remake_texture) */
    uint64_t texels;                /* a hash of all of level 0 as last written whole */
};

struct RhiView {
    RhiImage *image;                /* NULL for the back buffer */
    uint32_t  kind;
    uint32_t  dim;
    uint32_t  base_mip;
    IDirect3DSurface9 *surf;        /* render target or depth */
};

struct RhiShader {
    uint32_t stage;
    IDirect3DVertexShader9 *vs;
    IDirect3DPixelShader9  *ps;
    uint64_t id;                    /* its bytecode's hash, for kind_2d */
};

struct RhiVertexLayout {
    IDirect3DVertexDeclaration9 *decl;
};

struct RhiBlendState  { RhiBlendDesc d; };
struct RhiDepthState  { RhiDepthDesc d; };
struct RhiRasterState { RhiRasterDesc d; };
struct RhiSampler     { RhiSamplerDesc d; };

/* ---- the device ------------------------------------------------------------------------ */

typedef IDirect3D9 *(WINAPI *PFN_Direct3DCreate9)(UINT);

static HMODULE           g_dll;
static IDirect3D9       *g_d3d;
static IDirect3DDevice9 *g_dev;
static D3DPRESENT_PARAMETERS g_pp;
static HWND              g_hwnd;
static RhiView          *g_bb_view;          /* the back buffer */
static int               g_vrr;
static RECT              g_present_rect;     /* where Present puts the back buffer */
static int               g_present_rect_set;
static int               g_in_scene;         /* between BeginScene and EndScene */

/* ---- mods (src/hle/recomp_mod.h) ------------------------------------------------------- */

/* What mods asked to hear of. Registered at load, before the device. */
#define MOD_HOOKS 8
static struct { RecompModBeforeDraw before; RecompModAfterDraw after; void *user; }
                         g_mod_draw[MOD_HOOKS];
static struct { RecompModDeviceFn fn; void *user; } g_mod_device[MOD_HOOKS], g_mod_present[MOD_HOOKS];
static struct { RecompModClearFn fn; void *user; } g_mod_clear[MOD_HOOKS];
static int  g_mod_draw_n, g_mod_device_n, g_mod_present_n, g_mod_clear_n;
static char g_mod_vs_hlsl[4096];    /* added to every vertex program */
static int  g_remake_written;       /* rhi_d3d9_mod_remake_written_textures */

int rhi_d3d9_mod_on_draw(RecompModBeforeDraw before, RecompModAfterDraw after, void *user)
{
    if (g_mod_draw_n >= MOD_HOOKS || (!before && !after))
        return 0;
    g_mod_draw[g_mod_draw_n].before = before;
    g_mod_draw[g_mod_draw_n].after = after;
    g_mod_draw[g_mod_draw_n].user = user;
    g_mod_draw_n++;
    return 1;
}

int rhi_d3d9_mod_on_device(RecompModDeviceFn fn, void *user)
{
    if (g_mod_device_n >= MOD_HOOKS || !fn)
        return 0;
    g_mod_device[g_mod_device_n].fn = fn;
    g_mod_device[g_mod_device_n++].user = user;
    return 1;
}

int rhi_d3d9_mod_on_present(RecompModDeviceFn fn, void *user)
{
    if (g_mod_present_n >= MOD_HOOKS || !fn)
        return 0;
    g_mod_present[g_mod_present_n].fn = fn;
    g_mod_present[g_mod_present_n++].user = user;
    return 1;
}

int rhi_d3d9_mod_on_clear(RecompModClearFn fn, void *user)
{
    if (g_mod_clear_n >= MOD_HOOKS || !fn)
        return 0;
    g_mod_clear[g_mod_clear_n].fn = fn;
    g_mod_clear[g_mod_clear_n++].user = user;
    return 1;
}

int rhi_d3d9_mod_vertex_hlsl(const char *hlsl)
{
    size_t have = strlen(g_mod_vs_hlsl), n = hlsl ? strlen(hlsl) : 0;

    if (have + n + 2 > sizeof g_mod_vs_hlsl)
        return 0;
    memcpy(g_mod_vs_hlsl + have, hlsl, n);
    g_mod_vs_hlsl[have + n] = '\n';
    g_mod_vs_hlsl[have + n + 1] = 0;
    return 1;
}

void rhi_d3d9_mod_remake_written_textures(int on)
{
    g_remake_written = on != 0;
}

static void mod_cleared(uint32_t what, float z, int whole)
{
    int i;

    for (i = 0; i < g_mod_clear_n; i++)
        g_mod_clear[i].fn(what, z, whole, g_mod_clear[i].user);
}

/* D3D9 draws only inside BeginScene/EndScene, and refuses a draw outside one
 * (D3DERR_INVALIDCALL). The first draw opens a scene; Present, and the
 * copies and readbacks, close it first (StretchRect involving a depth
 * surface must be outside one, and a readback wants the frame's draws). */
static void scene_begin(void)
{
    if (!g_in_scene && g_dev && SUCCEEDED(IDirect3DDevice9_BeginScene(g_dev)))
        g_in_scene = 1;
}

static void scene_end(void)
{
    if (g_in_scene) {
        IDirect3DDevice9_EndScene(g_dev);
        g_in_scene = 0;
    }
}

/* The internal streams: vertex numbers for draws with no layout, and the
 * index ring each indexed draw copies its indices into. */
#define VID_COUNT   256u
#define IRING_BYTES (4u << 20)
static IDirect3DVertexBuffer9      *g_vid_vb;
static IDirect3DVertexDeclaration9 *g_vid_decl;
static IDirect3DIndexBuffer9       *g_iring[2];     /* [0] 16-bit, [1] 32-bit */
static uint32_t                     g_iring_pos[2];

/* What is bound, as rhi.h set it. The parts D3D9 keeps elsewhere (the
 * stencil faces follow the cull winding, the uniform buffers become
 * registers, the viewport is clamped) are applied at the draw. */
static struct {
    RhiShader       *vs, *ps;
    RhiVertexLayout *layout;
    RhiBuffer       *vb[16];
    uint32_t         vb_stride[16], vb_offset[16];
    RhiBuffer       *ib;
    uint32_t         ib_bits, ib_offset;
    uint32_t         topology;
    RhiBuffer       *ub[2][VS_SLOTS];
    RhiBuffer       *ub_sent[2][VS_SLOTS];
    uint32_t         ub_sent_version[2][VS_SLOTS];
    RhiView         *tex[16];
    RhiSampler      *samp[16];
    int              samp_dirty;
    RhiView         *rt, *ds;
    RhiViewport      vp;
    int              vp_dirty;
    RECT             vp_pixels;          /* the pixels D3D11 would draw in vp */
    int              vp_clip;            /* and D3D9's viewport covers more */
    RhiRect          scissor;
    int              scissor_dirty;
    int              raster_scissor;     /* the raster state's scissor test */
    RhiDepthState   *depth;
    uint32_t         stencil_ref;
    RhiRasterState  *raster;
    int              ds_dirty;
    float            adjust[4];          /* RHI_VP_ADJUST for the current viewport */
    int              adjust_sent;
    float            clamp[4];           /* and its depth clamp flag */
    int              clamp_sent;
    int              vid_bound;
} g_b;

/* Cached device state, so the renderer can set the same value every draw
 * (it does: d3d8_states_apply) without each one reaching the runtime. */
#define RS_COUNT 256
#define TSS_COUNT 33
#define SAMP_COUNT 14
static DWORD   g_rs[RS_COUNT];
static uint8_t g_rs_known[RS_COUNT];
static DWORD   g_tss[8][TSS_COUNT];
static uint8_t g_tss_known[8][TSS_COUNT];
static DWORD   g_samp[16][SAMP_COUNT];
static uint8_t g_samp_known[16][SAMP_COUNT];

/* The fixed-function projection as the renderer gave it; the device gets
 * it with the viewport correction applied (apply_projection). */
static int     g_title_draw;          /* rhi_d3d9_title_draw: the next draw is the title's */
static int     g_draw_title;          /* it was, for the draw in hand (mod_before_draw) */
static int     g_title_2d;            /* and is screen-space */
static int     g_title_said_2d;       /* the title's own test said so (rhi_d3d9_title_says_2d) */
static float   g_title_2d_w;          /* and the draw's clip w, or 0 */
static float   g_dev_world[16], g_dev_view[16];  /* WORLD and VIEW as the device has them */
static int     g_title_offscreen;     /* the title's draw goes to a texture (rhi_d3d9_title_draw) */
static DWORD   g_title_alpha_test;    /* its D3DRS_ALPHATESTENABLE */
static float   g_ff_proj[16];
static int     g_ff_proj_set, g_ff_proj_dirty;

static void set_rs(D3DRENDERSTATETYPE s, DWORD v)
{
    if ((unsigned)s < RS_COUNT) {
        if (g_rs_known[s] && g_rs[s] == v)
            return;
        g_rs_known[s] = 1;
        g_rs[s] = v;
    }
    IDirect3DDevice9_SetRenderState(g_dev, s, v);
}

static void set_tss(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v)
{
    if (stage < 8 && (unsigned)t < TSS_COUNT) {
        if (g_tss_known[stage][t] && g_tss[stage][t] == v)
            return;
        g_tss_known[stage][t] = 1;
        g_tss[stage][t] = v;
    }
    IDirect3DDevice9_SetTextureStageState(g_dev, stage, t, v);
}

static void set_samp(DWORD sampler, D3DSAMPLERSTATETYPE t, DWORD v)
{
    if (sampler < 16 && (unsigned)t < SAMP_COUNT) {
        if (g_samp_known[sampler][t] && g_samp[sampler][t] == v)
            return;
        g_samp_known[sampler][t] = 1;
        g_samp[sampler][t] = v;
    }
    IDirect3DDevice9_SetSamplerState(g_dev, sampler, t, v);
}

static DWORD f2dw(float f) { DWORD d; memcpy(&d, &f, 4); return d; }

static DWORD to_d3dcolor(const float c[4])
{
    int i;
    DWORD v[4];
    for (i = 0; i < 4; i++) {
        float x = c[i] < 0.0f ? 0.0f : c[i] > 1.0f ? 1.0f : c[i];
        v[i] = (DWORD)(x * 255.0f + 0.5f);
    }
    return D3DCOLOR_ARGB(v[3], v[0], v[1], v[2]);
}

int rhi_d3d9_active(void)
{
    return g_rhi == &rhi_d3d9_backend && g_dev != NULL;
}


static void forget_state(void)
{
    memset(g_rs_known, 0, sizeof g_rs_known);
    memset(g_tss_known, 0, sizeof g_tss_known);
    memset(g_samp_known, 0, sizeof g_samp_known);
    memset(&g_b, 0, sizeof g_b);
    g_b.vp_dirty = g_b.scissor_dirty = g_b.ds_dirty = g_b.samp_dirty = 1;
    g_ff_proj_set = g_ff_proj_dirty = 0;
}

static RhiView *wrap_surface(IDirect3DSurface9 *s, uint32_t kind)
{
    RhiView *v = calloc(1, sizeof *v);
    if (!v)
        return NULL;
    v->kind = kind;
    v->surf = s;
    return v;
}

static int make_internal_streams(void)
{
    static const D3DVERTEXELEMENT9 vid_el[] = {
        { 0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 15 },
        D3DDECL_END()
    };
    float *p;
    uint32_t i;

    if (FAILED(IDirect3DDevice9_CreateVertexBuffer(g_dev, VID_COUNT * 4u, D3DUSAGE_WRITEONLY, 0,
                                                   D3DPOOL_MANAGED, &g_vid_vb, NULL)))
        return -1;
    if (FAILED(IDirect3DVertexBuffer9_Lock(g_vid_vb, 0, 0, (void **)&p, 0)))
        return -1;
    for (i = 0; i < VID_COUNT; i++)
        p[i] = (float)i;
    IDirect3DVertexBuffer9_Unlock(g_vid_vb);
    if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(g_dev, vid_el, &g_vid_decl)))
        return -1;
    if (FAILED(IDirect3DDevice9_CreateIndexBuffer(g_dev, IRING_BYTES,
                                                  D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                                  D3DFMT_INDEX16, D3DPOOL_DEFAULT,
                                                  &g_iring[0], NULL)))
        return -1;
    if (FAILED(IDirect3DDevice9_CreateIndexBuffer(g_dev, IRING_BYTES,
                                                  D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                                  D3DFMT_INDEX32, D3DPOOL_DEFAULT,
                                                  &g_iring[1], NULL)))
        return -1;
    g_iring_pos[0] = g_iring_pos[1] = IRING_BYTES;    /* the first use discards */
    return 0;
}

static int d9_device_create(const RhiDeviceDesc *dd)
{
    PFN_Direct3DCreate9 create;
    IDirect3DSurface9 *bb = NULL;
    D3DADAPTER_IDENTIFIER9 id;
    HRESULT hr;
    DWORD flags;

    if (!dd->window) {
        fprintf(stderr, "[RHI] d3d9: needs a window\n");
        return -1;
    }
    /* By name, so the d3d9.dll beside the executable -- Remix's -- is the
     * one found before the system's. */
    g_dll = LoadLibraryA("d3d9.dll");
    create = g_dll ? (PFN_Direct3DCreate9)(void *)GetProcAddress(g_dll, "Direct3DCreate9") : NULL;
    if (!create || !(g_d3d = create(D3D_SDK_VERSION))) {
        fprintf(stderr, "[RHI] d3d9: no Direct3D 9 (d3d9.dll %s)\n",
                g_dll ? "has no Direct3DCreate9 that works" : "not found");
        return -1;
    }

    g_hwnd = (HWND)dd->window;
    memset(&g_pp, 0, sizeof g_pp);
    g_pp.BackBufferWidth = dd->width;
    g_pp.BackBufferHeight = dd->height;
    g_pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    g_pp.BackBufferCount = 1;
    g_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_pp.hDeviceWindow = g_hwnd;
    /* Always windowed: the window's owner makes it fill the screen, as it
     * does for the other backends, and a windowed device is never lost. */
    g_pp.Windowed = TRUE;
    g_pp.EnableAutoDepthStencil = FALSE;
    g_pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    /* The renderer is called from more than one thread (the HLE's guest
     * threads, under the guest lock), so the device is made thread safe. */
    flags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED |
            D3DCREATE_FPU_PRESERVE;
    hr = IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd, flags,
                                 &g_pp, &g_dev);
    if (FAILED(hr)) {
        flags = (flags & ~(DWORD)D3DCREATE_HARDWARE_VERTEXPROCESSING) |
                D3DCREATE_SOFTWARE_VERTEXPROCESSING;
        hr = IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd, flags,
                                     &g_pp, &g_dev);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[RHI] d3d9: CreateDevice failed (0x%08lX)\n", (unsigned long)hr);
        g_dev = NULL;
        return -1;
    }
    if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(g_d3d, D3DADAPTER_DEFAULT, 0, &id)))
        fprintf(stderr, "[RHI] d3d9: %s, %ux%u back buffer (%s)\n", id.Description,
                dd->width, dd->height, id.Driver);
    fflush(stderr);

    forget_state();
    if (FAILED(IDirect3DDevice9_GetBackBuffer(g_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) ||
        !(g_bb_view = wrap_surface(bb, RHI_VIEW_RENDER_TARGET))) {
        fprintf(stderr, "[RHI] d3d9: no back buffer\n");
        if (bb) IDirect3DSurface9_Release(bb);
        return -1;
    }
    if (make_internal_streams() != 0) {
        fprintf(stderr, "[RHI] d3d9: the internal streams could not be made\n");
        return -1;
    }
    /* D3D9's defaults that differ from what the renderer assumes: lighting
     * on, and no separate alpha blend. */
    set_rs(D3DRS_LIGHTING, FALSE);
    set_rs(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    set_rs(D3DRS_TWOSIDEDSTENCILMODE, TRUE);
    {
        int i;

        for (i = 0; i < g_mod_device_n; i++)
            g_mod_device[i].fn(g_dev, g_mod_device[i].user);
    }
    return 0;
}

static void d9_device_destroy(void)
{
    int i;

    scene_end();
    if (g_bb_view) {
        if (g_bb_view->surf) IDirect3DSurface9_Release(g_bb_view->surf);
        free(g_bb_view);
        g_bb_view = NULL;
    }
    for (i = 0; i < 2; i++)
        if (g_iring[i]) { IDirect3DIndexBuffer9_Release(g_iring[i]); g_iring[i] = NULL; }
    if (g_vid_decl) { IDirect3DVertexDeclaration9_Release(g_vid_decl); g_vid_decl = NULL; }
    if (g_vid_vb)   { IDirect3DVertexBuffer9_Release(g_vid_vb); g_vid_vb = NULL; }
    if (g_dev) { IDirect3DDevice9_Release(g_dev); g_dev = NULL; }
    if (g_d3d) { IDirect3D9_Release(g_d3d); g_d3d = NULL; }
    /* d3d9.dll stays loaded: Remix does not expect to be unloaded. */
}

static int d9_device_ready(void)      { return g_dev != NULL; }
static RhiView *d9_swapchain_view(void) { return g_bb_view; }

/* The back buffer keeps its size (no Reset, which would mean releasing and
 * remaking every render target the title holds); Present stretches it. */
static int d9_swapchain_resize(uint32_t w, uint32_t h)
{
    (void)w; (void)h;
    return 0;
}

static void d9_swapchain_set_vrr(int on)
{
    g_vrr = on ? 1 : 0;
}

static int32_t d9_present(uint32_t interval)
{
    RECT client;
    HRESULT hr;
    int h;

    if (!g_dev)
        return -1;
    (void)interval;     /* the flip gate paces frames; D3D9 presents at once */
    /* Bars: the parts of the client area the picture does not cover. */
    if (g_present_rect_set && GetClientRect(g_hwnd, &client)) {
        if (g_present_rect.left > 0 || g_present_rect.top > 0 ||
            g_present_rect.right < client.right || g_present_rect.bottom < client.bottom) {
            HDC dc = GetDC(g_hwnd);
            if (dc) {
                HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
                RECT r;
                r = client; r.right = g_present_rect.left;   FillRect(dc, &r, black);
                r = client; r.left = g_present_rect.right;   FillRect(dc, &r, black);
                r = client; r.bottom = g_present_rect.top;   FillRect(dc, &r, black);
                r = client; r.top = g_present_rect.bottom;   FillRect(dc, &r, black);
                ReleaseDC(g_hwnd, dc);
            }
        }
    }
    scene_end();
    {
        int i;

        for (i = 0; i < g_mod_present_n; i++)
            g_mod_present[i].fn(g_dev, g_mod_present[i].user);
    }
    h = rhi_wait_begin();
    hr = IDirect3DDevice9_Present(g_dev, NULL, g_present_rect_set ? &g_present_rect : NULL,
                                  NULL, NULL);
    rhi_wait_end(h);
    if (hr == D3DERR_DEVICELOST) {
        static int said;
        if (!said++)
            fprintf(stderr, "[RHI] d3d9: the device was lost; frames are dropped until "
                    "it comes back\n");
    }
    return (int32_t)hr;
}

/* ---- format tables ----------------------------------------------------------------- */

static int map_format(RhiFormat f, uint32_t bind, D3DFORMAT *out, int *conv,
                      uint32_t *bpp_in, uint32_t *bpp_out, int *block)
{
    *conv = CONV_NONE;
    *block = 0;
    if (bind & RHI_BIND_DEPTH) {
        int sampled = (bind & RHI_BIND_SAMPLED) != 0;
        *bpp_in = *bpp_out = 4;
        switch (f) {
        case RHI_FORMAT_D24_UNORM_S8_UINT: *out = sampled ? D3DFMT_INTZ : D3DFMT_D24S8; return 0;
        case RHI_FORMAT_D16_UNORM:         *out = sampled ? D3DFMT_INTZ : D3DFMT_D16; *bpp_in = 2; return 0;
        case RHI_FORMAT_D32_FLOAT:         *out = sampled ? D3DFMT_INTZ : D3DFMT_D32F_LOCKABLE; return 0;
        default: break;
        }
    }
    switch (f) {
    case RHI_FORMAT_B8G8R8A8_UNORM: *out = D3DFMT_A8R8G8B8; *bpp_in = *bpp_out = 4; return 0;
    case RHI_FORMAT_B8G8R8X8_UNORM: *out = D3DFMT_X8R8G8B8; *bpp_in = *bpp_out = 4; return 0;
    case RHI_FORMAT_R8G8B8A8_UNORM:
        *out = D3DFMT_A8R8G8B8; *conv = CONV_SWAP_RB; *bpp_in = *bpp_out = 4; return 0;
    case RHI_FORMAT_B5G6R5_UNORM:   *out = D3DFMT_R5G6B5;   *bpp_in = *bpp_out = 2; return 0;
    case RHI_FORMAT_B5G5R5A1_UNORM: *out = D3DFMT_A1R5G5B5; *bpp_in = *bpp_out = 2; return 0;
    case RHI_FORMAT_B4G4R4A4_UNORM: *out = D3DFMT_A4R4G4B4; *bpp_in = *bpp_out = 2; return 0;
    case RHI_FORMAT_A8_UNORM:
        /* White, as the Xbox samples A8 (and so as D3D9's own fixed-function
         * stages read it, which the renderer draws through here): DXGI's A8
         * is black, which the D3D11 renderer corrects in its shaders. */
        *out = D3DFMT_A8R8G8B8; *conv = CONV_A8_TO_ARGB; *bpp_in = 1; *bpp_out = 4; return 0;
    case RHI_FORMAT_R8_UNORM:
        /* (r, 0, 0, 1) as D3D11 samples it: widened like R8G8 with g = 0. */
        *out = D3DFMT_A8R8G8B8; *conv = CONV_RG8_TO_ARGB; *bpp_in = 1; *bpp_out = 4; return 0;
    case RHI_FORMAT_R8G8_UNORM:
        *out = D3DFMT_A8R8G8B8; *conv = CONV_RG8_TO_ARGB; *bpp_in = 2; *bpp_out = 4; return 0;
    case RHI_FORMAT_R16_UNORM:
        *out = D3DFMT_A16B16G16R16; *conv = CONV_R16_TO_RGBA16; *bpp_in = 2; *bpp_out = 8; return 0;
    case RHI_FORMAT_R16G16_UNORM:
        *out = D3DFMT_A16B16G16R16; *conv = CONV_RG16_TO_RGBA16; *bpp_in = 4; *bpp_out = 8; return 0;
    case RHI_FORMAT_R16G16B16A16_UNORM: *out = D3DFMT_A16B16G16R16; *bpp_in = *bpp_out = 8; return 0;
    case RHI_FORMAT_R8G8B8A8_SNORM:     *out = D3DFMT_Q8W8V8U8;     *bpp_in = *bpp_out = 4; return 0;
    case RHI_FORMAT_R8G8_SNORM:         *out = D3DFMT_V8U8;         *bpp_in = *bpp_out = 2; return 0;
    case RHI_FORMAT_R16G16_SNORM:       *out = D3DFMT_V16U16;       *bpp_in = *bpp_out = 4; return 0;
    case RHI_FORMAT_R16G16B16A16_SNORM: *out = D3DFMT_Q16W16V16U16; *bpp_in = *bpp_out = 8; return 0;
    case RHI_FORMAT_R16_FLOAT:          *out = D3DFMT_R16F;         *bpp_in = *bpp_out = 2; return 0;
    case RHI_FORMAT_R32_FLOAT:          *out = D3DFMT_R32F;         *bpp_in = *bpp_out = 4; return 0;
    case RHI_FORMAT_R16G16_FLOAT:       *out = D3DFMT_G16R16F;      *bpp_in = *bpp_out = 4; return 0;
    case RHI_FORMAT_R32G32_FLOAT:       *out = D3DFMT_G32R32F;      *bpp_in = *bpp_out = 8; return 0;
    case RHI_FORMAT_R16G16B16A16_FLOAT: *out = D3DFMT_A16B16G16R16F; *bpp_in = *bpp_out = 8; return 0;
    case RHI_FORMAT_R32G32B32A32_FLOAT: *out = D3DFMT_A32B32G32R32F; *bpp_in = *bpp_out = 16; return 0;
    case RHI_FORMAT_R32G32B32A32_SINT:
        *out = D3DFMT_A32B32G32R32F; *conv = CONV_SINT_TO_FLOAT; *bpp_in = *bpp_out = 16; return 0;
    case RHI_FORMAT_R10G10B10A2_UNORM:  *out = D3DFMT_A2B10G10R10; *bpp_in = *bpp_out = 4; return 0;
    case RHI_FORMAT_R11G11B10_FLOAT:
        *out = D3DFMT_A16B16G16R16F; *conv = CONV_R11G11B10_TO_F16; *bpp_in = 4; *bpp_out = 8; return 0;
    case RHI_FORMAT_BC1_UNORM: *out = D3DFMT_DXT1; *bpp_in = *bpp_out = 8;  *block = 1; return 0;
    case RHI_FORMAT_BC2_UNORM: *out = D3DFMT_DXT3; *bpp_in = *bpp_out = 16; *block = 1; return 0;
    case RHI_FORMAT_BC3_UNORM: *out = D3DFMT_DXT5; *bpp_in = *bpp_out = 16; *block = 1; return 0;
    case RHI_FORMAT_BC5_UNORM: *out = D3DFMT_ATI2; *bpp_in = *bpp_out = 16; *block = 1; return 0;
    default:
        return -1;
    }
}

/* A half float from a float, rounding to nearest; enough for colour. */
static uint16_t f32_to_f16(float f)
{
    uint32_t x, sign, mant;
    int exp;

    memcpy(&x, &f, 4);
    sign = (x >> 16) & 0x8000u;
    exp = (int)((x >> 23) & 0xFF) - 127 + 15;
    mant = x & 0x7FFFFFu;
    if (exp <= 0)
        return (uint16_t)sign;
    if (exp >= 31)
        return (uint16_t)(sign | 0x7C00u);
    return (uint16_t)(sign | ((uint32_t)exp << 10) | ((mant + 0x1000u) >> 13));
}

static float small_float(uint32_t bits, int mant_bits)
{
    uint32_t e = bits >> mant_bits, m = bits & ((1u << mant_bits) - 1u);
    if (e == 0)
        return ldexpf((float)m, -14 - mant_bits);
    if (e == 31)
        return 65504.0f;
    return ldexpf(1.0f + (float)m / (float)(1u << mant_bits), (int)e - 15);
}

/* One row of `w` texels from the RHI's layout into the stored one. */
static void convert_row(int conv, uint8_t *dst, const uint8_t *src, uint32_t w)
{
    uint32_t x;

    switch (conv) {
    case CONV_SWAP_RB:
        for (x = 0; x < w; x++) {
            dst[4 * x + 0] = src[4 * x + 2];
            dst[4 * x + 1] = src[4 * x + 1];
            dst[4 * x + 2] = src[4 * x + 0];
            dst[4 * x + 3] = src[4 * x + 3];
        }
        break;
    case CONV_RG8_TO_ARGB:
        /* bpp_in says whether there is a green; see the R8 entry. */
        break;
    case CONV_R16_TO_RGBA16:
        for (x = 0; x < w; x++) {
            uint16_t *d = (uint16_t *)(dst + 8 * x);
            memcpy(&d[0], src + 2 * x, 2);
            d[1] = 0; d[2] = 0; d[3] = 0xFFFF;
        }
        break;
    case CONV_RG16_TO_RGBA16:
        for (x = 0; x < w; x++) {
            uint16_t *d = (uint16_t *)(dst + 8 * x);
            memcpy(&d[0], src + 4 * x, 4);
            d[2] = 0; d[3] = 0xFFFF;
        }
        break;
    case CONV_R11G11B10_TO_F16:
        for (x = 0; x < w; x++) {
            uint32_t v;
            uint16_t *d = (uint16_t *)(dst + 8 * x);
            memcpy(&v, src + 4 * x, 4);
            d[0] = f32_to_f16(small_float(v & 0x7FFu, 6));
            d[1] = f32_to_f16(small_float((v >> 11) & 0x7FFu, 6));
            d[2] = f32_to_f16(small_float((v >> 22) & 0x3FFu, 5));
            d[3] = 0x3C00;  /* 1.0 */
        }
        break;
    case CONV_A8_TO_ARGB:
        for (x = 0; x < w; x++) {
            dst[4 * x + 0] = dst[4 * x + 1] = dst[4 * x + 2] = 0xFF;
            dst[4 * x + 3] = src[x];
        }
        break;
    case CONV_SINT_TO_FLOAT:
        for (x = 0; x < 4 * w; x++) {
            int32_t i;
            float f;
            memcpy(&i, src + 4 * x, 4);
            f = (float)i;
            memcpy(dst + 4 * x, &f, 4);
        }
        break;
    default:
        break;
    }
}

static void convert_row_img(const RhiImage *img, uint8_t *dst, const uint8_t *src, uint32_t w)
{
    if (img->conv == CONV_RG8_TO_ARGB) {
        uint32_t x;
        for (x = 0; x < w; x++) {
            uint8_t r = src[img->bpp_in * x];
            uint8_t g = img->bpp_in > 1 ? src[img->bpp_in * x + 1] : 0;
            dst[4 * x + 0] = 0;     /* b */
            dst[4 * x + 1] = g;
            dst[4 * x + 2] = r;
            dst[4 * x + 3] = 0xFF;
        }
        return;
    }
    if (img->conv == CONV_NONE)
        memcpy(dst, src, (size_t)w * img->bpp_in);
    else
        convert_row(img->conv, dst, src, w);
}

/* ---- buffers ----------------------------------------------------------------------- */

static RhiBuffer *d9_buffer_create(const RhiBufferDesc *d, const void *initial)
{
    RhiBuffer *b = calloc(1, sizeof *b);

    if (!b)
        return NULL;
    b->desc = *d;
    if (d->bind & RHI_BIND_VERTEX) {
        int dynamic = d->usage == RHI_USAGE_DYNAMIC;
        HRESULT hr = IDirect3DDevice9_CreateVertexBuffer(
            g_dev, d->size, dynamic ? (D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY) : D3DUSAGE_WRITEONLY,
            0, dynamic ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED, &b->vb, NULL);
        if (FAILED(hr)) {
            free(b);
            return NULL;
        }
        if (initial) {
            void *p;
            if (SUCCEEDED(IDirect3DVertexBuffer9_Lock(b->vb, 0, 0, &p,
                                                      dynamic ? D3DLOCK_DISCARD : 0))) {
                memcpy(p, initial, d->size);
                IDirect3DVertexBuffer9_Unlock(b->vb);
            }
        }
        return b;
    }
    /* Index and uniform buffers live on the CPU. */
    b->cpu = calloc(1, d->size ? d->size : 1);
    if (!b->cpu) {
        free(b);
        return NULL;
    }
    if (initial)
        memcpy(b->cpu, initial, d->size);
    b->version = 1;
    return b;
}

static void d9_buffer_destroy(RhiBuffer *b)
{
    int s, i;

    for (s = 0; s < 2; s++)
        for (i = 0; i < VS_SLOTS; i++) {
            if (g_b.ub[s][i] == b) g_b.ub[s][i] = NULL;
            if (g_b.ub_sent[s][i] == b) g_b.ub_sent[s][i] = NULL;
        }
    for (i = 0; i < 16; i++)
        if (g_b.vb[i] == b) {
            g_b.vb[i] = NULL;
            if (!(i == 0 && g_b.vid_bound))
                IDirect3DDevice9_SetStreamSource(g_dev, i, NULL, 0, 0);
        }
    if (g_b.ib == b)
        g_b.ib = NULL;
    if (b->vb) IDirect3DVertexBuffer9_Release(b->vb);
    free(b->cpu);
    free(b);
}

static void *d9_buffer_map(RhiBuffer *b, uint32_t mode)
{
    if (!b)
        return NULL;
    if (b->vb) {
        void *p = NULL;
        DWORD flags = mode == RHI_MAP_WRITE_NO_OVERWRITE ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD;
        if (b->desc.usage != RHI_USAGE_DYNAMIC)
            flags = 0;
        if (FAILED(IDirect3DVertexBuffer9_Lock(b->vb, 0, 0, &p, flags)))
            return NULL;
        b->locked = p;
        return p;
    }
    return b->cpu;
}

static void d9_buffer_unmap(RhiBuffer *b)
{
    if (!b)
        return;
    if (b->vb) {
        if (b->locked)
            IDirect3DVertexBuffer9_Unlock(b->vb);
        b->locked = NULL;
        return;
    }
    b->version++;
}

static void d9_buffer_update(RhiBuffer *b, const void *data)
{
    if (!b)
        return;
    if (b->vb) {
        void *p;
        if (SUCCEEDED(IDirect3DVertexBuffer9_Lock(b->vb, 0, 0, &p,
                                                  b->desc.usage == RHI_USAGE_DYNAMIC
                                                      ? D3DLOCK_DISCARD : 0))) {
            memcpy(p, data, b->desc.size);
            IDirect3DVertexBuffer9_Unlock(b->vb);
        }
        return;
    }
    memcpy(b->cpu, data, b->desc.size);
    b->version++;
}

/* ---- images ------------------------------------------------------------------------ */

static RhiImage *d9_image_create(const RhiImageDesc *d, const RhiSubresourceData *initial);
static uint64_t image_id(const RhiImage *img, const void *data, size_t bytes);
static void d9_image_update(RhiImage *img, uint32_t sub, const RhiBox *box,
                            const void *data, uint32_t row_pitch, uint32_t slice_pitch);

static RhiImage *d9_image_create(const RhiImageDesc *d, const RhiSubresourceData *initial)
{
    RhiImage *img = calloc(1, sizeof *img);
    uint32_t levels = d->mip_levels ? d->mip_levels : 1;
    DWORD usage = 0;
    HRESULT hr = E_FAIL;

    if (!img)
        return NULL;
    img->desc = *d;
    img->desc.samples = 1;      /* no multisampling here: Remix renders its own */
    img->desc.sample_quality = 0;
    img->refs = 1;
    img->id = image_id(img, NULL, 0);
    if (map_format(d->format, d->bind, &img->fmt, &img->conv, &img->bpp_in, &img->bpp_out,
                   &img->block) != 0) {
        fprintf(stderr, "[RHI] d3d9: no D3D9 format for DXGI format %u\n", (unsigned)d->format);
        free(img);
        return NULL;
    }

    if (d->bind & RHI_BIND_RENDER_TARGET) {
        usage = D3DUSAGE_RENDERTARGET;
        img->pool = D3DPOOL_DEFAULT;
    } else if (d->bind & RHI_BIND_DEPTH) {
        usage = D3DUSAGE_DEPTHSTENCIL;
        img->pool = D3DPOOL_DEFAULT;
    } else if (d->usage == RHI_USAGE_DYNAMIC) {
        usage = D3DUSAGE_DYNAMIC;
        img->pool = D3DPOOL_DEFAULT;
    } else {
        img->pool = D3DPOOL_MANAGED;
    }

    if ((d->bind & RHI_BIND_DEPTH) && !(d->bind & RHI_BIND_SAMPLED)) {
        hr = IDirect3DDevice9_CreateDepthStencilSurface(g_dev, d->width, d->height, img->fmt,
                                                        D3DMULTISAMPLE_NONE, 0, FALSE,
                                                        &img->surf, NULL);
    } else if (d->type == RHI_IMAGE_3D) {
        hr = IDirect3DDevice9_CreateVolumeTexture(g_dev, d->width, d->height, d->depth, levels,
                                                  usage, img->fmt, img->pool,
                                                  (IDirect3DVolumeTexture9 **)&img->tex, NULL);
    } else if (d->cube) {
        hr = IDirect3DDevice9_CreateCubeTexture(g_dev, d->width, levels, usage, img->fmt,
                                                img->pool, (IDirect3DCubeTexture9 **)&img->tex,
                                                NULL);
    } else {
        if (d->depth > 1) {
            static int said;
            if (!said++)
                fprintf(stderr, "[RHI] d3d9: a 2D array image has no D3D9 equal; only its "
                        "first layer is kept\n");
        }
        hr = IDirect3DDevice9_CreateTexture(g_dev, d->width, d->height, levels, usage, img->fmt,
                                            img->pool, (IDirect3DTexture9 **)&img->tex, NULL);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[RHI] d3d9: could not make a %ux%u image of D3D9 format %u "
                "(0x%08lX)\n", d->width, d->height, (unsigned)img->fmt, (unsigned long)hr);
        free(img);
        return NULL;
    }

    if (initial) {
        uint32_t layers = d->type == RHI_IMAGE_3D ? 1 : (d->cube ? 6 : 1);
        uint32_t i;
        for (i = 0; i < levels * layers; i++)
            if (initial[i].data)
                d9_image_update(img, i, NULL, initial[i].data, initial[i].row_pitch,
                                initial[i].slice_pitch);
    }
    return img;
}

static RhiImage *d9_image_retain(RhiImage *img)
{
    InterlockedIncrement(&img->refs);
    return img;
}

static void d9_image_destroy(RhiImage *img)
{
    int i;

    if (InterlockedDecrement(&img->refs) > 0)
        return;
    for (i = 0; i < 16; i++)
        if (g_b.tex[i] && g_b.tex[i]->image == img) {
            g_b.tex[i] = NULL;
            IDirect3DDevice9_SetTexture(g_dev, i, NULL);
        }
    if (img->tex) IDirect3DBaseTexture9_Release(img->tex);
    if (img->surf) IDirect3DSurface9_Release(img->surf);
    free(img);
}

static void d9_image_get_desc(const RhiImage *img, RhiImageDesc *out)
{
    *out = img->desc;
}

/* The surface behind a 2D or cube subresource. AddRef'd. */
static IDirect3DSurface9 *subresource_surface(RhiImage *img, uint32_t sub)
{
    uint32_t levels = img->desc.mip_levels ? img->desc.mip_levels : 1;
    uint32_t mip = sub % levels, layer = sub / levels;
    IDirect3DSurface9 *s = NULL;

    if (img->surf) {
        IDirect3DSurface9_AddRef(img->surf);
        return img->surf;
    }
    if (!img->tex)
        return NULL;
    if (img->desc.cube) {
        if (FAILED(IDirect3DCubeTexture9_GetCubeMapSurface((IDirect3DCubeTexture9 *)img->tex,
                                                           (D3DCUBEMAP_FACES)layer, mip, &s)))
            return NULL;
    } else if (img->desc.type != RHI_IMAGE_3D) {
        if (FAILED(IDirect3DTexture9_GetSurfaceLevel((IDirect3DTexture9 *)img->tex, mip, &s)))
            return NULL;
    }
    return s;
}

/* Copy rows into a locked rectangle or box, converting. */
static void write_rows(const RhiImage *img, uint8_t *dst, uint32_t dst_pitch,
                       const uint8_t *src, uint32_t src_pitch, uint32_t w, uint32_t rows)
{
    uint32_t y, texels = img->block ? (w + 3) / 4 : w;

    for (y = 0; y < rows; y++)
        convert_row_img(img, dst + (size_t)y * dst_pitch, src + (size_t)y * src_pitch, texels);
}

/* An image's identity for kind_2d, which must survive the image being made
 * again: its description, and, once its first level is filled whole, a
 * sample of what it was filled with (256 words spread across it, not all of
 * it: this runs at every upload). A pointer would do for a title, which keeps
 * its images; a frame replayed in a loop (d3d8_replay) makes every image anew
 * each time round, and its 2D was never learnt. */
static uint64_t image_id(const RhiImage *img, const void *data, size_t bytes)
{
    const RhiImageDesc *d = &img->desc;
    uint64_t v[7], h = 1469598103934665603ull;
    size_t i;

    v[0] = d->type; v[1] = d->width; v[2] = d->height; v[3] = d->depth;
    v[4] = d->mip_levels; v[5] = d->format; v[6] = d->bind;
    for (i = 0; i < 7; i++)
        h = (h ^ v[i]) * 1099511628211ull;
    if (data && bytes >= 8) {
        size_t n = bytes / 8 < 256 ? bytes / 8 : 256, step = (bytes - 8) / (n > 1 ? n - 1 : 1);

        for (i = 0; i < n; i++) {
            uint64_t w;

            memcpy(&w, (const uint8_t *)data + i * step, 8);
            h = (h ^ w) * 1099511628211ull;
        }
    }
    return h;
}

/* A level of a 2D or cube texture. AddRef'd. */
static IDirect3DSurface9 *level_surface(IDirect3DBaseTexture9 *t, int cube, uint32_t face,
                                        uint32_t mip)
{
    IDirect3DSurface9 *s = NULL;

    if (cube)
        IDirect3DCubeTexture9_GetCubeMapSurface((IDirect3DCubeTexture9 *)t,
                                                (D3DCUBEMAP_FACES)face, mip, &s);
    else
        IDirect3DTexture9_GetSurfaceLevel((IDirect3DTexture9 *)t, mip, &s);
    return s;
}

/* When a mod asks (rhi_d3d9_mod_remake_written_textures), a texture whose
 * texels change after a draw has used it is made anew first, for a D3D9
 * runtime that knows a texture by the hash of its first level, taken once,
 * when a draw first uses it: RTX Remix does so (D3D9CommonTexture::
 * SetupForRtxFrom), and kept Max Payne's graphic novel pages, filled after
 * their first use, blank. A new D3D9 texture is hashed afresh. What the
 * old one held is copied over, so a write of a part lands on the rest as
 * before. Managed 2D and cube textures only: what a title fills from the CPU.
 * Only for writes to level 0, and not for one that writes it whole with what
 * it already held: a title that uploads the same texels every frame would
 * otherwise get a new texture every frame. At most once between draws that
 * use it, however many writes come between. Each one is counted on stderr. */
static void remake_texture(RhiImage *img)
{
    const RhiImageDesc *d = &img->desc;
    uint32_t levels = d->mip_levels ? d->mip_levels : 1, faces = d->cube ? 6 : 1, f, m, i;
    IDirect3DBaseTexture9 *old = img->tex, *nt = NULL;
    HRESULT hr;

    img->drawn = 0;
    if (!g_remake_written || img->pool != D3DPOOL_MANAGED || !old || d->type == RHI_IMAGE_3D)
        return;
    if (d->cube)
        hr = IDirect3DDevice9_CreateCubeTexture(g_dev, d->width, levels, 0, img->fmt,
                                                D3DPOOL_MANAGED,
                                                (IDirect3DCubeTexture9 **)&nt, NULL);
    else
        hr = IDirect3DDevice9_CreateTexture(g_dev, d->width, d->height, levels, 0, img->fmt,
                                            D3DPOOL_MANAGED, (IDirect3DTexture9 **)&nt, NULL);
    if (FAILED(hr))
        return;
    for (f = 0; f < faces; f++)
        for (m = 0; m < levels; m++) {
            IDirect3DSurface9 *from = level_surface(old, d->cube, f, m);
            IDirect3DSurface9 *to = level_surface(nt, d->cube, f, m);
            D3DLOCKED_RECT a, b;
            uint32_t h = d->height >> m, rows, y;

            if (!h) h = 1;
            rows = img->block ? (h + 3) / 4 : h;
            if (from && to &&
                SUCCEEDED(IDirect3DSurface9_LockRect(from, &a, NULL, D3DLOCK_READONLY))) {
                if (SUCCEEDED(IDirect3DSurface9_LockRect(to, &b, NULL, 0))) {
                    size_t n = (size_t)(a.Pitch < b.Pitch ? a.Pitch : b.Pitch);

                    for (y = 0; y < rows; y++)
                        memcpy((uint8_t *)b.pBits + (size_t)y * (uint32_t)b.Pitch,
                               (const uint8_t *)a.pBits + (size_t)y * (uint32_t)a.Pitch, n);
                    IDirect3DSurface9_UnlockRect(to);
                }
                IDirect3DSurface9_UnlockRect(from);
            }
            if (from) IDirect3DSurface9_Release(from);
            if (to) IDirect3DSurface9_Release(to);
        }
    img->tex = nt;
    for (i = 0; i < 16; i++)
        if (g_b.tex[i] && g_b.tex[i]->image == img)
            IDirect3DDevice9_SetTexture(g_dev, i, nt);
    IDirect3DBaseTexture9_Release(old);
    {
        static unsigned made;

        if (++made <= 16 || !(made & 255))
            fprintf(stderr, "[RHI] d3d9: a %ux%u texture (D3D9 format %u) written "
                    "after use is made anew (%u so far)\n", d->width, d->height,
                    (unsigned)img->fmt, made);
    }
}

/* All of a level's bytes, for remake_texture. */
static uint64_t texels_hash(const void *data, size_t bytes)
{
    const uint8_t *p = data;
    uint64_t h = 1469598103934665603ull, w;
    size_t i;

    for (i = 0; i + 8 <= bytes; i += 8) {
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 1099511628211ull;
    }
    for (; i < bytes; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static void d9_image_update(RhiImage *img, uint32_t sub, const RhiBox *box,
                            const void *data, uint32_t row_pitch, uint32_t slice_pitch)
{
    uint32_t levels = img->desc.mip_levels ? img->desc.mip_levels : 1;
    uint32_t mip = sub % levels;
    uint32_t w = img->desc.width >> mip, h = img->desc.height >> mip;
    uint32_t x0 = 0, y0 = 0, rows;

    if (!w) w = 1;
    if (!h) h = 1;
    if (box) {
        x0 = box->left; y0 = box->top;
        w = box->right - box->left;
        h = box->bottom - box->top;
    }
    rows = img->block ? (h + 3) / 4 : h;
    if (sub == 0 && !box && data)
        img->id = image_id(img, data, (size_t)row_pitch * rows);
    if (g_remake_written && sub == 0 && data) {
        uint64_t t = box ? 0 : texels_hash(data, (size_t)row_pitch * rows);

        if (img->drawn && (box || t != img->texels))
            remake_texture(img);
        img->texels = t;                /* 0 after a part: not known */
    }

    if (img->desc.type == RHI_IMAGE_3D) {
        IDirect3DVolumeTexture9 *vt = (IDirect3DVolumeTexture9 *)img->tex;
        D3DLOCKED_BOX lb;
        D3DBOX b, *pb = NULL;
        uint32_t d = img->desc.depth >> mip, z;

        if (!d) d = 1;
        if (box) {
            b.Left = box->left; b.Top = box->top; b.Front = box->front;
            b.Right = box->right; b.Bottom = box->bottom; b.Back = box->back;
            d = box->back - box->front;
            pb = &b;
        }
        if (FAILED(IDirect3DVolumeTexture9_LockBox(vt, mip, &lb, pb, 0)))
            return;
        for (z = 0; z < d; z++)
            write_rows(img, (uint8_t *)lb.pBits + (size_t)z * lb.SlicePitch, lb.RowPitch,
                       (const uint8_t *)data + (size_t)z * slice_pitch, row_pitch, w, rows);
        IDirect3DVolumeTexture9_UnlockBox(vt, mip);
        return;
    }

    if (img->pool == D3DPOOL_MANAGED || img->desc.usage == RHI_USAGE_DYNAMIC) {
        IDirect3DSurface9 *s = subresource_surface(img, sub);
        D3DLOCKED_RECT lr;
        RECT r;
        DWORD flags = 0;

        if (!s)
            return;
        r.left = (LONG)x0; r.top = (LONG)y0; r.right = (LONG)(x0 + w); r.bottom = (LONG)(y0 + h);
        /* A dynamic texture written whole is discarded; a part of one is
         * locked as a rectangle, keeping the rest. */
        if (img->pool == D3DPOOL_DEFAULT && !box)
            flags = D3DLOCK_DISCARD;
        if (SUCCEEDED(IDirect3DSurface9_LockRect(s, &lr, box ? &r : NULL, flags))) {
            write_rows(img, lr.pBits, (uint32_t)lr.Pitch, data, row_pitch, w, rows);
            IDirect3DSurface9_UnlockRect(s);
        }
        IDirect3DSurface9_Release(s);
        return;
    }

    /* A render target: through a system-memory surface. */
    {
        IDirect3DSurface9 *dst = subresource_surface(img, sub), *tmp = NULL;
        D3DLOCKED_RECT lr;
        RECT r;
        POINT p;

        if (!dst)
            return;
        if (img->fmt == D3DFMT_INTZ || (img->desc.bind & RHI_BIND_DEPTH)) {
            IDirect3DSurface9_Release(dst);
            return;     /* depth is never written from the CPU */
        }
        if (SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, w, h, img->fmt,
                                                                   D3DPOOL_SYSTEMMEM, &tmp, NULL)) &&
            SUCCEEDED(IDirect3DSurface9_LockRect(tmp, &lr, NULL, 0))) {
            write_rows(img, lr.pBits, (uint32_t)lr.Pitch, data, row_pitch, w, rows);
            IDirect3DSurface9_UnlockRect(tmp);
            r.left = 0; r.top = 0; r.right = (LONG)w; r.bottom = (LONG)h;
            p.x = (LONG)x0; p.y = (LONG)y0;
            IDirect3DDevice9_UpdateSurface(g_dev, tmp, &r, dst, &p);
        }
        if (tmp) IDirect3DSurface9_Release(tmp);
        IDirect3DSurface9_Release(dst);
    }
}

static int d9_image_copy(RhiImage *dst, RhiImage *src)
{
    IDirect3DSurface9 *a, *b;
    HRESULT hr;

    if (!dst || !src || dst == src || dst->desc.type != RHI_IMAGE_2D ||
        src->desc.type != RHI_IMAGE_2D || dst->desc.width != src->desc.width ||
        dst->desc.height != src->desc.height || dst->fmt != src->fmt ||
        !(dst->desc.bind & RHI_BIND_RENDER_TARGET))
        return -1;
    scene_end();
    a = subresource_surface(dst, 0);
    b = subresource_surface(src, 0);
    hr = (a && b) ? IDirect3DDevice9_StretchRect(g_dev, b, NULL, a, NULL, D3DTEXF_NONE) : E_FAIL;
    if (a) IDirect3DSurface9_Release(a);
    if (b) IDirect3DSurface9_Release(b);
    return SUCCEEDED(hr) ? 0 : -1;
}

/* Rows of a locked surface into dst, converted back to the RHI's layout. */
static void read_rows(const RhiImage *img, uint8_t *dst, uint32_t dst_pitch,
                      const uint8_t *src, uint32_t src_pitch, uint32_t w, uint32_t rows)
{
    uint32_t y, x;

    for (y = 0; y < rows; y++) {
        const uint8_t *s = src + (size_t)y * src_pitch;
        uint8_t *d = dst + (size_t)y * dst_pitch;

        if (!img || img->conv == CONV_SWAP_RB) {
            /* The back buffer (img NULL) is X8R8G8B8 and is read as RGBA8. */
            for (x = 0; x < w; x++) {
                d[4 * x + 0] = s[4 * x + 2];
                d[4 * x + 1] = s[4 * x + 1];
                d[4 * x + 2] = s[4 * x + 0];
                d[4 * x + 3] = img ? s[4 * x + 3] : 0xFF;
            }
        } else if (img->conv == CONV_NONE) {
            uint32_t n = (img->block ? (w + 3) / 4 : w) * img->bpp_out;
            memcpy(d, s, n < dst_pitch ? n : dst_pitch);
        } else {
            /* Widened formats are read back as stored; nothing reads them. */
            uint32_t n = w * img->bpp_out;
            memcpy(d, s, n < dst_pitch ? n : dst_pitch);
        }
    }
}

static int read_surface(const RhiImage *img, IDirect3DSurface9 *s, void *dst, uint32_t dst_pitch)
{
    D3DSURFACE_DESC sd;
    IDirect3DSurface9 *tmp = NULL;
    D3DLOCKED_RECT lr;
    int rc = -1;

    if (FAILED(IDirect3DSurface9_GetDesc(s, &sd)))
        return -1;
    scene_end();
    if (sd.Pool == D3DPOOL_DEFAULT) {
        if (!(sd.Usage & D3DUSAGE_RENDERTARGET) && sd.Type != D3DRTYPE_SURFACE)
            return -1;
        if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, sd.Width, sd.Height,
                                                                sd.Format, D3DPOOL_SYSTEMMEM,
                                                                &tmp, NULL)))
            return -1;
        if (FAILED(IDirect3DDevice9_GetRenderTargetData(g_dev, s, tmp))) {
            IDirect3DSurface9_Release(tmp);
            return -1;
        }
        s = tmp;
    }
    if (SUCCEEDED(IDirect3DSurface9_LockRect(s, &lr, NULL, D3DLOCK_READONLY))) {
        uint32_t rows = (img && img->block) ? (sd.Height + 3) / 4 : sd.Height;
        read_rows(img, dst, dst_pitch, lr.pBits, (uint32_t)lr.Pitch, sd.Width, rows);
        IDirect3DSurface9_UnlockRect(s);
        rc = 0;
    }
    if (tmp) IDirect3DSurface9_Release(tmp);
    return rc;
}

static int d9_image_readback(RhiImage *img, uint32_t sub, void *dst, uint32_t dst_pitch)
{
    IDirect3DSurface9 *s;
    int rc;

    if (!img || img->desc.type != RHI_IMAGE_2D || (img->desc.bind & RHI_BIND_DEPTH))
        return -1;
    s = subresource_surface(img, sub);
    if (!s)
        return -1;
    rc = read_surface(img, s, dst, dst_pitch);
    IDirect3DSurface9_Release(s);
    return rc;
}

static int d9_swapchain_readback(void *dst, uint32_t pitch, uint32_t *w, uint32_t *h)
{
    if (!g_bb_view)
        return -1;
    if (w) *w = g_pp.BackBufferWidth;
    if (h) *h = g_pp.BackBufferHeight;
    return dst ? read_surface(NULL, g_bb_view->surf, dst, pitch) : 0;
}

static int d9_sample_count_supported(RhiFormat f, uint32_t samples)
{
    (void)f;
    return samples <= 1;
}

/* ---- views ------------------------------------------------------------------------- */

static RhiView *d9_view_create(RhiImage *img, uint32_t kind, const RhiViewDesc *d)
{
    RhiView *v = calloc(1, sizeof *v);

    if (!v)
        return NULL;
    v->image = img;
    v->kind = kind;
    v->dim = d ? d->dim : (img->desc.cube ? RHI_VIEW_DIM_CUBE :
                           img->desc.type == RHI_IMAGE_3D ? RHI_VIEW_DIM_3D : RHI_VIEW_DIM_2D);
    v->base_mip = d ? d->base_mip : 0;
    if (kind != RHI_VIEW_SAMPLED) {
        uint32_t levels = img->desc.mip_levels ? img->desc.mip_levels : 1;
        uint32_t layer = d ? d->base_layer : 0;
        v->surf = subresource_surface(img, v->base_mip + layer * levels);
        if (!v->surf) {
            free(v);
            return NULL;
        }
    } else if (!img->tex) {
        free(v);        /* a depth surface that was not made to be sampled */
        return NULL;
    }
    /* A view keeps its image, as a D3D11 view keeps its resource: the
     * renderer may let go of an image while a view of it is still bound. */
    d9_image_retain(img);
    return v;
}

static void d9_view_destroy(RhiView *v)
{
    int i;

    for (i = 0; i < 16; i++)
        if (g_b.tex[i] == v) {
            g_b.tex[i] = NULL;
            IDirect3DDevice9_SetTexture(g_dev, i, NULL);
        }
    if (g_b.rt == v) g_b.rt = NULL;
    if (g_b.ds == v) g_b.ds = NULL;
    if (v->surf) IDirect3DSurface9_Release(v->surf);
    if (v->image) d9_image_destroy(v->image);
    free(v);
}

static RhiImage *d9_view_image(const RhiView *v)
{
    return v->image;
}

/* ---- shaders and layouts ---------------------------------------------------------- */

#define D3D9_CACHE_TAG "d3d9-fxc-2"

static RhiShader *d9_shader_create(uint32_t stage, const RhiShaderSource *src, char *err,
                                   size_t err_len)
{
    D3D_SHADER_MACRO macros[16];
    int vertex = stage == RHI_STAGE_VERTEX;
    const char *prelude = vertex ? kVsPrelude : kPsPrelude;
    size_t plen = strlen(prelude), mlen = vertex ? strlen(g_mod_vs_hlsl) : 0;
    size_t elen = vertex ? strlen(kVsPreludeEnd) : 0;
    RhiShaderSource full = *src;
    ID3DBlob *code = NULL, *errors = NULL;
    const DWORD *bytes;
    char *text;
    RhiShader *s;
    HRESULT hr;
    int n = 0;

    if (err && err_len)
        err[0] = 0;
    text = malloc(plen + mlen + elen + src->len + 1);
    if (!text)
        return NULL;
    memcpy(text, prelude, plen);
    memcpy(text + plen, g_mod_vs_hlsl, mlen);
    memcpy(text + plen + mlen, kVsPreludeEnd, elen);
    plen += mlen + elen;
    memcpy(text + plen, src->hlsl, src->len);
    text[plen + src->len] = 0;
    full.hlsl = text;
    full.len = plen + src->len;
    /* Shader Model 3, whatever the source was written for, except a pixel
     * shader asked for as Shader Model 2: the one D3D9 runs under its
     * fixed-function vertex pipeline (the register combiners' NV2A_HOST_SM2). */
    full.target = stage == RHI_STAGE_VERTEX ? "vs_3_0"
                : (src->target && strncmp(src->target, "ps_2_", 5) == 0) ? src->target
                : "ps_3_0";

    if (src->macros) {
        for (; src->macros[n].name && n < 15; n++) {
            macros[n].Name = src->macros[n].name;
            macros[n].Definition = src->macros[n].value;
        }
    }
    macros[n].Name = NULL;
    macros[n].Definition = NULL;
    d3d8_hlsl_note(full.hlsl, full.len, full.name, full.macros, full.entry, full.target);

    {
        void *cached = NULL;
        size_t cached_bytes = 0;

        if (rhi_shader_cache_get(&full, D3D9_CACHE_TAG, &cached, &cached_bytes) &&
            SUCCEEDED(D3DCreateBlob(cached_bytes, &code))) {
            memcpy(ID3D10Blob_GetBufferPointer(code), cached, cached_bytes);
            free(cached);
            goto compiled;
        }
        free(cached);
        code = NULL;
    }
    hr = D3DCompile(full.hlsl, (SIZE_T)full.len, full.name, macros, NULL, full.entry,
                    full.target, full.optimize ? D3DCOMPILE_OPTIMIZATION_LEVEL3 : 0, 0,
                    &code, &errors);
    if (errors) {
        if (err && err_len)
            snprintf(err, err_len, "%s", (const char *)ID3D10Blob_GetBufferPointer(errors));
        ID3D10Blob_Release(errors);
    }
    if (FAILED(hr)) {
        if (err && err_len && !err[0])
            snprintf(err, err_len, "D3DCompile failed (0x%08lX)", (unsigned long)hr);
        free(text);
        return NULL;
    }
    rhi_shader_cache_put(&full, D3D9_CACHE_TAG, ID3D10Blob_GetBufferPointer(code),
                         ID3D10Blob_GetBufferSize(code));
compiled:
    free(text);
    s = calloc(1, sizeof *s);
    if (!s) {
        ID3D10Blob_Release(code);
        return NULL;
    }
    s->stage = stage;
    bytes = (const DWORD *)ID3D10Blob_GetBufferPointer(code);
    {
        const uint8_t *b = (const uint8_t *)bytes;
        size_t i, nb = ID3D10Blob_GetBufferSize(code);

        s->id = 1469598103934665603ull;
        for (i = 0; i < nb; i++)
            s->id = (s->id ^ b[i]) * 1099511628211ull;
    }
    if (stage == RHI_STAGE_VERTEX)
        hr = IDirect3DDevice9_CreateVertexShader(g_dev, bytes, &s->vs);
    else
        hr = IDirect3DDevice9_CreatePixelShader(g_dev, bytes, &s->ps);
    ID3D10Blob_Release(code);
    if (FAILED(hr)) {
        if (err && err_len)
            snprintf(err, err_len, "Create%sShader failed (0x%08lX)",
                     stage == RHI_STAGE_VERTEX ? "Vertex" : "Pixel", (unsigned long)hr);
        free(s);
        return NULL;
    }
    return s;
}

static void d9_shader_destroy(RhiShader *s)
{
    if (g_b.vs == s) { g_b.vs = NULL; IDirect3DDevice9_SetVertexShader(g_dev, NULL); }
    if (g_b.ps == s) { g_b.ps = NULL; IDirect3DDevice9_SetPixelShader(g_dev, NULL); }
    if (s->vs) IDirect3DVertexShader9_Release(s->vs);
    if (s->ps) IDirect3DPixelShader9_Release(s->ps);
    free(s);
}

/* The NV2A register convention (rhi_d3d9.h). */
const char *rhi_d3d9_attr_semantic(int reg, int *index)
{
    static const struct { const char *name; int index; } map[16] = {
        { "POSITION", 0 }, { "BLENDWEIGHT", 0 }, { "NORMAL", 0 }, { "COLOR", 0 },
        { "COLOR", 1 }, { "FOG", 0 }, { "PSIZE", 0 }, { "COLOR", 2 },
        { "COLOR", 3 }, { "TEXCOORD", 0 }, { "TEXCOORD", 1 }, { "TEXCOORD", 2 },
        { "TEXCOORD", 3 }, { "TEXCOORD", 4 }, { "TEXCOORD", 5 }, { "TEXCOORD", 6 },
    };
    if (reg < 0 || reg > 15)
        reg = 15;
    if (index)
        *index = map[reg].index;
    return map[reg].name;
}

static int usage_of(const char *name, uint32_t index, BYTE *usage, BYTE *usage_index)
{
    static const struct { const char *name; BYTE usage; } names[] = {
        { "POSITIONT", D3DDECLUSAGE_POSITIONT }, { "POSITION", D3DDECLUSAGE_POSITION },
        { "BLENDWEIGHT", D3DDECLUSAGE_BLENDWEIGHT }, { "BLENDINDICES", D3DDECLUSAGE_BLENDINDICES },
        { "NORMAL", D3DDECLUSAGE_NORMAL }, { "PSIZE", D3DDECLUSAGE_PSIZE },
        { "TEXCOORD", D3DDECLUSAGE_TEXCOORD }, { "TANGENT", D3DDECLUSAGE_TANGENT },
        { "BINORMAL", D3DDECLUSAGE_BINORMAL }, { "COLOR", D3DDECLUSAGE_COLOR },
        { "FOG", D3DDECLUSAGE_FOG }, { "DEPTH", D3DDECLUSAGE_DEPTH },
    };
    size_t i;

    if (strcmp(name, "ATTR") == 0) {
        int idx = 0;
        name = rhi_d3d9_attr_semantic((int)index, &idx);
        index = (uint32_t)idx;
    }
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(name, names[i].name) == 0) {
            *usage = names[i].usage;
            *usage_index = (BYTE)index;
            return 0;
        }
    return -1;
}

/* The declaration type for a DXGI vertex format, and its size. -1: none. */
static int decl_type(RhiFormat f, BYTE *type, uint32_t *size)
{
    static int said_short1, said_byte2, said_byte1;

    switch (f) {
    case RHI_FORMAT_R32_FLOAT:          *type = D3DDECLTYPE_FLOAT1;    *size = 4;  return 0;
    case RHI_FORMAT_R32G32_FLOAT:       *type = D3DDECLTYPE_FLOAT2;    *size = 8;  return 0;
    case RHI_FORMAT_R32G32B32_FLOAT:    *type = D3DDECLTYPE_FLOAT3;    *size = 12; return 0;
    case RHI_FORMAT_R32G32B32A32_FLOAT: *type = D3DDECLTYPE_FLOAT4;    *size = 16; return 0;
    case RHI_FORMAT_B8G8R8A8_UNORM:     *type = D3DDECLTYPE_D3DCOLOR;  *size = 4;  return 0;
    case RHI_FORMAT_R8G8B8A8_UNORM:     *type = D3DDECLTYPE_UBYTE4N;   *size = 4;  return 0;
    case RHI_FORMAT_R16G16_SNORM:       *type = D3DDECLTYPE_SHORT2N;   *size = 4;  return 0;
    case RHI_FORMAT_R16G16B16A16_SNORM: *type = D3DDECLTYPE_SHORT4N;   *size = 8;  return 0;
    case RHI_FORMAT_R16G16_UNORM:       *type = D3DDECLTYPE_USHORT2N;  *size = 4;  return 0;
    case RHI_FORMAT_R16G16B16A16_UNORM: *type = D3DDECLTYPE_USHORT4N;  *size = 8;  return 0;
    case RHI_FORMAT_R16G16_FLOAT:       *type = D3DDECLTYPE_FLOAT16_2; *size = 4;  return 0;
    case RHI_FORMAT_R16G16B16A16_FLOAT: *type = D3DDECLTYPE_FLOAT16_4; *size = 8;  return 0;
    case RHI_FORMAT_R16_SNORM:
        /* No one-short type: read as two, the second being whatever follows. */
        if (!said_short1++)
            fprintf(stderr, "[RHI] d3d9: a one-component short attribute is read as two\n");
        *type = D3DDECLTYPE_SHORT2N; *size = 2; return 0;
    case RHI_FORMAT_R8G8_UNORM:
        if (!said_byte2++)
            fprintf(stderr, "[RHI] d3d9: a two-byte attribute is read as four\n");
        *type = D3DDECLTYPE_UBYTE4N; *size = 2; return 0;
    case RHI_FORMAT_R8_UNORM:
        /* An Xbox PBYTE1 (one byte, 0..1): read as four, the rest being
         * whatever follows. Not a register the vertex does not carry, which
         * d3d8_vsh.c leaves out under D3D9; once that marker was this
         * format, and dropping it dropped every real PBYTE1 too. */
        if (!said_byte1++)
            fprintf(stderr, "[RHI] d3d9: a one-byte attribute is read as four\n");
        *type = D3DDECLTYPE_UBYTE4N; *size = 1; return 0;
    default:
        return -1;
    }
}

static RhiVertexLayout *d9_vertex_layout_create(const RhiVertexElement *e, uint32_t n,
                                                const RhiShader *vs)
{
    D3DVERTEXELEMENT9 el[33];
    uint32_t i, out = 0, offset[16] = { 0 };
    RhiVertexLayout *l;

    (void)vs;
    if (n > 32)
        return NULL;
    for (i = 0; i < n; i++) {
        BYTE type, usage, usage_index;
        uint32_t size, slot = e[i].slot < 16 ? e[i].slot : 15;
        uint32_t off = e[i].offset == RHI_APPEND ? offset[slot] : e[i].offset;

        if (decl_type(e[i].format, &type, &size) != 0) {
            fprintf(stderr, "[RHI] d3d9: vertex format %u has no declaration type; "
                    "%s%u is left out\n", (unsigned)e[i].format, e[i].semantic,
                    e[i].semantic_index);
            offset[slot] = off + 1;
            continue;
        }
        if (usage_of(e[i].semantic, e[i].semantic_index, &usage, &usage_index) != 0) {
            fprintf(stderr, "[RHI] d3d9: no declaration usage for semantic %s\n",
                    e[i].semantic);
            continue;
        }
        el[out].Stream = (WORD)slot;
        el[out].Offset = (WORD)off;
        el[out].Type = type;
        el[out].Method = D3DDECLMETHOD_DEFAULT;
        el[out].Usage = usage;
        el[out].UsageIndex = usage_index;
        out++;
        offset[slot] = off + size;
    }
    {
        D3DVERTEXELEMENT9 end = D3DDECL_END();
        el[out] = end;
    }
    l = calloc(1, sizeof *l);
    if (!l)
        return NULL;
    if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(g_dev, el, &l->decl))) {
        fprintf(stderr, "[RHI] d3d9: CreateVertexDeclaration failed (%u elements)\n", out);
        free(l);
        return NULL;
    }
    return l;
}

static void d9_vertex_layout_destroy(RhiVertexLayout *l)
{
    if (g_b.layout == l)
        g_b.layout = NULL;
    if (l->decl) IDirect3DVertexDeclaration9_Release(l->decl);
    free(l);
}

/* ---- state objects ----------------------------------------------------------------- */

static RhiBlendState *d9_blend_state_create(const RhiBlendDesc *d)
{
    RhiBlendState *s = calloc(1, sizeof *s);
    if (s) s->d = *d;
    return s;
}

static RhiDepthState *d9_depth_state_create(const RhiDepthDesc *d)
{
    RhiDepthState *s = calloc(1, sizeof *s);
    if (s) s->d = *d;
    return s;
}

static RhiRasterState *d9_raster_state_create(const RhiRasterDesc *d)
{
    RhiRasterState *s = calloc(1, sizeof *s);
    if (s) s->d = *d;
    return s;
}

static RhiSampler *d9_sampler_create(const RhiSamplerDesc *d)
{
    RhiSampler *s = calloc(1, sizeof *s);
    if (s) s->d = *d;
    return s;
}

static void d9_blend_state_destroy(RhiBlendState *s) { free(s); }
static void d9_depth_state_destroy(RhiDepthState *s)
{
    if (g_b.depth == s) g_b.depth = NULL;
    free(s);
}
static void d9_raster_state_destroy(RhiRasterState *s)
{
    if (g_b.raster == s) g_b.raster = NULL;
    free(s);
}
static void d9_sampler_destroy(RhiSampler *s)
{
    int i;
    for (i = 0; i < 16; i++)
        if (g_b.samp[i] == s) g_b.samp[i] = NULL;
    free(s);
}

/* ---- binding ------------------------------------------------------------------------ */

static void target_size(uint32_t *w, uint32_t *h)
{
    D3DSURFACE_DESC sd;
    IDirect3DSurface9 *s = g_b.rt ? g_b.rt->surf : g_b.ds ? g_b.ds->surf : NULL;

    *w = g_pp.BackBufferWidth;
    *h = g_pp.BackBufferHeight;
    if (s && SUCCEEDED(IDirect3DSurface9_GetDesc(s, &sd))) {
        *w = sd.Width;
        *h = sd.Height;
    }
}

static void d9_set_render_target(RhiView *color, RhiView *depth)
{
    g_b.rt = color;
    g_b.ds = depth;
    /* D3D9 cannot run with no colour target; a depth-only pass keeps the
     * one it had, with colour writes off from its blend state. */
    if (color && color->surf)
        IDirect3DDevice9_SetRenderTarget(g_dev, 0, color->surf);
    IDirect3DDevice9_SetDepthStencilSurface(g_dev, depth ? depth->surf : NULL);
    /* SetRenderTarget resets the viewport and the scissor to the target. */
    g_b.vp_dirty = g_b.scissor_dirty = 1;
}

static void d9_output_save(RhiOutputState *s)
{
    memset(s, 0, sizeof *s);
    s->color = g_b.rt;
    s->depth = g_b.ds;
    s->viewport_count = 1;
    s->viewports[0] = g_b.vp;
}

static void d9_set_viewports(uint32_t n, const RhiViewport *vps)
{
    if (!n)
        return;
    g_b.vp = vps[0];
    g_b.vp_dirty = 1;
}

static void d9_output_restore(RhiOutputState *s)
{
    d9_set_render_target((RhiView *)s->color, (RhiView *)s->depth);
    if (s->viewport_count)
        d9_set_viewports(1, s->viewports);
    s->color = s->depth = NULL;
}

static int d9_output_color_is(const RhiOutputState *s, const RhiView *v)
{
    return v && s->color == (const void *)v;
}

static void d9_set_scissor(const RhiRect *r)
{
    g_b.scissor = *r;
    g_b.scissor_dirty = 1;
}

static void d9_set_topology(uint32_t t)        { g_b.topology = t; }
static void d9_set_vertex_layout(RhiVertexLayout *l) { g_b.layout = l; }

static void d9_set_vertex_buffer(uint32_t slot, RhiBuffer *b, uint32_t stride, uint32_t offset)
{
    if (slot >= 16)
        return;
    g_b.vb[slot] = b;
    g_b.vb_stride[slot] = stride;
    g_b.vb_offset[slot] = offset;
    IDirect3DDevice9_SetStreamSource(g_dev, slot, b ? b->vb : NULL, b ? offset : 0,
                                     b ? stride : 0);
    if (slot == 0)
        g_b.vid_bound = 0;
}

static void d9_set_index_buffer(RhiBuffer *b, uint32_t bits, uint32_t offset)
{
    g_b.ib = b;
    g_b.ib_bits = bits == 32 ? 32 : 16;
    g_b.ib_offset = offset;
}

static void d9_set_shader(uint32_t stage, RhiShader *s)
{
    if (stage == RHI_STAGE_VERTEX) {
        g_b.vs = s;
        IDirect3DDevice9_SetVertexShader(g_dev, s ? s->vs : NULL);
        g_ff_proj_dirty = 1;
    } else {
        g_b.ps = s;
        IDirect3DDevice9_SetPixelShader(g_dev, s ? s->ps : NULL);
    }
}

static void d9_set_uniform_buffers(uint32_t stage, uint32_t slot, uint32_t n, RhiBuffer *const *b)
{
    uint32_t i, st = stage == RHI_STAGE_VERTEX ? 0 : 1;

    for (i = 0; i < n && slot + i < VS_SLOTS; i++)
        g_b.ub[st][slot + i] = b[i];
}

static void d9_set_textures(uint32_t slot, uint32_t n, RhiView *const *v)
{
    uint32_t i;

    for (i = 0; i < n && slot + i < 16; i++) {
        RhiView *view = v[i];
        g_b.tex[slot + i] = view;
        IDirect3DDevice9_SetTexture(g_dev, slot + i,
                                    view && view->image ? view->image->tex : NULL);
    }
    g_b.samp_dirty = 1;
}

static void d9_set_samplers(uint32_t slot, uint32_t n, RhiSampler *const *s)
{
    uint32_t i;

    for (i = 0; i < n && slot + i < 16; i++)
        g_b.samp[slot + i] = s[i];
    g_b.samp_dirty = 1;
}

static void d9_set_blend_state(RhiBlendState *s, const float *factor, uint32_t mask)
{
    static const RhiBlendDesc none = { 0, RHI_BLEND_ONE, RHI_BLEND_ZERO, RHI_BLEND_OP_ADD,
                                       RHI_BLEND_ONE, RHI_BLEND_ZERO, RHI_BLEND_OP_ADD,
                                       RHI_WRITE_ALL, 0 };
    const RhiBlendDesc *d = s ? &s->d : &none;

    /* The RHI's blend numbers are D3D11's, which are D3D9's (D3DBLEND,
     * D3DBLENDOP and D3DCOLORWRITEENABLE all kept their values). */
    set_rs(D3DRS_ALPHABLENDENABLE, d->enable ? TRUE : FALSE);
    if (d->enable) {
        set_rs(D3DRS_SRCBLEND, d->src);
        set_rs(D3DRS_DESTBLEND, d->dst);
        set_rs(D3DRS_BLENDOP, d->op);
        set_rs(D3DRS_SRCBLENDALPHA, d->src_alpha);
        set_rs(D3DRS_DESTBLENDALPHA, d->dst_alpha);
        set_rs(D3DRS_BLENDOPALPHA, d->op_alpha);
    }
    set_rs(D3DRS_COLORWRITEENABLE, d->write_mask & 0xFu);
    if (factor)
        set_rs(D3DRS_BLENDFACTOR, to_d3dcolor(factor));
    set_rs(D3DRS_MULTISAMPLEMASK, mask);
}

static void d9_set_depth_state(RhiDepthState *s, uint32_t ref)
{
    g_b.depth = s;
    g_b.stencil_ref = ref;
    g_b.ds_dirty = 1;
}

static void d9_set_raster_state(RhiRasterState *s)
{
    g_b.raster = s;
    g_b.ds_dirty = 1;
}

/* ---- the draw ------------------------------------------------------------------------ */

/* Viewport and the clip-space correction for it (RHI_VP_ADJUST). D3D9
 * wants a viewport inside its target and in whole pixels, and puts pixel
 * centres half a pixel from where D3D10 and later do; the rectangle asked
 * for is mapped onto the one set by scaling and offsetting clip x and y. */
static void apply_viewport(void)
{
    uint32_t tw, th;
    float x0, y0, x1, y1, w, h, aw, ah;
    D3DVIEWPORT9 vp;

    target_size(&tw, &th);
    w = g_b.vp.width;
    h = g_b.vp.height;
    if (!(w > 0.0f) || !(h > 0.0f)) {
        w = (float)tw;
        h = (float)th;
        g_b.vp.x = g_b.vp.y = 0.0f;
        g_b.vp.max_depth = 1.0f;
    }
    /* Rounded out, never in: a viewport at a fraction of a pixel (every
     * title's, from the console's pixel offset, d3d8_device.c) rounded to
     * the nearest started at pixel 1, and nothing was ever drawn in the
     * target's first column and row (two of each at three times the
     * resolution). The pixels D3D11 would draw are those whose centres lie
     * in the rectangle, left and top inclusive; where the rounded viewport
     * covers more, apply_scissor cuts it back to them. */
    x0 = floorf(g_b.vp.x);
    y0 = floorf(g_b.vp.y);
    x1 = ceilf(g_b.vp.x + w);
    y1 = ceilf(g_b.vp.y + h);
    if (x0 < 0.0f) x0 = 0.0f;
    if (y0 < 0.0f) y0 = 0.0f;
    if (x1 > (float)tw) x1 = (float)tw;
    if (y1 > (float)th) y1 = (float)th;
    if (x1 <= x0) x1 = x0 + 1.0f;
    if (y1 <= y0) y1 = y0 + 1.0f;
    {
        float px0 = ceilf(g_b.vp.x - 0.5f), py0 = ceilf(g_b.vp.y - 0.5f);
        float px1 = ceilf(g_b.vp.x + w - 0.5f), py1 = ceilf(g_b.vp.y + h - 0.5f);

        if (px0 < x0) px0 = x0;
        if (py0 < y0) py0 = y0;
        if (px1 > x1) px1 = x1;
        if (py1 > y1) py1 = y1;
        if (px1 < px0) px1 = px0;
        if (py1 < py0) py1 = py0;
        g_b.vp_pixels.left = (LONG)px0;
        g_b.vp_pixels.top = (LONG)py0;
        g_b.vp_pixels.right = (LONG)px1;
        g_b.vp_pixels.bottom = (LONG)py1;
        g_b.vp_clip = px0 > x0 || py0 > y0 || px1 < x1 || py1 < y1;
        g_b.scissor_dirty = 1;
    }
    aw = x1 - x0;
    ah = y1 - y0;

    vp.X = (DWORD)x0;
    vp.Y = (DWORD)y0;
    vp.Width = (DWORD)aw;
    vp.Height = (DWORD)ah;
    vp.MinZ = g_b.vp.min_depth;
    vp.MaxZ = g_b.vp.max_depth;
    IDirect3DDevice9_SetViewport(g_dev, &vp);

    /* ndc' = ndc * (w / aw) + offset, so that a point lands on the pixel the
     * requested viewport would have put it on; then half a pixel left and
     * up, D3D9's centres. */
    g_b.adjust[0] = w / aw;
    g_b.adjust[1] = h / ah;
    g_b.adjust[2] = (2.0f * (g_b.vp.x - x0) + w) / aw - 1.0f - 1.0f / aw;
    g_b.adjust[3] = 1.0f - 2.0f * (g_b.vp.y - y0) / ah - h / ah + 1.0f / ah;
    g_b.adjust_sent = 0;
    g_ff_proj_dirty = 1;
}

/* The title's scissor when its raster state asks for one, cut to the
 * viewport's pixels when D3D9's whole-pixel viewport covers more of them
 * (apply_viewport). */
static void apply_scissor(void)
{
    uint32_t tw, th;
    RECT r;

    set_rs(D3DRS_SCISSORTESTENABLE, g_b.raster_scissor || g_b.vp_clip);
    if (!g_b.raster_scissor && !g_b.vp_clip)
        return;
    target_size(&tw, &th);
    if (g_b.raster_scissor) {
        r.left = g_b.scissor.left < 0 ? 0 : g_b.scissor.left;
        r.top = g_b.scissor.top < 0 ? 0 : g_b.scissor.top;
        r.right = g_b.scissor.right > (int32_t)tw ? (LONG)tw : g_b.scissor.right;
        r.bottom = g_b.scissor.bottom > (int32_t)th ? (LONG)th : g_b.scissor.bottom;
    } else {
        r.left = r.top = 0;
        r.right = (LONG)tw;
        r.bottom = (LONG)th;
    }
    if (g_b.vp_clip) {
        if (r.left < g_b.vp_pixels.left) r.left = g_b.vp_pixels.left;
        if (r.top < g_b.vp_pixels.top) r.top = g_b.vp_pixels.top;
        if (r.right > g_b.vp_pixels.right) r.right = g_b.vp_pixels.right;
        if (r.bottom > g_b.vp_pixels.bottom) r.bottom = g_b.vp_pixels.bottom;
    }
    if (r.right < r.left) r.right = r.left;
    if (r.bottom < r.top) r.bottom = r.top;
    IDirect3DDevice9_SetScissorRect(g_dev, &r);
}

/* The fixed-function projection, with the viewport correction folded in:
 * clip x' = x * ax + w * cx, as the shaders do it. */
static void set_projection(const float *m)
{
    float p[16];
    int r;

    memcpy(p, m, sizeof p);
    for (r = 0; r < 4; r++) {
        p[r * 4 + 0] = m[r * 4 + 0] * g_b.adjust[0] + m[r * 4 + 3] * g_b.adjust[2];
        p[r * 4 + 1] = m[r * 4 + 1] * g_b.adjust[1] + m[r * 4 + 3] * g_b.adjust[3];
    }
    IDirect3DDevice9_SetTransform(g_dev, D3DTS_PROJECTION, (const D3DMATRIX *)p);
}

static void apply_projection(void)
{
    if (g_ff_proj_set)
        set_projection(g_ff_proj);
}

/* recomp_mod.h's d3d9_set_projection and d3d9_render_state. */
void rhi_d3d9_mod_set_projection(const float m[16])
{
    if (rhi_d3d9_active() && m)
        set_projection(m);
}

int rhi_d3d9_mod_render_state(uint32_t state, uint32_t *value)
{
    if (state >= RS_COUNT || !g_rs_known[state])
        return 0;
    if (value)
        *value = g_rs[state];
    return 1;
}

static void stencil_face(const RhiStencilFace *f, int ccw)
{
    set_rs(ccw ? D3DRS_CCW_STENCILFAIL : D3DRS_STENCILFAIL, f->fail);
    set_rs(ccw ? D3DRS_CCW_STENCILZFAIL : D3DRS_STENCILZFAIL, f->depth_fail);
    set_rs(ccw ? D3DRS_CCW_STENCILPASS : D3DRS_STENCILPASS, f->pass);
    set_rs(ccw ? D3DRS_CCW_STENCILFUNC : D3DRS_STENCILFUNC, f->func);
}

/* Depth, stencil and raster state, which D3D9 keeps together: its stencil
 * faces are by winding (clockwise, counter-clockwise) where D3D11's are
 * front and back, and the raster state says which winding is the front. */
static void apply_depth_raster(void)
{
    static const RhiRasterDesc raster_default = { RHI_FILL_SOLID, RHI_CULL_BACK, 0, 0, 0.0f,
                                                  0.0f, 1, 0, 0, 0 };
    const RhiRasterDesc *r = g_b.raster ? &g_b.raster->d : &raster_default;
    int front_ccw = r->front_ccw != 0;
    DWORD cull = D3DCULL_NONE;

    if (r->cull == RHI_CULL_BACK)
        cull = front_ccw ? D3DCULL_CW : D3DCULL_CCW;
    else if (r->cull == RHI_CULL_FRONT)
        cull = front_ccw ? D3DCULL_CCW : D3DCULL_CW;
    set_rs(D3DRS_CULLMODE, cull);
    set_rs(D3DRS_FILLMODE, r->fill == RHI_FILL_WIREFRAME ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
    /* D3D11's bias is in units of the depth format's resolution; D3D9's is
     * a depth. Every depth buffer here is 24-bit. */
    set_rs(D3DRS_DEPTHBIAS, f2dw((float)r->depth_bias / 16777216.0f));
    set_rs(D3DRS_SLOPESCALEDEPTHBIAS, f2dw(r->slope_scaled_depth_bias));
    if (g_b.raster_scissor != (r->scissor != 0)) {
        g_b.raster_scissor = r->scissor != 0;
        g_b.scissor_dirty = 1;
    }
    set_rs(D3DRS_MULTISAMPLEANTIALIAS, r->multisample ? TRUE : FALSE);
    set_rs(D3DRS_ANTIALIASEDLINEENABLE, r->antialiased_lines ? TRUE : FALSE);
    if (g_b.clamp[0] != (r->depth_clip ? 0.0f : 1.0f)) {
        g_b.clamp[0] = r->depth_clip ? 0.0f : 1.0f;
        g_b.clamp_sent = 0;
    }

    if (g_b.depth) {
        const RhiDepthDesc *d = &g_b.depth->d;
        set_rs(D3DRS_ZENABLE, d->depth_enable ? D3DZB_TRUE : D3DZB_FALSE);
        set_rs(D3DRS_ZWRITEENABLE, d->depth_enable && d->depth_write ? TRUE : FALSE);
        set_rs(D3DRS_ZFUNC, d->depth_func ? d->depth_func : D3DCMP_LESS);
        set_rs(D3DRS_STENCILENABLE, d->stencil_enable ? TRUE : FALSE);
        if (d->stencil_enable) {
            set_rs(D3DRS_STENCILMASK, d->stencil_read_mask);
            set_rs(D3DRS_STENCILWRITEMASK, d->stencil_write_mask);
            set_rs(D3DRS_STENCILREF, g_b.stencil_ref & 0xFFu);
            /* D3D9's plain stencil states are the clockwise face's. */
            stencil_face(front_ccw ? &d->back : &d->front, 0);
            stencil_face(front_ccw ? &d->front : &d->back, 1);
        }
    } else {
        set_rs(D3DRS_ZENABLE, D3DZB_TRUE);
        set_rs(D3DRS_ZWRITEENABLE, TRUE);
        set_rs(D3DRS_ZFUNC, D3DCMP_LESS);
        set_rs(D3DRS_STENCILENABLE, FALSE);
    }
}

static void apply_samplers(void)
{
    int i;

    for (i = 0; i < 16; i++) {
        const RhiSampler *s = g_b.samp[i];
        const RhiView *v = g_b.tex[i];
        DWORD minf, magf, mipf;
        uint32_t f;

        if (!v || !s)
            continue;
        f = s->d.filter;
        if (f == RHI_FILTER_ANISOTROPIC) {
            minf = magf = D3DTEXF_ANISOTROPIC;
            mipf = D3DTEXF_LINEAR;
        } else {
            minf = (f & 0x10u) ? D3DTEXF_LINEAR : D3DTEXF_POINT;
            magf = (f & 0x04u) ? D3DTEXF_LINEAR : D3DTEXF_POINT;
            mipf = (f & 0x01u) ? D3DTEXF_LINEAR : D3DTEXF_POINT;
        }
        if (s->d.max_lod < 1.0f || (v->image && v->image->desc.mip_levels <= 1))
            mipf = D3DTEXF_NONE;
        set_samp(i, D3DSAMP_MINFILTER, minf);
        set_samp(i, D3DSAMP_MAGFILTER, magf);
        set_samp(i, D3DSAMP_MIPFILTER, mipf);
        set_samp(i, D3DSAMP_ADDRESSU, s->d.address_u ? s->d.address_u : D3DTADDRESS_WRAP);
        set_samp(i, D3DSAMP_ADDRESSV, s->d.address_v ? s->d.address_v : D3DTADDRESS_WRAP);
        set_samp(i, D3DSAMP_ADDRESSW, s->d.address_w ? s->d.address_w : D3DTADDRESS_WRAP);
        set_samp(i, D3DSAMP_BORDERCOLOR, to_d3dcolor(s->d.border));
        set_samp(i, D3DSAMP_MIPMAPLODBIAS, f2dw(s->d.mip_lod_bias));
        set_samp(i, D3DSAMP_MAXANISOTROPY, s->d.max_anisotropy ? s->d.max_anisotropy : 1);
        {
            uint32_t base = v->base_mip;
            if (s->d.min_lod > (float)base)
                base = (uint32_t)s->d.min_lod;
            set_samp(i, D3DSAMP_MAXMIPLEVEL, base);
        }
    }
}

static void apply_constants(void)
{
    int st, slot;

    for (st = 0; st < 2; st++) {
        const int *base = st == 0 ? g_vs_slot_base : g_ps_slot_base;
        int limit = st == 0 ? VS_CLAMP_REG : 224;
        RhiShader *bound = st == 0 ? g_b.vs : g_b.ps;

        if (!bound)
            continue;   /* fixed function reads no registers */
        for (slot = 0; slot < VS_SLOTS; slot++) {
            RhiBuffer *b = g_b.ub[st][slot];
            uint32_t regs;

            if (!b || base[slot] < 0)
                continue;
            if (g_b.ub_sent[st][slot] == b && g_b.ub_sent_version[st][slot] == b->version)
                continue;
            regs = b->desc.size / 16u;
            if (base[slot] + (int)regs > limit)
                regs = (uint32_t)(limit - base[slot]);
            if (st == 0)
                IDirect3DDevice9_SetVertexShaderConstantF(g_dev, base[slot],
                                                          (const float *)b->cpu, regs);
            else
                IDirect3DDevice9_SetPixelShaderConstantF(g_dev, base[slot],
                                                         (const float *)b->cpu, regs);
            g_b.ub_sent[st][slot] = b;
            g_b.ub_sent_version[st][slot] = b->version;
        }
    }
    if (g_b.vs && !g_b.adjust_sent) {
        IDirect3DDevice9_SetVertexShaderConstantF(g_dev, VS_ADJUST_REG, g_b.adjust, 1);
        g_b.adjust_sent = 1;
    }
    if (g_b.vs && !g_b.clamp_sent) {
        IDirect3DDevice9_SetVertexShaderConstantF(g_dev, VS_CLAMP_REG, g_b.clamp, 1);
        g_b.clamp_sent = 1;
    }
}

static int prepare_draw(void)
{
    int i;

    if (!g_dev)
        return 0;
    scene_begin();
    if (g_remake_written)
        for (i = 0; i < 16; i++)
            if (g_b.tex[i] && g_b.tex[i]->image)
                g_b.tex[i]->image->drawn = 1;   /* remake_texture */
    if (g_b.vp_dirty) {
        apply_viewport();
        g_b.vp_dirty = 0;
    }
    if (g_b.ds_dirty) {
        apply_depth_raster();
        g_b.ds_dirty = 0;
    }
    /* After the raster state, which says whether the title's scissor is on. */
    if (g_b.scissor_dirty) {
        apply_scissor();
        g_b.scissor_dirty = 0;
    }
    if (g_b.samp_dirty) {
        apply_samplers();
        g_b.samp_dirty = 0;
    }
    if (g_ff_proj_dirty) {
        apply_projection();
        g_ff_proj_dirty = 0;
    }
    /* The title's alpha test is D3D9's own (it applies under a pixel shader
     * too), and is the title's draws' alone: the overlay and the other
     * passes are drawn without it. */
    set_rs(D3DRS_ALPHATESTENABLE, g_title_draw ? g_title_alpha_test : FALSE);
    g_draw_title = g_title_draw;
    g_title_draw = 0;
    apply_constants();
    if (g_b.layout) {
        /* Stream 0 back from the vertex numbers to what rhi.h bound there:
         * a title binds its vertex buffer once and draws from it many times,
         * across the passes drawn between (the overlay, at least once a
         * frame). Left on the numbers, its level geometry was read from them
         * -- walls gone, stray triangles across the sky -- while its menus,
         * which rebind for every draw, came out right. */
        if (g_b.vid_bound) {
            RhiBuffer *b = g_b.vb[0];
            IDirect3DDevice9_SetStreamSource(g_dev, 0, b ? b->vb : NULL,
                                             b ? g_b.vb_offset[0] : 0,
                                             b ? g_b.vb_stride[0] : 0);
            g_b.vid_bound = 0;
        }
        IDirect3DDevice9_SetVertexDeclaration(g_dev, g_b.layout->decl);
    } else {
        /* A pass with no vertices of its own: the vertex number stream. */
        IDirect3DDevice9_SetVertexDeclaration(g_dev, g_vid_decl);
        IDirect3DDevice9_SetStreamSource(g_dev, 0, g_vid_vb, 0, 4);
        g_b.vid_bound = 1;
    }
    return 1;
}

static UINT prim_count(uint32_t topology, uint32_t n, D3DPRIMITIVETYPE *type)
{
    /* The RHI's topologies are D3D11's, whose list and strip values are
     * D3D9's primitive types. */
    *type = (D3DPRIMITIVETYPE)topology;
    switch (topology) {
    case RHI_TOPOLOGY_POINTS:         return n;
    case RHI_TOPOLOGY_LINES:          return n / 2;
    case RHI_TOPOLOGY_LINE_STRIP:     return n >= 2 ? n - 1 : 0;
    case RHI_TOPOLOGY_TRIANGLES:      return n / 3;
    case RHI_TOPOLOGY_TRIANGLE_STRIP: return n >= 3 ? n - 2 : 0;
    default:                          return 0;
    }
}

/* A refused draw, said for the first few: what was bound, which is what
 * D3D9 checks a draw against. */
static void draw_failed(const char *what, HRESULT hr)
{
    static int said;
    uint32_t tw, th;

    if (said >= 8)
        return;
    said++;
    target_size(&tw, &th);
    fprintf(stderr, "[RHI] d3d9: %s refused (0x%08lX): vs %s, ps %s, layout %s, "
            "target %ux%u, in scene %d\n", what, (unsigned long)hr,
            g_b.vs ? "program" : "fixed", g_b.ps ? "program" : "fixed",
            g_b.layout ? "title's" : "vertex numbers", tw, th, g_in_scene);
}

/* RECOMP_D3D9_SKIP_VS (d3d8_vsh.c), a test switch: the next draw is left out. */
static int g_skip_next;

void rhi_d3d9_skip_next_draw(void)
{
    g_skip_next = 1;
}

/* What the mods are told of the draw in hand (mod_before_draw). */
static RecompModDraw g_mod_d;
static int           g_mod_called;      /* how many befores ran for it */

/* After prepare_draw: the mods' befores, in the order they asked. 0 when
 * the draw is to be left out (the befores after that one are not asked,
 * and no after is called). */
static int mod_before_draw(D3DPRIMITIVETYPE type, UINT prims)
{
    RecompModDraw *d = &g_mod_d;
    const RhiDepthDesc *dd = g_b.depth ? &g_b.depth->d : NULL;
    int i;

    g_mod_called = 0;
    if (!g_mod_draw_n)
        return 1;
    memset(d, 0, sizeof *d);
    d->size = sizeof *d;
    d->device = g_dev;
    d->title = g_draw_title;
    d->two_d = g_draw_title && g_title_2d;
    d->title_2d = g_draw_title && g_title_said_2d;
    d->clip_w = g_title_2d_w;
    d->offscreen = g_title_offscreen;
    d->program = g_b.vs != NULL;
    d->program_id = g_b.vs ? g_b.vs->id : 0;
    d->primitive = (uint32_t)type;
    d->primitives = prims;
    for (i = 0; i < 8; i++) {
        RhiImage *img = g_b.tex[i] ? g_b.tex[i]->image : NULL;

        if (!img)
            continue;
        d->textures[i] = img->tex;
        d->texture_ids[i] = img->id;
        if (img->screen_copy)
            d->screen_copies |= 1u << i;
    }
    if (dd) {
        d->depth_state = 1;
        d->depth_enable = dd->depth_enable != 0;
        d->depth_write = dd->depth_write != 0;
        d->depth_func = dd->depth_func;
    }
    d->projection_set = g_ff_proj_set;
    memcpy(d->projection, g_ff_proj, sizeof d->projection);
    memcpy(d->world, g_dev_world, sizeof d->world);
    memcpy(d->view, g_dev_view, sizeof d->view);
    for (i = 0; i < g_mod_draw_n; i++) {
        if (!g_mod_draw[i].before) {
            g_mod_called++;
            continue;
        }
        if (g_mod_draw[i].before(d, g_mod_draw[i].user) == RECOMP_DRAW_SKIP)
            return 0;
        g_mod_called++;
    }
    return 1;
}

static void mod_after_draw(void)
{
    int i;

    for (i = g_mod_called - 1; i >= 0; i--)
        if (g_mod_draw[i].after)
            g_mod_draw[i].after(&g_mod_d, g_mod_draw[i].user);
    g_mod_called = 0;
}

/* The test switch's draw is left out before anything else sees it. */
static int skip_this_draw(void)
{
    int skip = g_skip_next;

    g_skip_next = 0;
    return skip;
}

static void d9_draw(uint32_t n, uint32_t first)
{
    D3DPRIMITIVETYPE type;
    UINT prims = prim_count(g_b.topology, n, &type);
    HRESULT hr;

    if (skip_this_draw() || !prims || !prepare_draw())
        return;
    if (!g_b.layout && first + n > VID_COUNT)
        return;
    if (!mod_before_draw(type, prims))
        return;
    hr = IDirect3DDevice9_DrawPrimitive(g_dev, type, first, prims);
    if (FAILED(hr))
        draw_failed("DrawPrimitive", hr);
    mod_after_draw();
}

static void d9_draw_indexed(uint32_t n, uint32_t first, int32_t base)
{
    D3DPRIMITIVETYPE type;
    UINT prims = prim_count(g_b.topology, n, &type);
    uint32_t which = g_b.ib_bits == 32 ? 1 : 0, isz = which ? 4 : 2;
    uint32_t bytes = n * isz, src_off, i, lo = 0xFFFFFFFFu, hi = 0;
    const uint8_t *src;
    void *dst;
    DWORD flags = D3DLOCK_NOOVERWRITE;
    HRESULT hr;

    if (skip_this_draw() || !prims || !g_b.ib || !prepare_draw())
        return;
    src_off = g_b.ib_offset + first * isz;
    if (src_off + bytes > g_b.ib->desc.size || bytes > IRING_BYTES)
        return;
    src = g_b.ib->cpu + src_off;

    /* The vertices the draw reaches, which D3D9 asks for. */
    for (i = 0; i < n; i++) {
        uint32_t v;
        if (which) memcpy(&v, src + 4u * i, 4);
        else { uint16_t s; memcpy(&s, src + 2u * i, 2); v = s; }
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }

    if (g_iring_pos[which] + bytes > IRING_BYTES) {
        g_iring_pos[which] = 0;
        flags = D3DLOCK_DISCARD;
    }
    if (FAILED(IDirect3DIndexBuffer9_Lock(g_iring[which], g_iring_pos[which], bytes, &dst, flags)))
        return;
    memcpy(dst, src, bytes);
    IDirect3DIndexBuffer9_Unlock(g_iring[which]);
    IDirect3DDevice9_SetIndices(g_dev, g_iring[which]);
    if (mod_before_draw(type, prims)) {
        hr = IDirect3DDevice9_DrawIndexedPrimitive(g_dev, type, base, lo, hi - lo + 1,
                                                   g_iring_pos[which] / isz, prims);
        if (FAILED(hr))
            draw_failed("DrawIndexedPrimitive", hr);
        mod_after_draw();
    }
    g_iring_pos[which] += (bytes + 15u) & ~15u;
}

static void d9_clear_color(RhiView *v, const float *rgba)
{
    IDirect3DSurface9 *cur = NULL;

    if (!v || !v->surf)
        return;
    /* ColorFill clears a surface without touching what is bound, and
     * without a draw that Remix could mistake for the scene's. */
    if (FAILED(IDirect3DDevice9_ColorFill(g_dev, v->surf, NULL, to_d3dcolor(rgba)))) {
        IDirect3DDevice9_GetRenderTarget(g_dev, 0, &cur);
        IDirect3DDevice9_SetRenderTarget(g_dev, 0, v->surf);
        IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, to_d3dcolor(rgba), 1.0f, 0);
        if (cur) {
            IDirect3DDevice9_SetRenderTarget(g_dev, 0, cur);
            IDirect3DSurface9_Release(cur);
        }
        g_b.vp_dirty = g_b.scissor_dirty = 1;
    }
    mod_cleared(RECOMP_CLEAR_COLOR, 0.0f, 1);
}

static void d9_clear_depth(RhiView *v, uint32_t flags, float z, uint8_t s)
{
    IDirect3DSurface9 *cur = NULL;
    DWORD f = 0;

    if (!v || !v->surf)
        return;
    if (flags & RHI_CLEAR_DEPTH)   f |= D3DCLEAR_ZBUFFER;
    if (flags & RHI_CLEAR_STENCIL) f |= D3DCLEAR_STENCIL;
    if (!f)
        return;
    mod_cleared(((f & D3DCLEAR_ZBUFFER) ? RECOMP_CLEAR_DEPTH : 0) |
                ((f & D3DCLEAR_STENCIL) ? RECOMP_CLEAR_STENCIL : 0), z, 1);
    /* Clear works on what is bound, and clips to the viewport: bind the
     * surface, clear it whole, and put things back. */
    IDirect3DDevice9_GetDepthStencilSurface(g_dev, &cur);
    IDirect3DDevice9_SetDepthStencilSurface(g_dev, v->surf);
    {
        D3DSURFACE_DESC sd;
        D3DVIEWPORT9 vp;
        if (SUCCEEDED(IDirect3DSurface9_GetDesc(v->surf, &sd))) {
            uint32_t tw, th;
            target_size(&tw, &th);
            vp.X = 0; vp.Y = 0;
            vp.Width = sd.Width < tw ? sd.Width : tw;
            vp.Height = sd.Height < th ? sd.Height : th;
            vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
            IDirect3DDevice9_SetViewport(g_dev, &vp);
        }
    }
    set_rs(D3DRS_SCISSORTESTENABLE, FALSE);
    IDirect3DDevice9_Clear(g_dev, 0, NULL, f, 0, z, s);
    IDirect3DDevice9_SetDepthStencilSurface(g_dev, cur);
    if (cur) IDirect3DSurface9_Release(cur);
    g_b.vp_dirty = g_b.ds_dirty = g_b.scissor_dirty = 1;
}

int rhi_d3d9_clear_rects(RhiView *color, RhiView *depth, uint32_t width, uint32_t height,
                         const RhiRect *rects, uint32_t count, uint32_t what,
                         const float rgba[4], float z, uint8_t s)
{
    RhiView *saved_rt = g_b.rt, *saved_ds = g_b.ds;
    D3DVIEWPORT9 vp;
    D3DRECT r[32];
    DWORD f = 0;
    uint32_t i = 0;

    if (!rhi_d3d9_active())
        return -1;
    if ((what & RHI_D3D9_CLEAR_COLOR) && color && color->surf)
        f |= D3DCLEAR_TARGET;
    if (depth && depth->surf) {
        if (what & RHI_CLEAR_DEPTH)   f |= D3DCLEAR_ZBUFFER;
        if (what & RHI_CLEAR_STENCIL) f |= D3DCLEAR_STENCIL;
    }
    if (!f || !width || !height)
        return 0;
    mod_cleared(((f & D3DCLEAR_TARGET) ? RECOMP_CLEAR_COLOR : 0) |
                ((f & D3DCLEAR_ZBUFFER) ? RECOMP_CLEAR_DEPTH : 0) |
                ((f & D3DCLEAR_STENCIL) ? RECOMP_CLEAR_STENCIL : 0), z, 0);

    /* Clear clears what is bound, inside the viewport and the scissor. */
    if (f & D3DCLEAR_TARGET)
        IDirect3DDevice9_SetRenderTarget(g_dev, 0, color->surf);
    IDirect3DDevice9_SetDepthStencilSurface(g_dev, (f & ~D3DCLEAR_TARGET) ? depth->surf : NULL);
    vp.X = vp.Y = 0;
    vp.Width = width;
    vp.Height = height;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    IDirect3DDevice9_SetViewport(g_dev, &vp);
    set_rs(D3DRS_SCISSORTESTENABLE, FALSE);
    while (i < count) {
        DWORD n = 0;
        for (; i < count && n < 32; i++) {
            LONG x0 = rects[i].left < 0 ? 0 : rects[i].left;
            LONG y0 = rects[i].top < 0 ? 0 : rects[i].top;
            LONG x1 = rects[i].right > (int32_t)width ? (LONG)width : rects[i].right;
            LONG y1 = rects[i].bottom > (int32_t)height ? (LONG)height : rects[i].bottom;
            if (x1 <= x0 || y1 <= y0)
                continue;
            r[n].x1 = x0; r[n].y1 = y0; r[n].x2 = x1; r[n].y2 = y1;
            n++;
        }
        if (n)
            IDirect3DDevice9_Clear(g_dev, n, r, f, to_d3dcolor(rgba), z < 0.0f ? 0.0f :
                                   z > 1.0f ? 1.0f : z, s);
    }
    d9_set_render_target(saved_rt, saved_ds);
    g_b.ds_dirty = 1;
    return 0;
}

int rhi_d3d9_copy_rect(RhiView *src, const RhiRect *src_rect, RhiView *dst,
                       const RhiRect *dst_rect)
{
    IDirect3DSurface9 *from;
    RECT sr, dr;
    HRESULT hr;

    if (!rhi_d3d9_active() || !src || !src->image || !dst || !dst->surf)
        return -1;
    from = subresource_surface(src->image, 0);
    if (!from)
        return -1;
    scene_end();
    if (src_rect) {
        sr.left = src_rect->left; sr.top = src_rect->top;
        sr.right = src_rect->right; sr.bottom = src_rect->bottom;
    }
    if (dst_rect) {
        dr.left = dst_rect->left; dr.top = dst_rect->top;
        dr.right = dst_rect->right; dr.bottom = dst_rect->bottom;
    }
    hr = IDirect3DDevice9_StretchRect(g_dev, from, src_rect ? &sr : NULL, dst->surf,
                                      dst_rect ? &dr : NULL, D3DTEXF_POINT);
    IDirect3DSurface9_Release(from);
    if (FAILED(hr))
        return -1;
    if (dst->image)
        dst->image->screen_copy = 1;
    return 0;
}

/* ---- fixed function and Remix --------------------------------------------------------- */

void rhi_d3d9_set_render_state(uint32_t state, uint32_t value)
{
    if (!rhi_d3d9_active())
        return;
    if (state == D3DRS_ALPHATESTENABLE)
        g_title_alpha_test = value ? TRUE : FALSE;  /* at the title's draws (prepare_draw) */
    else
        set_rs((D3DRENDERSTATETYPE)state, value);
}

void rhi_d3d9_title_draw(int two_d, int offscreen)
{
    g_title_draw = 1;
    g_title_offscreen = offscreen != 0;
    g_title_2d = two_d != 0;
    g_title_said_2d = 0;
    g_title_2d_w = 0.0f;
}

void rhi_d3d9_title_says_2d(float w)
{
    if (!g_title_draw || g_b.vs)
        return;
    g_title_2d = g_title_said_2d = 1;
    g_title_2d_w = w;
}

void rhi_d3d9_set_texture_stage(uint32_t stage, uint32_t type, uint32_t value)
{
    if (rhi_d3d9_active())
        set_tss(stage, (D3DTEXTURESTAGESTATETYPE)type, value);
}

void rhi_d3d9_set_transform(uint32_t type, const float m[16])
{
    if (!rhi_d3d9_active() || !m)
        return;
    if (type == D3DTS_PROJECTION) {
        /* Applied at the draw, with the viewport correction folded in. */
        if (g_ff_proj_set && memcmp(g_ff_proj, m, sizeof g_ff_proj) == 0)
            return;
        memcpy(g_ff_proj, m, sizeof g_ff_proj);
        g_ff_proj_set = 1;
        g_ff_proj_dirty = 1;
        return;
    }
    if (type == D3DTS_WORLD)
        memcpy(g_dev_world, m, sizeof g_dev_world);
    else if (type == D3DTS_VIEW)
        memcpy(g_dev_view, m, sizeof g_dev_view);
    IDirect3DDevice9_SetTransform(g_dev, (D3DTRANSFORMSTATETYPE)type, (const D3DMATRIX *)m);
}

void rhi_d3d9_set_light(uint32_t index, const void *light, int enable)
{
    if (!rhi_d3d9_active())
        return;
    if (light)
        IDirect3DDevice9_SetLight(g_dev, index, (const D3DLIGHT9 *)light);
    IDirect3DDevice9_LightEnable(g_dev, index, enable ? TRUE : FALSE);
}

void rhi_d3d9_set_material(const void *material)
{
    if (rhi_d3d9_active() && material)
        IDirect3DDevice9_SetMaterial(g_dev, (const D3DMATERIAL9 *)material);
}

void rhi_d3d9_set_camera(const float world[16], const float view[16], const float proj[16])
{
    if (!rhi_d3d9_active())
        return;
    if (world)
        rhi_d3d9_set_transform(D3DTS_WORLD, world);
    if (view)
        rhi_d3d9_set_transform(D3DTS_VIEW, view);
    /* The projection Remix inverts is the clip space the program writes
     * after RHI_VP_ADJUST, which is what the fixed-function projection is
     * given too: the one transform serves both kinds of draw. */
    if (proj)
        rhi_d3d9_set_transform(D3DTS_PROJECTION, proj);
}

int rhi_d3d9_present_scene(RhiView *scene, uint32_t shape_w, uint32_t shape_h)
{
    IDirect3DSurface9 *src;
    RECT client;
    HRESULT hr;

    if (!rhi_d3d9_active() || !scene || !scene->image || !g_bb_view)
        return -1;
    src = subresource_surface(scene->image, 0);
    if (!src)
        return -1;
    scene_end();
    hr = IDirect3DDevice9_StretchRect(g_dev, src, NULL, g_bb_view->surf, NULL, D3DTEXF_NONE);
    IDirect3DSurface9_Release(src);
    if (FAILED(hr))
        return -1;

    /* Where Present puts it: the largest rectangle of the picture's shape
     * that fits the client area, centred. */
    g_present_rect_set = 0;
    if (shape_w && shape_h && GetClientRect(g_hwnd, &client) && client.right > 0 &&
        client.bottom > 0) {
        LONG cw = client.right, ch = client.bottom, w = cw, h = ch;
        if ((uint64_t)shape_w * (uint64_t)ch > (uint64_t)cw * shape_h)
            h = (LONG)(((uint64_t)cw * shape_h) / shape_w);
        else
            w = (LONG)(((uint64_t)ch * shape_w) / shape_h);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        g_present_rect.left = (cw - w) / 2;
        g_present_rect.top = (ch - h) / 2;
        g_present_rect.right = g_present_rect.left + w;
        g_present_rect.bottom = g_present_rect.top + h;
        g_present_rect_set = 1;
    }
    return 0;
}

const RhiBackend rhi_d3d9_backend = {
    "d3d9",
    d9_device_create, d9_device_destroy, d9_device_ready, d9_swapchain_view, d9_swapchain_resize,
    d9_present, d9_swapchain_readback, d9_swapchain_set_vrr,
    d9_buffer_create, d9_buffer_destroy, d9_buffer_map, d9_buffer_unmap, d9_buffer_update,
    d9_image_create, d9_image_retain, d9_image_destroy, d9_image_get_desc, d9_image_update,
    d9_image_readback,
    d9_view_create, d9_view_destroy, d9_view_image,
    d9_sample_count_supported,
    d9_shader_create, d9_shader_destroy, d9_vertex_layout_create, d9_vertex_layout_destroy,
    d9_blend_state_create, d9_depth_state_create, d9_raster_state_create, d9_sampler_create,
    d9_blend_state_destroy, d9_depth_state_destroy, d9_raster_state_destroy, d9_sampler_destroy,
    d9_set_render_target, d9_output_save, d9_output_restore, d9_output_color_is, d9_set_viewports,
    d9_set_scissor, d9_set_topology, d9_set_vertex_layout, d9_set_vertex_buffer,
    d9_set_index_buffer, d9_set_shader, d9_set_uniform_buffers, d9_set_textures, d9_set_samplers,
    d9_set_blend_state, d9_set_depth_state, d9_set_raster_state, d9_draw, d9_draw_indexed,
    d9_clear_color, d9_clear_depth,
    d9_image_copy,
};

#else /* !_WIN32 */

/* Off Windows there is no D3D9 and these are all no-ops, so the renderer
 * calls them without asking which platform it is on. */
#include "rhi_d3d9.h"

int  rhi_d3d9_active(void) { return 0; }
const char *rhi_d3d9_attr_semantic(int reg, int *index)
{
    (void)reg;
    if (index) *index = 0;
    return "ATTR";
}
void rhi_d3d9_set_render_state(uint32_t state, uint32_t value) { (void)state; (void)value; }
void rhi_d3d9_title_draw(int two_d, int offscreen) { (void)two_d; (void)offscreen; }
void rhi_d3d9_title_says_2d(float w) { (void)w; }
int rhi_d3d9_mod_on_device(RecompModDeviceFn fn, void *user) { (void)fn; (void)user; return 0; }
int rhi_d3d9_mod_on_draw(RecompModBeforeDraw before, RecompModAfterDraw after, void *user)
{
    (void)before; (void)after; (void)user;
    return 0;
}
int rhi_d3d9_mod_on_clear(RecompModClearFn fn, void *user) { (void)fn; (void)user; return 0; }
int rhi_d3d9_mod_on_present(RecompModDeviceFn fn, void *user) { (void)fn; (void)user; return 0; }
int rhi_d3d9_mod_vertex_hlsl(const char *hlsl) { (void)hlsl; return 0; }
int rhi_d3d9_mod_render_state(uint32_t state, uint32_t *value) { (void)state; (void)value; return 0; }
void rhi_d3d9_mod_set_projection(const float m[16]) { (void)m; }
void rhi_d3d9_mod_remake_written_textures(int on) { (void)on; }
int rhi_d3d9_copy_rect(RhiView *src, const RhiRect *src_rect, RhiView *dst,
                       const RhiRect *dst_rect)
{
    (void)src; (void)src_rect; (void)dst; (void)dst_rect;
    return -1;
}
int rhi_d3d9_clear_rects(RhiView *color, RhiView *depth, uint32_t width, uint32_t height,
                         const RhiRect *rects, uint32_t count, uint32_t what,
                         const float rgba[4], float z, uint8_t s)
{
    (void)color; (void)depth; (void)width; (void)height; (void)rects; (void)count;
    (void)what; (void)rgba; (void)z; (void)s;
    return -1;
}
void rhi_d3d9_set_texture_stage(uint32_t stage, uint32_t type, uint32_t value)
{
    (void)stage; (void)type; (void)value;
}
void rhi_d3d9_set_transform(uint32_t type, const float m[16]) { (void)type; (void)m; }
void rhi_d3d9_set_light(uint32_t index, const void *light, int enable)
{
    (void)index; (void)light; (void)enable;
}
void rhi_d3d9_set_material(const void *material) { (void)material; }
void rhi_d3d9_set_camera(const float world[16], const float view[16], const float proj[16])
{
    (void)world; (void)view; (void)proj;
}

void rhi_d3d9_skip_next_draw(void)
{
}
int rhi_d3d9_present_scene(RhiView *scene, uint32_t shape_w, uint32_t shape_h)
{
    (void)scene; (void)shape_w; (void)shape_h;
    return -1;
}

#endif /* _WIN32 */
