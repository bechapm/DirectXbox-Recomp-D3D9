/*
 * d3d8_clear.c -- Clear with rectangles, as D3D8 means it.
 *
 * D3D8's Clear takes rectangles: only those parts of the render target (and
 * of the depth/stencil buffer) are cleared, in the target's own pixels,
 * whatever the viewport and the scissor say. A split-screen title clears
 * each player's quarter this way. The renderer cleared the whole target
 * whatever it was given, which painted one player's view over the others'.
 *
 * Neither graphics API has a clear that does all of this everywhere: D3D11's
 * rectangle clear (ClearView) needs 11.1 hardware and does not touch depth,
 * and Vulkan's (vkCmdClearAttachments) has no D3D11 counterpart. So it is one
 * pass, the same through every backend: a quad per rectangle, drawn with no
 * depth test, writing the colour (or nothing, for a depth-only clear), the
 * depth as the quad's z, and the stencil as the reference value through
 * REPLACE -- with blending, scissor and culling off. A clear with no
 * rectangles, or one that covers the target, never comes here: it stays the
 * backends' native clear (d3d8_device.c, dev_Clear).
 *
 * Under the D3D9 backend it is D3D9's own Clear, which takes rectangles
 * (rhi_d3d9_clear_rects): RTX Remix would take a quad for the scene's.
 *
 * What it touches on the device is what d3d8_overlay.c touches, and it puts
 * back the same: the render target and viewport. Everything else is set
 * again by the next draw (d3d8_states_apply runs per draw), and nothing here
 * binds a texture or a sampler.
 */
#include "d3d8_internal.h"
#include "rhi_d3d9.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    float x0, y0, x1, y1;           /* the rectangle in clip space */
    float r, g, b, a;
    float z, pad0, pad1, pad2;
} ClearConstants;

static const char kSource[] =
    "cbuffer RectClear : register(b7) { float4 rect; float4 color; float4 depth; };\n"
    "float4 vs_main(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 c = float2(id & 1, id >> 1);\n"   /* 0,0  1,0  0,1  1,1 */
    "    return float4(lerp(rect.xy, rect.zw, c), depth.x, 1);\n"
    "}\n"
    "float4 ps_main() : SV_Target { return color; }\n";

static struct {
    int             tried, failed;
    RhiShader      *vs, *ps;
    RhiBuffer      *cb;
    RhiBlendState  *blend[2];       /* [writes colour] */
    RhiDepthState  *depth[4];       /* [writes depth | writes stencil << 1] */
    RhiRasterState *raster;
} g;

static void fail(const char *what)
{
    g.failed = 1;
    fprintf(stderr, "D3D8 clear: %s failed; a Clear with rectangles will clear the "
            "whole target\n", what);
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
    src.name = "rect_clear";
    src.entry = entry;
    src.target = target;
    s = rhi_shader_create(stage, &src, err, sizeof err);
    if (!s && err[0])
        fprintf(stderr, "D3D8 clear: %s\n", err);
    return s;
}

static int create(void)
{
    RhiBufferDesc bd;
    RhiBlendDesc bl;
    RhiDepthDesc ds;
    RhiRasterDesc rd;
    int i;

    if (g.failed) return 0;
    if (g.tried) return g.ps != NULL;
    g.tried = 1;
    if (!rhi_device_ready()) { g.failed = 1; return 0; }

    if (!(g.vs = compile_one(RHI_STAGE_VERTEX, "vs_main", "vs_4_0"))) { fail("the vertex shader"); return 0; }
    if (!(g.ps = compile_one(RHI_STAGE_PIXEL, "ps_main", "ps_4_0"))) { fail("the pixel shader"); return 0; }

    memset(&bd, 0, sizeof bd);
    bd.size = sizeof(ClearConstants);
    bd.usage = RHI_USAGE_DYNAMIC;
    bd.bind = RHI_BIND_UNIFORM;
    bd.cpu_access = RHI_CPU_WRITE;
    if (!(g.cb = rhi_buffer_create(&bd, NULL))) { fail("CreateBuffer"); return 0; }

    for (i = 0; i < 2; i++) {
        memset(&bl, 0, sizeof bl);
        bl.write_mask = i ? RHI_WRITE_ALL : 0;
        if (!(g.blend[i] = rhi_blend_state_create(&bl))) { fail("CreateBlendState"); return 0; }
    }
    for (i = 0; i < 4; i++) {
        memset(&ds, 0, sizeof ds);
        /* Depth: always passes, and is written only for a depth clear. */
        ds.depth_enable = 1;
        ds.depth_write = (i & 1) != 0;
        ds.depth_func = RHI_CMP_ALWAYS;
        if (i & 2) {
            ds.stencil_enable = 1;
            ds.stencil_read_mask = 0xFF;
            ds.stencil_write_mask = 0xFF;
            ds.front.fail = ds.front.depth_fail = ds.front.pass = RHI_STENCIL_REPLACE;
            ds.front.func = RHI_CMP_ALWAYS;
            ds.back = ds.front;
        }
        if (!(g.depth[i] = rhi_depth_state_create(&ds))) { fail("CreateDepthStencilState"); return 0; }
    }

    memset(&rd, 0, sizeof rd);
    rd.fill = RHI_FILL_SOLID;
    rd.cull = RHI_CULL_NONE;
    rd.depth_clip = 1;
    rd.scissor = 0;                 /* the scissor does not apply to Clear */
    if (!(g.raster = rhi_raster_state_create(&rd))) { fail("CreateRasterizerState"); return 0; }
    return 1;
}

