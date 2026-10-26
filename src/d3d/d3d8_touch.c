/*
 * d3d8_touch.c -- the on-screen controls over the presented frame
 * (d3d8_touch.h).
 *
 * Drawn the way the overlay is (d3d8_overlay.c): its own shaders, blend,
 * depth and raster state, the constant buffer in b7, no vertex buffer, and
 * the output state put back afterwards. No texture and no sampler.
 */
#include "d3d8_touch.h"

#include <stdio.h>
#include <string.h>

#define MAX_SHAPES 16

typedef struct {
    float head[4];                  /* count, opacity, -, - */
    float shape[MAX_SHAPES][3][4];  /* (x, y, r, kind), (w, h, kx, ky), (r, g, b, pressed) */
} TouchConstants;

/* Each control as a signed distance d (negative inside): a faint fill, a
 * solid fill while held, and an outline just inside the edge, in the
 * control's colour -- Steam Link's look, as DroidDeck draws it. */
static const char kShaderSource[] =
    "cbuffer Touch : register(b7) {\n"
    "    float4 head;\n"
    "    float4 shp[48];\n"
    "};\n"
    "struct VSOut { float4 pos : SV_Position; };\n"
    "VSOut vs_main(uint id : SV_VertexID) {\n"
    "    float2 c = float2((id << 1) & 2, id & 2);\n"
    "    VSOut o;\n"
    "    o.pos = float4(c * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n"
    "float sd_box(float2 p, float2 b, float r) {\n"
    "    float2 q = abs(p) - b + r;\n"
    "    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
    "}\n"
    "void over(inout float4 acc, float3 rgb, float a) {\n"
    "    acc.rgb = acc.rgb * (1.0 - a) + rgb * a;\n"
    "    acc.a = acc.a * (1.0 - a) + a;\n"
    "}\n"
    "float4 ps_main(VSOut i) : SV_Target {\n"
    "    float4 acc = float4(0, 0, 0, 0);\n"
    "    int n = (int)head.x;\n"
    "    for (int k = 0; k < 16; k++) {\n"
    "        if (k >= n) break;\n"
    "        float4 a = shp[k * 3], b = shp[k * 3 + 1], c = shp[k * 3 + 2];\n"
    "        float2 p = i.pos.xy - a.xy;\n"
    "        float r = a.z;\n"
    "        int kind = (int)a.w;\n"
    "        uint pressed = (uint)c.w;\n"
    "        float stroke = max(1.5, r * 0.045);\n"
    "        float d, held = 0.0;\n"
    "        if (kind == 1) {\n"                       /* stick: ring and knob */
    "            d = length(p) - r;\n"
    "            float dk = length(p - b.zw) - r * 0.45;\n"
    "            over(acc, c.rgb, saturate(0.5 - d) * 0.12);\n"
    "            over(acc, c.rgb, saturate(0.5 - (abs(d + stroke) - stroke)));\n"
    "            over(acc, c.rgb, saturate(0.5 - dk) * (pressed != 0 ? 0.85 : 0.45));\n"
    "            continue;\n"
    "        } else if (kind == 3) {\n"                /* pill */
    "            d = sd_box(p, b.xy * 0.5, r);\n"
    "            held = pressed != 0 ? 1.0 : 0.0;\n"
    "        } else if (kind == 4) {\n"                /* d-pad: a cross */
    "            float arm = r * 0.34;\n"
    "            d = min(sd_box(p, float2(r, arm), r * 0.12), sd_box(p, float2(arm, r), r * 0.12));\n"
    "            if ((pressed & 1u) != 0 && p.y < -abs(p.x)) held = 1.0;\n"
    "            if ((pressed & 2u) != 0 && p.y >  abs(p.x)) held = 1.0;\n"
    "            if ((pressed & 4u) != 0 && p.x < -abs(p.y)) held = 1.0;\n"
    "            if ((pressed & 8u) != 0 && p.x >  abs(p.y)) held = 1.0;\n"
    "        } else {\n"                               /* button: a disc */
    "            d = length(p) - r;\n"
    "            held = pressed != 0 ? 1.0 : 0.0;\n"
    "        }\n"
    "        float inside = saturate(0.5 - d);\n"
    "        over(acc, c.rgb, inside * lerp(0.28, 0.85, held));\n"
    "        over(acc, c.rgb, saturate(0.5 - (abs(d + stroke) - stroke)));\n"
    "    }\n"
    "    acc.a *= head.y;\n"
    "    return acc;\n"
    "}\n";

static struct {
    int                tried, failed;
    d3d8_touch_source  source;
    RhiBuffer         *cb;
    RhiShader         *vs, *ps;
    RhiBlendState     *blend;
    RhiDepthState     *depth;
    RhiRasterState    *raster;
} g;

void d3d8_touch_set_source(d3d8_touch_source fn)
{
    g.source = fn;
}

static void touch_fail(const char *what)
{
    g.failed = 1;
    fprintf(stderr, "D3D8 touch controls: %s failed; they are not drawn for this run\n", what);
    fflush(stderr);
    d3d8_touch_shutdown();
}

static RhiShader *compile_one(uint32_t stage, const char *entry, const char *target)
{
    RhiShaderSource src;
    RhiShader *s;
    char err[2048];

    memset(&src, 0, sizeof src);
    src.hlsl = kShaderSource;
    src.len = sizeof kShaderSource - 1;
    src.name = "touch";
    src.entry = entry;
    src.target = target;
    s = rhi_shader_create(stage, &src, err, sizeof err);
    if (!s && err[0])
        fprintf(stderr, "D3D8 touch controls: %s\n", err);
    return s;
}

