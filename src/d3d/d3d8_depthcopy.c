/*
 * d3d8_depthcopy.c -- the device's depth, as the bytes a colour texture over
 * its memory reads.
 *
 * An Xbox title can bind a colour texture whose texels are its depth buffer's
 * memory and read depth in the combiners: the NV2A keeps Z24S8 as one 32-bit
 * word per pixel, depth in bits 31..8 and stencil in 7..0, so a LIN_B8G8R8A8
 * view of it samples the top depth byte as alpha, the middle as red, the low
 * byte as green and the stencil as blue. Rainbow Six 3's post-process does
 * that: its bright pass puts the depth's top byte into alpha, and the depth-
 * of-field blur and the bloom built on it are weighted by it.
 *
 * Under shadow mode the depth is the host's and was never in guest memory, so
 * that texture read whatever the memory last held: the glow was masked by a
 * stale pattern (cut off straight across a third of the way down the screen)
 * and a ghost of the scene sat over every frame. This writes the host's depth
 * in that byte layout into the host texture standing in for the title's. The
 * stencil is not read back (blue is 0): nothing has needed it yet.
 *
 * The host keeps Z. Where the title drew W-buffered (D3DZB_USEW), as RS3
 * draws its world, the Xbox's depth holds W instead -- eye distance over the
 * projection's far W, linear -- so Z is turned back into that through the
 * projection d3d8_GetWBufferProjection kept. Read as Z, everything a few
 * metres away sat at the far plane and the haze covered the picture.
 *
 * Built the way d3d8_screencopy.c is: one oversized triangle, Load at integer
 * pixels so no sampler is bound, the output state put back afterwards.
 */
#include "d3d8_internal.h"
#include "d3d8_xbox.h"
#include "rhi_d3d9.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    int             tried, failed;
    RhiShader      *vs;
    RhiShader      *ps;
    RhiBuffer      *cb;
    RhiBlendState  *blend;
    RhiDepthState  *depth;
    RhiRasterState *raster;
    unsigned long   copies;
} g;

/* Source pixel = destination pixel * scale: the depth may be rendered larger
 * than the guest asked (RECOMP_RES_SCALE). */
typedef struct {
    float scale_x, scale_y, wbuffer, pad0;
    float m33, m34, m43, m44;   /* the W-buffered projection, when wbuffer */
} DepthCopyConstants;

static const char kSource[] =
    "cbuffer DepthCopy : register(b7) { float4 scale; float4 proj; };\n"
    "struct VSOut { float4 pos : SV_Position; };\n"
    "VSOut vs_main(uint id : SV_VertexID) {\n"
    "    float2 c = float2((id << 1) & 2, id & 2);\n"
    "    VSOut o;\n"
    "    o.pos = float4(c * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n"
    "Texture2D<float> depth : register(t9);\n"
    "float4 ps_main(VSOut i) : SV_Target {\n"
    "    int2 p = int2(i.pos.xy * scale.xy);\n"
    "    float d = depth.Load(int3(p, 0));\n"
    /* Z back to W: the eye distance ze from z = (ze*_33 + _43) / (ze*_34 +
     * _44), then w = ze*_34 + _44 over the far W the XDK scales the W-buffer
     * by, (_44 - _43) / (_33 - _34) * _34 + _44 -- the D3D formula. */
    "    if (scale.z != 0.0) {\n"
    "        float ze = (proj.z - d * proj.w) / (d * proj.y - proj.x);\n"
    "        float wfar = (proj.w - proj.z) / (proj.x - proj.y) * proj.y + proj.w;\n"
    "        d = (ze * proj.y + proj.w) / wfar;\n"
    "    }\n"
    "    uint z = (uint)round(saturate(d) * 16777215.0);\n"
    /* Z24S8 read as B8G8R8A8: B = stencil, G = bits 0-7, R = 8-15, A = 16-23. */
    "    return float4(float((z >> 8) & 255u), float(z & 255u), 0.0,\n"
    "                  float((z >> 16) & 255u)) / 255.0;\n"
    "}\n";