int d3d8_clear_rects(RhiView *rtv, RhiView *dsv, UINT width, UINT height,
                     const RhiRect *rects, UINT count, int color, int depth, int stencil,
                     const float rgba[4], float z, uint8_t stencil_value)
{
    RhiOutputState saved;
    RhiViewport vp;
    float blend_factor[4] = { 1, 1, 1, 1 };
    UINT i;

    if (!width || !height || (!color && !depth && !stencil))
        return 0;
    if (rhi_d3d9_active())
        return rhi_d3d9_clear_rects(rtv, dsv, width, height, rects, count,
                                    (color ? RHI_D3D9_CLEAR_COLOR : 0) |
                                    (depth ? RHI_CLEAR_DEPTH : 0) |
                                    (stencil ? RHI_CLEAR_STENCIL : 0),
                                    rgba, z, stencil_value);
    if (!create())
        return -1;
    if (!dsv) {
        depth = stencil = 0;
        if (!color)
            return 0;
    }
    if (!rtv)
        color = 0;
    if (!color && !depth && !stencil)
        return 0;

    rhi_output_save(&saved);
    vp.x = vp.y = 0.0f;
    vp.width = (float)width;
    vp.height = (float)height;
    vp.min_depth = 0.0f;
    vp.max_depth = 1.0f;
    rhi_set_render_target(rtv, dsv);
    rhi_set_viewports(1, &vp);
    rhi_set_vertex_layout(NULL);
    rhi_set_topology(RHI_TOPOLOGY_TRIANGLE_STRIP);
    rhi_set_shader(RHI_STAGE_VERTEX, g.vs);
    rhi_set_shader(RHI_STAGE_PIXEL, g.ps);
    rhi_set_uniform_buffers(RHI_STAGE_VERTEX, 7, 1, &g.cb);
    rhi_set_uniform_buffers(RHI_STAGE_PIXEL, 7, 1, &g.cb);
    rhi_set_blend_state(g.blend[color ? 1 : 0], blend_factor, 0xFFFFFFFF);
    rhi_set_depth_state(g.depth[(depth ? 1 : 0) | (stencil ? 2 : 0)], stencil_value);
    rhi_set_raster_state(g.raster);

    for (i = 0; i < count; i++) {
        ClearConstants c;
        void *mapped;
        int32_t x0 = rects[i].left, y0 = rects[i].top, x1 = rects[i].right, y1 = rects[i].bottom;

        /* Clipped to the target; an empty one clears nothing. */
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int32_t)width) x1 = (int32_t)width;
        if (y1 > (int32_t)height) y1 = (int32_t)height;
        if (x1 <= x0 || y1 <= y0)
            continue;
        c.x0 = -1.0f + 2.0f * (float)x0 / (float)width;
        c.x1 = -1.0f + 2.0f * (float)x1 / (float)width;
        c.y0 =  1.0f - 2.0f * (float)y0 / (float)height;
        c.y1 =  1.0f - 2.0f * (float)y1 / (float)height;
        c.r = rgba[0]; c.g = rgba[1]; c.b = rgba[2]; c.a = rgba[3];
        c.z = z < 0.0f ? 0.0f : z > 1.0f ? 1.0f : z;
        c.pad0 = c.pad1 = c.pad2 = 0.0f;
        if (!(mapped = rhi_buffer_map(g.cb, RHI_MAP_WRITE_DISCARD)))
            break;
        memcpy(mapped, &c, sizeof c);
        rhi_buffer_unmap(g.cb);
        rhi_draw(4, 0);
    }

    rhi_output_restore(&saved);
    return 0;
}

void d3d8_clear_shutdown(void)
{
    int i;

    rhi_shader_destroy(g.vs);            g.vs = NULL;
    rhi_shader_destroy(g.ps);            g.ps = NULL;
    rhi_buffer_destroy(g.cb);            g.cb = NULL;
    for (i = 0; i < 2; i++) { rhi_blend_state_destroy(g.blend[i]); g.blend[i] = NULL; }
    for (i = 0; i < 4; i++) { rhi_depth_state_destroy(g.depth[i]); g.depth[i] = NULL; }
    rhi_raster_state_destroy(g.raster);  g.raster = NULL;
    g.tried = g.failed = 0;
}