static int touch_create(void)
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
    bd.size = sizeof(TouchConstants);
    bd.usage = RHI_USAGE_DYNAMIC;
    bd.bind = RHI_BIND_UNIFORM;
    bd.cpu_access = RHI_CPU_WRITE;
    if (!(g.cb = rhi_buffer_create(&bd, NULL))) { touch_fail("CreateBuffer"); return 0; }
    if (!(g.vs = compile_one(RHI_STAGE_VERTEX, "vs_main", "vs_4_0"))) {
        touch_fail("the vertex shader");
        return 0;
    }
    if (!(g.ps = compile_one(RHI_STAGE_PIXEL, "ps_main", "ps_4_0"))) {
        touch_fail("the pixel shader");
        return 0;
    }

    /* The shader's colour is already weighted by its own coverage. */
    memset(&bl, 0, sizeof bl);
    bl.enable = 1;
    bl.src = RHI_BLEND_SRC_ALPHA;
    bl.dst = RHI_BLEND_INV_SRC_ALPHA;
    bl.op = RHI_BLEND_OP_ADD;
    bl.src_alpha = RHI_BLEND_ONE;
    bl.dst_alpha = RHI_BLEND_INV_SRC_ALPHA;
    bl.op_alpha = RHI_BLEND_OP_ADD;
    bl.write_mask = RHI_WRITE_ALL;
    if (!(g.blend = rhi_blend_state_create(&bl))) { touch_fail("CreateBlendState"); return 0; }

    memset(&ds, 0, sizeof ds);
    if (!(g.depth = rhi_depth_state_create(&ds))) { touch_fail("CreateDepthStencilState"); return 0; }

    memset(&rd, 0, sizeof rd);
    rd.fill = RHI_FILL_SOLID;
    rd.cull = RHI_CULL_NONE;
    rd.depth_clip = 1;
    if (!(g.raster = rhi_raster_state_create(&rd))) { touch_fail("CreateRasterizerState"); return 0; }
    return 1;
}

void d3d8_touch_draw(RhiView *out, unsigned w, unsigned h)
{
    TouchShape shapes[MAX_SHAPES];
    TouchConstants c;
    RhiOutputState saved;
    RhiViewport vp;
    float blend_factor[4] = { 1, 1, 1, 1 };
    float opacity = 0.0f;
    void *mapped;
    int n, i;

    if (!g.source || !out || !w || !h)
        return;
    n = g.source(shapes, MAX_SHAPES, (int)w, (int)h, &opacity);
    if (n <= 0 || opacity <= 0.0f || !touch_create())
        return;

    memset(&c, 0, sizeof c);
    c.head[0] = (float)n;
    c.head[1] = opacity;
    for (i = 0; i < n; i++) {
        const TouchShape *s = &shapes[i];
        c.shape[i][0][0] = s->x;
        c.shape[i][0][1] = s->y;
        c.shape[i][0][2] = s->r;
        c.shape[i][0][3] = (float)s->kind;
        c.shape[i][1][0] = s->w;
        c.shape[i][1][1] = s->h;
        c.shape[i][1][2] = s->kx;
        c.shape[i][1][3] = s->ky;
        c.shape[i][2][0] = (float)((s->rgb >> 16) & 0xFF) / 255.0f;
        c.shape[i][2][1] = (float)((s->rgb >> 8) & 0xFF) / 255.0f;
        c.shape[i][2][2] = (float)(s->rgb & 0xFF) / 255.0f;
        c.shape[i][2][3] = (float)s->pressed;
    }

    rhi_output_save(&saved);
    if (!(mapped = rhi_buffer_map(g.cb, RHI_MAP_WRITE_DISCARD)))
        goto restore;
    memcpy(mapped, &c, sizeof c);
    rhi_buffer_unmap(g.cb);

    vp.x = vp.y = 0.0f;
    vp.width = (float)w;
    vp.height = (float)h;
    vp.min_depth = 0.0f;
    vp.max_depth = 1.0f;
    rhi_set_render_target(out, NULL);
    rhi_set_viewports(1, &vp);
    rhi_set_vertex_layout(NULL);
    rhi_set_topology(RHI_TOPOLOGY_TRIANGLES);
    rhi_set_shader(RHI_STAGE_VERTEX, g.vs);
    rhi_set_shader(RHI_STAGE_PIXEL, g.ps);
    rhi_set_uniform_buffers(RHI_STAGE_VERTEX, 7, 1, &g.cb);
    rhi_set_uniform_buffers(RHI_STAGE_PIXEL, 7, 1, &g.cb);
    rhi_set_blend_state(g.blend, blend_factor, 0xFFFFFFFF);
    rhi_set_depth_state(g.depth, 0);
    rhi_set_raster_state(g.raster);
    rhi_draw(3, 0);

restore:
    rhi_output_restore(&saved);
}

void d3d8_touch_shutdown(void)
{
    rhi_buffer_destroy(g.cb);            g.cb = NULL;
    rhi_shader_destroy(g.vs);            g.vs = NULL;
    rhi_shader_destroy(g.ps);            g.ps = NULL;
    rhi_blend_state_destroy(g.blend);    g.blend = NULL;
    rhi_depth_state_destroy(g.depth);    g.depth = NULL;
    rhi_raster_state_destroy(g.raster);  g.raster = NULL;
    g.tried = 0;
}