static void fail(const char *what)
{
    g.failed = 1;
    fprintf(stderr, "D3D8 depth copy: %s failed; a title that reads its depth "
            "as colour will see what its memory held\n", what);
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
    src.name = "depthcopy";
    src.entry = entry;
    src.target = target;
    s = rhi_shader_create(stage, &src, err, sizeof err);
    if (!s) {
        if (err[0])
            fprintf(stderr, "D3D8 depth copy: %s\n", err);
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
    if (g.tried) return g.ps != NULL;
    g.tried = 1;
    if (!rhi_device_ready()) { g.failed = 1; return 0; }
    if (!d3d8_GetDepthSampledView()) { fail("a sampled view of the device depth"); return 0; }
    if (!(g.vs = compile_one(RHI_STAGE_VERTEX, "vs_main", "vs_4_0"))) return 0;
    if (!(g.ps = compile_one(RHI_STAGE_PIXEL, "ps_main", "ps_4_0"))) return 0;

    memset(&bd, 0, sizeof bd);
    bd.size = sizeof(DepthCopyConstants);
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

HRESULT xbox_D3D8CopyDepthToTexture(IDirect3DTexture8 *dst)
{
    D3D8Texture *tex = (D3D8Texture *)dst;
    RhiImageDesc dd;
    RhiView *rtv, *src, *none = NULL;
    RhiViewDesc rtvd;
    RhiOutputState saved;
    DepthCopyConstants c;
    RhiViewport vp;
    float blend_factor[4] = { 1, 1, 1, 1 };
    void *mapped;

    if (!rhi_device_ready() || !tex || !tex->image || !d3d8_GetDepthImage())
        return E_INVALIDARG;
    /* Not under D3D9: Shader Model 3 has no Load, and the copy would be a
     * full-screen draw that RTX Remix took for the scene's. The texture
     * keeps what it held. */
    if (rhi_d3d9_active()) {
        static int said;
        if (!said++)
            fprintf(stderr, "D3D8 depth copy: not done under the D3D9 backend; an effect "
                    "that reads the depth as a texture will not see it\n");
        return S_OK;
    }
    if (!create())
        return E_FAIL;
    src = d3d8_GetDepthSampledView();
    if (!src)
        return E_FAIL;

    memset(&rtvd, 0, sizeof rtvd);
    rtvd.format = tex->host_format;
    rtvd.dim = RHI_VIEW_DIM_2D;
    rtv = rhi_view_create(tex->image, RHI_VIEW_RENDER_TARGET, &rtvd);
    if (!rtv) {
        static int said;
        if (!said++)
            fprintf(stderr, "D3D8 depth copy: the destination texture is not a render "
                    "target; nothing copied\n");
        return E_FAIL;
    }

    rhi_image_get_desc(d3d8_GetDepthImage(), &dd);
    rhi_output_save(&saved);
    vp.x = vp.y = 0.0f;
    vp.width = (float)tex->width;
    vp.height = (float)tex->height;
    vp.min_depth = 0.0f;
    vp.max_depth = 1.0f;
    c.scale_x = tex->width ? (float)dd.width / (float)tex->width : 1.0f;
    c.scale_y = tex->height ? (float)dd.height / (float)tex->height : 1.0f;
    {
        float m[4] = { 0, 0, 0, 0 };
        c.wbuffer = d3d8_GetWBufferProjection(m) ? 1.0f : 0.0f;
        c.m33 = m[0]; c.m34 = m[1]; c.m43 = m[2]; c.m44 = m[3];
        /* A projection with no far W (orthographic, or none set) cannot be
         * inverted this way; its depth is left as Z. */
        if (c.wbuffer != 0.0f && (c.m34 == 0.0f || c.m33 == c.m34))
            c.wbuffer = 0.0f;
        if (c.wbuffer == 0.0f)
            c.m33 = c.m34 = c.m43 = c.m44 = 0.0f;
    }
    c.pad0 = 0.0f;
    if ((mapped = rhi_buffer_map(g.cb, RHI_MAP_WRITE_DISCARD)) != NULL) {
        memcpy(mapped, &c, sizeof c);
        rhi_buffer_unmap(g.cb);
    }
    /* The depth is a shader input here, so nothing may hold it as depth. */
    rhi_set_render_target(rtv, NULL);
    rhi_set_viewports(1, &vp);
    rhi_set_vertex_layout(NULL);
    rhi_set_topology(RHI_TOPOLOGY_TRIANGLES);
    rhi_set_shader(RHI_STAGE_VERTEX, g.vs);
    rhi_set_shader(RHI_STAGE_PIXEL, g.ps);
    rhi_set_uniform_buffers(RHI_STAGE_PIXEL, 7, 1, &g.cb);
    rhi_set_textures(9, 1, &src);
    rhi_set_blend_state(g.blend, blend_factor, 0xFFFFFFFF);
    rhi_set_depth_state(g.depth, 0);
    rhi_set_raster_state(g.raster);
    rhi_draw(3, 0);
    rhi_set_textures(9, 1, &none);
    rhi_output_restore(&saved);
    rhi_view_destroy(rtv);
    if (!g.copies++)
        fprintf(stderr, "D3D8: the title reads its depth as colour; the host's depth "
                "is copied into that texture as Z24S8 bytes\n");
    return S_OK;
}

void xbox_D3D8DepthCopyShutdown(void)
{
    rhi_shader_destroy(g.vs);            g.vs = NULL;
    rhi_shader_destroy(g.ps);            g.ps = NULL;
    rhi_buffer_destroy(g.cb);            g.cb = NULL;
    rhi_blend_state_destroy(g.blend);    g.blend = NULL;
    rhi_depth_state_destroy(g.depth);    g.depth = NULL;
    rhi_raster_state_destroy(g.raster);  g.raster = NULL;
    g.tried = 0;
}
