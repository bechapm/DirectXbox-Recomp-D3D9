/**
 * D3D8 Fixed-Function Pipeline Emulation via D3D11 Shaders
 *
 * Emulates the Xbox D3D8 fixed-function pipeline using D3D11
 * programmable shaders. Handles:
 *   - FVF-based vertex formats (XYZ, XYZRHW, Normal, Diffuse, Specular, TexCoord×4)
 *   - World/View/Projection transform
 *   - Pre-transformed vertex passthrough (XYZRHW)
 *   - Up to 4 texture stages with full D3D8 texture operations
 *   - Hardware T&L lighting (up to 8 directional/point/spot lights)
 *   - Vertex fog (linear/exp/exp2)
 *   - Alpha test
 *
 * Shader source is compiled at init time using D3DCompile.
 * When NV2A register combiners are active, the combiner module
 * overrides the pixel shader (d3d8_combiners.c).
 */

#include "d3d8_internal.h"
#include "d3d8_fvf.h"
#include "rhi_d3d9.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
/* malloc: without <stdlib.h> its pointer is truncated to int. */
#include <stdlib.h>


/* ================================================================
 * HLSL vertex shader (embedded)
 *
 * Supports:
 * - Standard transform (WVP) or pre-transformed (XYZRHW) passthrough
 * - Up to 4 texture coordinates
 * - Diffuse + specular vertex colors
 * - D3D8 hardware lighting (up to 8 lights)
 * - Vertex fog (linear/exp/exp2)
 * ================================================================ */

static const char g_vs_source[] =
    "cbuffer TransformCB : register(b0) {\n"
    "    float4x4 WorldViewProj;\n"
    "    float4x4 World;\n"
    "    float4x4 WorldInvTranspose;\n"
    "    float2   ScreenSize;\n"
    "    uint     Flags;\n"
    "    float    _pad0;\n"
    "    float4   EyePos;\n"
    "    float4   FogParams;\n"  /* x=start, y=end, z=density, w=vertex fog mode (0=none,1=exp,2=exp2,3=linear) */
    "    float4x4 WorldView;\n"             /* world*view (camera space) */
    "    float4x4 WorldViewInvTranspose;\n" /* inverse-transpose(world*view), for camera-space normals */
    "    float4x4 TexMat[4];\n"             /* per-stage texture matrices */
    "    uint4    TexCoordIndex;\n"         /* per-stage D3DTSS_TEXCOORDINDEX (TCI + set select) */
    "};\n"
    "\n"
    "cbuffer LightingCB : register(b1) {\n"
    "    float4 GlobalAmbient;\n"
    "    float4 MatDiffuse;\n"
    "    float4 MatAmbient;\n"
    "    float4 MatSpecular;\n"
    "    float4 MatEmissive;\n"
    "    float  MatPower;\n"
    "    uint   NumLights;\n"
    "    float2 _lpad0;\n"
    "    struct {\n"
    "        float4 PosType;\n"     /* xyz=position, w=type (1=point,2=spot,3=dir) */
    "        float4 DirRange;\n"    /* xyz=direction, w=range */
    "        float4 Diffuse;\n"
    "        float4 Ambient;\n"
    "        float4 Specular;\n"
    "        float4 Atten;\n"       /* x=const, y=linear, z=quadratic, w=falloff */
    "        float4 Spot;\n"        /* x=cos(theta/2), y=cos(phi/2), z=0, w=0 */
    "    } Lights[8];\n"
    "};\n"
    "\n"
    "struct VS_IN {\n"
    "    float4 pos      : POSITION;\n"
    "    float3 normal   : NORMAL;\n"
    "    float4 diffuse  : COLOR0;\n"
    "    float4 specular : COLOR1;\n"
    "    float4 tex0     : TEXCOORD0;\n"
    "    float4 tex1     : TEXCOORD1;\n"
    "    float4 tex2     : TEXCOORD2;\n"
    "    float4 tex3     : TEXCOORD3;\n"
    "};\n"
    "\n"
    /* The interface every pixel stage reads (here and d3d8_combiners.c,
     * PS_IN) and every vertex program writes (d3d8_vsh.c, VS_OUT): the
     * stages are linked by register, so the types and the order must agree,
     * not just the names. Coordinates are float4: the combiners' projected
     * and reflection modes read their w. */
    "struct VS_OUT {\n"
    "    float4 pos      : SV_POSITION;\n"
    "    float4 diffuse  : COLOR0;\n"
    "    float4 specular : COLOR1;\n"
    "    float4 tex0     : TEXCOORD0;\n"
    "    float4 tex1     : TEXCOORD1;\n"
    "    float4 tex2     : TEXCOORD2;\n"
    "    float4 tex3     : TEXCOORD3;\n"
    "    float  fog      : TEXCOORD4;\n"
    "    float4 viewpos  : TEXCOORD5;\n"   /* view-space position (per-pixel fog) */
    /* Vulkan: a vertex stage feeding a point list must write the point
     * size, where D3D11 has none (every point is one pixel). __spirv__ is
     * DXC's own macro for a SPIR-V compile, so D3DCompile never sees this,
     * and a builtin takes no varying location, so the pixel stages'
     * matching interface is unchanged. One pixel, as under D3D11. */
    "#ifdef __spirv__\n"
    "    [[vk::builtin(\"PointSize\")]] float vk_point_size : VK_POINT_SIZE;\n"
    "#endif\n"
    "};\n"
    "\n"
    "// Flags: bit0=pretransformed, bit1=hasDiffuse, bit2=hasSpecular, bit3=hasNormal\n"
    "//        bit4=lighting, bit8-11=texCount\n"
    "#define FLAG_PRETRANSFORMED 0x01u\n"
    "#define FLAG_HAS_DIFFUSE    0x02u\n"
    "#define FLAG_HAS_SPECULAR   0x04u\n"
    "#define FLAG_HAS_NORMAL     0x08u\n"
    "#define FLAG_LIGHTING       0x10u\n"
    "\n"
    "// D3DFOGMODE: 0=NONE, 1=EXP, 2=EXP2, 3=LINEAR\n"
    "float compute_fog(float dist) {\n"
    "    uint mode = (uint)FogParams.w;\n"
    "    if (mode == 1u) {\n"  /* EXP */
    "        return saturate(exp(-FogParams.z * dist));\n"
    "    } else if (mode == 2u) {\n"  /* EXP2 */
    "        float e = FogParams.z * dist;\n"
    "        return saturate(exp(-e * e));\n"
    "    } else if (mode == 3u) {\n"  /* LINEAR */
    "        return saturate((FogParams.y - dist) / max(FogParams.y - FogParams.x, 1e-6));\n"
    "    }\n"
    "    return 1.0;\n"  /* no fog */
    "}\n"
    "\n"
    // Generate the texture coordinate for a stage per D3DTSS_TEXCOORDINDEX.
    // Low 16 bits = source texcoord set (PASSTHRU), high bits = TCI mode.
    "float4 gen_tex(uint stage, VS_IN input, float3 posCam, float3 nrmCam) {\n"
    "    uint tci = TexCoordIndex[stage];\n"
    "    uint mode = tci & 0x000F0000u;\n"
    "    uint src  = tci & 0x0000FFFFu;\n"
    "    float4 uv;\n"
    "    if (mode == 0x00010000u) {\n"       /* TCI_CAMERASPACENORMAL */
    "        uv = float4(nrmCam, 1.0);\n"
    "    } else if (mode == 0x00020000u) {\n"   /* TCI_CAMERASPACEPOSITION */
    "        uv = float4(posCam, 1.0);\n"
    "    } else if (mode == 0x00030000u) {\n"   /* TCI_CAMERASPACEREFLECTIONVECTOR */
    "        float3 V = normalize(-posCam);\n"
    "        float3 N = normalize(nrmCam);\n"
    "        uv = float4(reflect(V, N), 1.0);\n"
    "    } else if (mode == 0x00040000u) {\n"   /* TCI_SPHEREMAP */
    "        float3 n = normalize(nrmCam);\n"
    "        float m = 2.0 * sqrt(n.x*n.x + n.y*n.y + (n.z + 1.0)*(n.z + 1.0));\n"
    "        uv = float4(n.x / m + 0.5, n.y / m + 0.5, 0.0, 1.0);\n"
    "    } else {\n"                             /* TCI_PASSTHRU: use texcoord set [src] */
    "        if (src == 1u) uv = input.tex1;\n"
    "        else if (src == 2u) uv = input.tex2;\n"
    "        else if (src == 3u) uv = input.tex3;\n"
    "        else               uv = input.tex0;\n"
    "    }\n"
    "    return mul(uv, TexMat[stage]);\n"
    "}\n"
    "\n"
    "float4 mat_source(uint src, float4 material, VS_IN input) {\n"
    "    if (src == 1u && (Flags & FLAG_HAS_DIFFUSE))  return input.diffuse.bgra;\n"
    "    if (src == 2u && (Flags & FLAG_HAS_SPECULAR)) return input.specular.bgra;\n"
    "    return material;\n"
    "}\n"
    "\n"
    "VS_OUT main(VS_IN input) {\n"
    "    VS_OUT o;\n"
    "#ifdef __spirv__\n"
    "    o.vk_point_size = 1.0;\n"
    "#endif\n"
    "    o.fog = 1.0;\n"
    "    o.specular = float4(0, 0, 0, 0);\n"
    "    o.viewpos = float4(0, 0, 0, 1);\n"
    "\n"
    "    if (Flags & FLAG_PRETRANSFORMED) {\n"
    "        o.pos.x = (input.pos.x / ScreenSize.x) * 2.0 - 1.0;\n"
    "        o.pos.y = 1.0 - (input.pos.y / ScreenSize.y) * 2.0;\n"
    "        o.pos.z = input.pos.z;\n"
    "        o.pos.w = 1.0;\n"
    /* Recover clip W for perspective interpolation without changing screen XYZ.
     * RHW = 0 is degenerate and titles do emit it for 2D overlays that do not
     * care; dividing by it culls the whole quad, so leave those alone. */
    "        if (input.pos.w != 0.0) o.pos /= input.pos.w;\n"
    "        o.tex0 = input.tex0;\n"
    "        o.tex1 = input.tex1;\n"
    "        o.tex2 = input.tex2;\n"
    "        o.tex3 = input.tex3;\n"
    "        o.diffuse = (Flags & FLAG_HAS_DIFFUSE) ? input.diffuse.bgra : float4(1,1,1,1);\n"
    "        if (Flags & FLAG_HAS_SPECULAR) o.specular = input.specular.bgra;\n"
    "        return o;\n"
    "    }\n"
    "\n"
    "    o.pos = mul(float4(input.pos.xyz, 1.0), WorldViewProj);\n"
    "    float4 viewPos4 = mul(float4(input.pos.xyz, 1.0), WorldView);\n"
    "    o.viewpos = viewPos4;\n"
    "    float3 posCam = viewPos4.xyz;\n"
    "    float3 nrmCam = float3(0, 0, 1);\n"
    "\n"
    "    // Vertex colors\n"
    "    float4 vertDiffuse = (Flags & FLAG_HAS_DIFFUSE) ? input.diffuse.bgra : float4(1,1,1,1);\n"
    "    float4 vertSpecular = float4(0,0,0,0);\n"
    "    if (Flags & FLAG_HAS_SPECULAR) vertSpecular = input.specular.bgra;\n"
    "\n"
    "    // World-space normal (lighting) and camera-space normal (TCI / fog)\n"
    "    float3 worldNormal = float3(0, 0, 1);\n"
    "    if (Flags & FLAG_HAS_NORMAL) {\n"
    "        worldNormal = normalize(mul(input.normal, (float3x3)WorldInvTranspose));\n"
    "        nrmCam = normalize(mul(input.normal, (float3x3)WorldViewInvTranspose));\n"
    "    }\n"
    "\n"
    "    // Texture coordinates: per-stage TEXCOORDINDEX + texture matrix\n"
    "    o.tex0 = gen_tex(0, input, posCam, nrmCam);\n"
    "    o.tex1 = gen_tex(1, input, posCam, nrmCam);\n"
    "    o.tex2 = gen_tex(2, input, posCam, nrmCam);\n"
    "    o.tex3 = gen_tex(3, input, posCam, nrmCam);\n"
    "\n"
    "    // Lighting\n"
    "    if ((Flags & FLAG_LIGHTING) && (Flags & FLAG_HAS_NORMAL)) {\n"
    "        float3 worldPos = mul(float4(input.pos.xyz, 1.0), World).xyz;\n"
    "        float3 viewDir = normalize(EyePos.xyz - worldPos);\n"
    "\n"
    /* Each material colour from where D3DRS_*MATERIALSOURCE says: the
     * material, the vertex's diffuse (1) or its specular (2), packed two bits
     * each into Flags 8-15 by the host (diffuse, ambient, specular, emissive)
     * and zero when COLORVERTEX is off. A vertex without that colour falls
     * back to the material, as D3D does. */
    "        float4 mDiffuse  = mat_source((Flags >> 8)  & 3u, MatDiffuse,  input);\n"
    "        float4 mAmbient  = mat_source((Flags >> 10) & 3u, MatAmbient,  input);\n"
    "        float4 mSpecular = mat_source((Flags >> 12) & 3u, MatSpecular, input);\n"
    "        float4 mEmissive = mat_source((Flags >> 14) & 3u, MatEmissive, input);\n"
    "        float4 litDiffuse = mEmissive + mAmbient * GlobalAmbient;\n"
    "        float4 litSpecular = float4(0,0,0,0);\n"
    "\n"
    "        for (uint i = 0; i < NumLights && i < 8u; i++) {\n"
    "            float3 lightDir;\n"
    "            float atten = 1.0;\n"
    "            float spotFactor = 1.0;\n"
    "            uint ltype = (uint)Lights[i].PosType.w;\n"
    "\n"
    "            if (ltype == 3u) {\n"  /* directional */
    "                lightDir = -normalize(Lights[i].DirRange.xyz);\n"
    "            } else {\n"  /* point or spot */
    "                float3 toLight = Lights[i].PosType.xyz - worldPos;\n"
    "                float dist = length(toLight);\n"
    "                if (dist > Lights[i].DirRange.w && Lights[i].DirRange.w > 0) continue;\n"
    "                lightDir = toLight / max(dist, 0.0001);\n"
    "                atten = 1.0 / (Lights[i].Atten.x + Lights[i].Atten.y * dist + Lights[i].Atten.z * dist * dist);\n"
    "\n"
    "                if (ltype == 2u) {\n"  /* spot */
    "                    float cosAngle = dot(-lightDir, normalize(Lights[i].DirRange.xyz));\n"
    "                    float cosOuter = Lights[i].Spot.y;\n"
    "                    float cosInner = Lights[i].Spot.x;\n"
    "                    spotFactor = saturate((cosAngle - cosOuter) / max(cosInner - cosOuter, 0.0001));\n"
    "                    spotFactor = pow(spotFactor, Lights[i].Atten.w);\n"
    "                }\n"
    "            }\n"
    "\n"
    "            float NdotL = max(dot(worldNormal, lightDir), 0.0);\n"
    "            litDiffuse += atten * spotFactor * (Lights[i].Ambient * mAmbient + NdotL * Lights[i].Diffuse * mDiffuse);\n"
    "\n"
    "            if (NdotL > 0.0 && MatPower > 0.0) {\n"
    "                float3 halfVec = normalize(lightDir + viewDir);\n"
    "                float NdotH = max(dot(worldNormal, halfVec), 0.0);\n"
    "                litSpecular += atten * spotFactor * pow(NdotH, MatPower) * Lights[i].Specular * mSpecular;\n"
    "            }\n"
    "        }\n"
    "\n"
    "        o.diffuse = saturate(litDiffuse);\n"
    "        o.diffuse.a = mDiffuse.a;\n"
    "        o.specular = saturate(litSpecular);\n"
    "    } else {\n"
    "        o.diffuse = vertDiffuse;\n"
    "        o.specular = vertSpecular;\n"
    "    }\n"
    "\n"
    "    // Vertex fog (FOGVERTEXMODE)\n"
    "    if ((uint)FogParams.w != 0u) {\n"
    "        float3 worldPos = mul(float4(input.pos.xyz, 1.0), World).xyz;\n"
    "        float fogDist = length(EyePos.xyz - worldPos);\n"
    "        o.fog = compute_fog(fogDist);\n"
    "    }\n"
    "\n"
    "    return o;\n"
    "}\n";

/* ================================================================
 * HLSL pixel shader (embedded)
 *
 * Supports:
 * - Up to 4 texture stages with D3D8 texture operations
 * - Per-stage COLOROP/COLORARG1/COLORARG2/ALPHAOP/ALPHAARG1/ALPHAARG2
 * - Fog blending
 * - Alpha test
 * ================================================================ */

static const char g_ps_body[] =
    "cbuffer PixelCB : register(b0) {\n"
    "    float4 TexFactor;\n"
    "    float4 FogColor;\n"
    "    float4 FogParams;\n"      /* x=start, y=end, z=density, w=table fog mode (D3DFOGMODE) */
    "    float  AlphaRef;\n"
    "    uint   AlphaFunc;\n"
    "    uint   PSFlags;\n"        /* bit 0: alpha test, bit 1: vertex fog, bit 2: specular add, bit 3: table fog, bit 4: range fog, bit 5: program fog coordinate */
    "    uint   _pad0;\n"
    "    // Per-stage: x=colorop, y=colorarg1, z=colorarg2, w=alphaop\n"
    "    uint4  StageColor[4];\n"
    "    // Per-stage: x=alphaarg1, y=alphaarg2, z=0, w=0\n"
    "    uint4  StageAlpha[4];\n"
    "    uint4  AlphaOnly;\n"
    "    // Per-stage texel -> normalised scale; 1 unless the texture is linear\n"
    "    float4 TexScale[4];\n"
    "};\n"
    "\n"
    "struct PS_IN {\n"   /* the same interface as VS_OUT above */
    "    float4 pos      : SV_POSITION;\n"
    "    float4 diffuse  : COLOR0;\n"
    "    float4 specular : COLOR1;\n"
    "    float4 tex0     : TEXCOORD0;\n"
    "    float4 tex1     : TEXCOORD1;\n"
    "    float4 tex2     : TEXCOORD2;\n"
    "    float4 tex3     : TEXCOORD3;\n"
    "    float  fog      : TEXCOORD4;\n"
    "    float4 viewpos  : TEXCOORD5;\n"
    "};\n"
    "\n"
    "// D3DFOGMODE: 0=NONE, 1=EXP, 2=EXP2, 3=LINEAR\n"
    "float compute_pfog(float dist) {\n"
    "    uint mode = (uint)FogParams.w;\n"
    "    if (mode == 1u) {\n"  /* EXP */
    "        return saturate(exp(-FogParams.z * dist));\n"
    "    } else if (mode == 2u) {\n"  /* EXP2 */
    "        float e = FogParams.z * dist;\n"
    "        return saturate(exp(-e * e));\n"
    "    } else if (mode == 3u) {\n"  /* LINEAR */
    "        return saturate((FogParams.y - dist) / max(FogParams.y - FogParams.x, 1e-6));\n"
    "    }\n"
    "    return 1.0;\n"
    "}\n"
    "\n"
    /* Vertex fog under a vertex program. The NV2A does not take a program's
     * oFog as the fog factor: it is a fog coordinate, which the fog unit maps
     * to a factor with the fog range -- for D3DFOG_NONE, linearly between
     * FOGSTART and FOGEND (xemu, pgraph/glsl/vsh.c: fogParam from start and
     * end; an infinite distance or a NaN factor reads as no fog). Taking it
     * as the factor painted Marvel vs Capcom 2's character select in its fog
     * colour: it draws there with fog on, black fog and start = end = -1e7,
     * a range the hardware evaluates as no fog at all. */
    "float program_fog(float d) {\n"
    "    float span = FogParams.y - FogParams.x;\n"
    "    if (span == 0.0 || isinf(d)) return 1.0;\n"
    "    float f = (FogParams.y - d) / span;\n"
    "    if (isnan(f)) return 1.0;\n"
    "    return saturate(f);\n"
    "}\n"
    "\n"
    /* Resolve a texture argument value */
    "float4 resolve_arg(uint arg, float4 diffuse, float4 current,\n"
    "                   float4 texel, float4 tfactor, float4 specular) {\n"
    "    float4 val;\n"
    "    uint base = arg & 0x0Fu;\n"
    "    if      (base == 0u) val = diffuse;\n"   /* D3DTA_DIFFUSE */
    "    else if (base == 1u) val = current;\n"   /* D3DTA_CURRENT */
    "    else if (base == 2u) val = texel;\n"     /* D3DTA_TEXTURE */
    "    else if (base == 3u) val = tfactor;\n"   /* D3DTA_TFACTOR */
    "    else if (base == 4u) val = specular;\n"  /* D3DTA_SPECULAR */
    "    else val = current;\n"
    "    if (arg & 0x10u) val = 1.0 - val;\n"     /* D3DTA_COMPLEMENT */
    "    if (arg & 0x20u) val = val.aaaa;\n"      /* D3DTA_ALPHAREPLICATE */
    "    return val;\n"
    "}\n"
    "\n"
    /* Apply a D3D8 texture blend operation */
    "float apply_op(uint op, float a1, float a2, float diffuse_a, float tex_a, float cur_a, float factor_a) {\n"
    "    if (op <= 1u) return a1;\n"                /* DISABLE - shouldn't reach here, but fallback */
    "    if (op == 2u) return a1;\n"                /* SELECTARG1 */
    "    if (op == 3u) return a2;\n"                /* SELECTARG2 */
    "    if (op == 4u) return a1 * a2;\n"           /* MODULATE */
    "    if (op == 5u) return saturate(a1 * a2 * 2.0);\n"  /* MODULATE2X */
    "    if (op == 6u) return saturate(a1 * a2 * 4.0);\n"  /* MODULATE4X */
    "    if (op == 7u) return saturate(a1 + a2);\n"         /* ADD */
    "    if (op == 8u) return saturate(a1 + a2 - 0.5);\n"  /* ADDSIGNED */
    "    if (op == 9u) return saturate((a1 + a2 - 0.5) * 2.0);\n" /* ADDSIGNED2X */
    "    if (op == 10u) return saturate(a1 - a2);\n"         /* SUBTRACT */
    "    if (op == 11u) return saturate(a1 + a2 - a1 * a2);\n" /* ADDSMOOTH */
    "    if (op == 12u) return lerp(a2, a1, diffuse_a);\n"   /* BLENDDIFFUSEALPHA */
    "    if (op == 13u) return lerp(a2, a1, tex_a);\n"       /* BLENDTEXTUREALPHA */
    "    if (op == 14u) return lerp(a2, a1, factor_a);\n"    /* BLENDFACTORALPHA */
    "    if (op == 15u) return lerp(a2, a1, cur_a);\n"       /* BLENDCURRENTALPHA */
    "    if (op == 24u) {\n"                                  /* DOTPRODUCT3 */
    "        float d = saturate(4.0 * ((a1 - 0.5) * (a2 - 0.5)));\n"
    "        return d;\n"
    "    }\n"
    "    return a1 * a2;\n"  /* fallback = MODULATE */
    "}\n"
    "\n"
    "float4 main(PS_IN input) : SV_TARGET {\n"
    "    float4 current = input.diffuse;\n"
    "    float4 texels[4];\n"
    "\n"
    "    // Pre-sample all textures\n";

static const char g_ps_tail[] =
    "\n"
    "    // Process up to 4 texture stages\n"
    "    [unroll] for (uint i = 0; i < 4; i++) {\n"
    "        uint colorop  = StageColor[i].x;\n"
    "        if (colorop <= 1u) break;\n"  /* D3DTOP_DISABLE = stage off, stop */
    "\n"
    "        uint colorarg1 = StageColor[i].y;\n"
    "        uint colorarg2 = StageColor[i].z;\n"
    "        uint alphaop   = StageColor[i].w;\n"
    "        uint alphaarg1 = StageAlpha[i].x;\n"
    "        uint alphaarg2 = StageAlpha[i].y;\n"
    "\n"
    "        float4 texel = texels[i];\n"
    "\n"
    "        // Resolve color arguments\n"
    "        float4 carg1 = resolve_arg(colorarg1, input.diffuse, current, texel, TexFactor, input.specular);\n"
    "        float4 carg2 = resolve_arg(colorarg2, input.diffuse, current, texel, TexFactor, input.specular);\n"
    "\n"
    "        // Apply color operation per channel\n"
    "        float3 color;\n"
    "        if (colorop == 24u) {\n"
    "            // DOTPRODUCT3: dot of (arg1-0.5)*(arg2-0.5)*4, replicated\n"
    "            float d = saturate(4.0 * dot(carg1.rgb - 0.5, carg2.rgb - 0.5));\n"
    "            color = float3(d, d, d);\n"
    "        } else {\n"
    "            color.r = apply_op(colorop, carg1.r, carg2.r, input.diffuse.a, texel.a, current.a, TexFactor.a);\n"
    "            color.g = apply_op(colorop, carg1.g, carg2.g, input.diffuse.a, texel.a, current.a, TexFactor.a);\n"
    "            color.b = apply_op(colorop, carg1.b, carg2.b, input.diffuse.a, texel.a, current.a, TexFactor.a);\n"
    "        }\n"
    "\n"
    "        // Resolve alpha arguments\n"
    "        float4 aarg1 = resolve_arg(alphaarg1, input.diffuse, current, texel, TexFactor, input.specular);\n"
    "        float4 aarg2 = resolve_arg(alphaarg2, input.diffuse, current, texel, TexFactor, input.specular);\n"
    "\n"
    "        // Apply alpha operation\n"
    "        float alpha;\n"
    "        if (alphaop <= 1u)\n"
    "            alpha = current.a;\n"  /* DISABLE on alpha = keep current */
    "        else\n"
    "            alpha = apply_op(alphaop, aarg1.a, aarg2.a, input.diffuse.a, texel.a, current.a, TexFactor.a);\n"
    "\n"
    "        current = float4(color, alpha);\n"
    "    }\n"
    "\n"
    "    // Add specular (D3DRS_SPECULARENABLE)\n"
    "    if (PSFlags & 4u)\n"
    "        current.rgb = saturate(current.rgb + input.specular.rgb);\n"
    "\n"
    "    // Fog blending\n"
    "    if (PSFlags & 32u)\n"  /* vertex fog under a program: input.fog is its fog coordinate */
    "        current.rgb = lerp(FogColor.rgb, current.rgb, program_fog(input.fog));\n"
    "    else if (PSFlags & 2u)\n"  /* vertex fog: input.fog is the fog factor */
    "        current.rgb = lerp(FogColor.rgb, current.rgb, input.fog);\n"
    "\n"
    "    if (PSFlags & 8u) {\n"  /* table fog: compute fog per-pixel from view-space depth */
    "        float dist = (PSFlags & 16u) ? length(input.viewpos.xyz) : -input.viewpos.z;\n"
    "        float f = compute_pfog(dist);\n"
    "        current.rgb = lerp(FogColor.rgb, current.rgb, f);\n"
    "    }\n"
    "\n";

/* Only in the variant whose signature has FF_PS_SIG_ALPHA_TEST: a shader that
 * contains a discard loses early depth (and, on tile-based GPUs, hidden
 * surface removal) for every draw, whether or not the discard is taken. */
static const char g_ps_alpha_test[] =
    "    // Alpha test\n"
    "    if (PSFlags & 1u) {\n"
    "        bool alphaOk = true;\n"
    "        if      (AlphaFunc == 1u) alphaOk = false;\n"
    "        else if (AlphaFunc == 2u) alphaOk = (current.a < AlphaRef);\n"
    "        else if (AlphaFunc == 3u) alphaOk = (current.a == AlphaRef);\n"
    "        else if (AlphaFunc == 4u) alphaOk = (current.a <= AlphaRef);\n"
    "        else if (AlphaFunc == 5u) alphaOk = (current.a > AlphaRef);\n"
    "        else if (AlphaFunc == 6u) alphaOk = (current.a != AlphaRef);\n"
    "        else if (AlphaFunc == 7u) alphaOk = (current.a >= AlphaRef);\n"
    "        if (!alphaOk) discard;\n"
    "    }\n"
    "\n";

static const char g_ps_end[] =
    "    return current;\n"
    "}\n";

/* Build the fixed-function pixel shader source for a texture signature.
 *
 * The signature packs 2 bits per stage (stages 0..3, low bits first):
 *   0 = 2D texture   (Texture2D,  float2 sample coord)
 *   1 = cube texture (TextureCUBE, float3 sample coord)
 *   2 = 3D texture   (Texture3D,  float3 sample coord)
 * Texture object declarations must match the dimension of the SRV bound
 * at each stage, otherwise D3D11 fails to sample it correctly.
 * Above them, FF_PS_SIG_ALPHA_TEST: the alpha test can reject
 * (d3d8_alpha_test_can_reject), and only then is it in the shader.
 */
#define FF_PS_SRC_SIZE 16384
#define FF_PS_SIG_ALPHA_TEST 0x100u

static void build_ps_source(UINT sig, char *buf, int bufsize)
{
    int off = 0;
    int i;

    for (i = 0; i < 4; i++) {
        UINT dim = (sig >> (i * 2)) & 3u;
        const char *decl = (dim == 1u) ? "TextureCube" :
                           (dim == 2u) ? "Texture3D" : "Texture2D";
        off += snprintf(buf + off, bufsize - off, "%s tex%d : register(t%d);\n",
                        decl, i, i);
        off += snprintf(buf + off, bufsize - off,
                        "SamplerState samp%d : register(s%d);\n", i, i);
    }
    off += snprintf(buf + off, bufsize - off, "%s", g_ps_body);

    for (i = 0; i < 4; i++) {
        UINT dim = (sig >> (i * 2)) & 3u;
        off += snprintf(buf + off, bufsize - off,
                        dim ? "    texels[%d] = tex%d.Sample(samp%d, input.tex%d.xyz);\n"
                            : "    texels[%d] = tex%d.Sample(samp%d, input.tex%d.xy * TexScale[%d].xy);\n",
                        i, i, i, i, i);
        /* Xbox A8 samples white RGB; DXGI A8 supplies zero RGB. */
        off += snprintf(buf + off, bufsize - off,
                        "    if (AlphaOnly[%d]) texels[%d].rgb = 1.0;\n", i, i);
    }
    off += snprintf(buf + off, bufsize - off, "%s", g_ps_tail);
    if (sig & FF_PS_SIG_ALPHA_TEST)
        off += snprintf(buf + off, bufsize - off, "%s", g_ps_alpha_test);
    off += snprintf(buf + off, bufsize - off, "%s", g_ps_end);
}

/* ================================================================
 * Compiled shader objects
 * ================================================================ */

static RhiShader *g_vs = NULL;
static RhiBuffer *g_vs_cb = NULL;       /* VS transform CB (b0) */
static RhiBuffer *g_vs_light_cb = NULL; /* VS lighting CB (b1) */
static RhiBuffer *g_ps_cb = NULL;       /* PS constant buffer */

/* Fixed-function PS cache, keyed by per-stage texture signature.
 * The texture object declarations must match the dimensions of the SRVs
 * bound at each stage, so the PS is recompiled when the set of
 * 2D/cube/volume textures changes. Practically only a handful of
 * signatures ever occur; the alpha test bit can double them, hence 32. */
#define FF_PS_CACHE_SIZE 32

typedef struct {
    UINT       sig;
    RhiShader *ps;
} FfPsCacheEntry;

static FfPsCacheEntry g_ff_ps_cache[FF_PS_CACHE_SIZE];
static int g_ff_ps_cache_count = 0;

/* Get (compiling if needed) the fixed-function pixel shader for a texture
 * signature. Returns NULL and logs on compile failure. */
static RhiShader *ff_ps_get_shader(UINT sig)
{
    RhiShaderSource ss;
    RhiShader *ps;
    char *src;
    char err[4096];
    int i;

    for (i = 0; i < g_ff_ps_cache_count; i++) {
        if (g_ff_ps_cache[i].sig == sig)
            return g_ff_ps_cache[i].ps;
    }

    src = (char *)malloc(FF_PS_SRC_SIZE);
    if (!src) return NULL;
    build_ps_source(sig, src, FF_PS_SRC_SIZE);

    memset(&ss, 0, sizeof ss);
    ss.hlsl = src;
    ss.len = strlen(src);
    ss.name = "ps_ffp";
    ss.entry = "main";
    ss.target = "ps_5_0";
    {
        long long started = d3d8_compile_clock();
        ps = rhi_shader_create(RHI_STAGE_PIXEL, &ss, err, sizeof err);
        d3d8_compile_note(1, started);
    }
    free(src);

    if (!ps) {
        fprintf(stderr, "D3D8: PS compile failed (sig 0x%03X): %s\n", sig,
                err[0] ? err : "unknown");
        return NULL;
    }

    if (g_ff_ps_cache_count >= FF_PS_CACHE_SIZE) {
        /* Evict the oldest entry */
        rhi_shader_destroy(g_ff_ps_cache[0].ps);
        memmove(&g_ff_ps_cache[0], &g_ff_ps_cache[1],
                (FF_PS_CACHE_SIZE - 1) * sizeof(FfPsCacheEntry));
        g_ff_ps_cache_count--;
    }
    g_ff_ps_cache[g_ff_ps_cache_count].sig = sig;
    g_ff_ps_cache[g_ff_ps_cache_count].ps = ps;
    g_ff_ps_cache_count++;

    return ps;
}

/* Compute the per-stage texture signature from the textures currently
 * bound to the pixel shader stages, and the alpha test bit from the render
 * states. */
static UINT ff_ps_compute_signature(void)
{
    UINT sig = d3d8_alpha_test_can_reject(d3d8_GetRenderStates()) ? FF_PS_SIG_ALPHA_TEST : 0;
    int i;

    for (i = 0; i < 4; i++) {
        IDirect3DBaseTexture8 *tex = d3d8_GetStageTexture(i);
        UINT dim = 0;
        if (tex) {
            D3DRESOURCETYPE type = IDirect3DBaseTexture8_GetType(tex);
            if (type == D3DRTYPE_CUBETEXTURE)      dim = 1;
            else if (type == D3DRTYPE_VOLUMETEXTURE) dim = 2;
        }
        sig |= dim << (i * 2);
    }
    return sig;
}

/* VS transform constant buffer layout (must match HLSL TransformCB) */
typedef struct {
    float wvp[16];               /* WorldViewProj (column-major) */
    float world[16];             /* World matrix (column-major) */
    float world_inv_transpose[16]; /* World inverse transpose (column-major) */
    float screen_w, screen_h;
    UINT  flags;
    float _pad0;
    float eye_pos[4];
    float fog_params[4];         /* start, end, density, mode */
    float world_view[16];        /* World*View, camera space (column-major) */
    float world_view_inv_transpose[16]; /* inverse-transpose(World*View) */
    float tex_mat[4][16];        /* per-stage texture matrices (column-major) */
    UINT  texcoord_index[4];     /* per-stage D3DTSS_TEXCOORDINDEX */
} VSTransformConstants;

/* VS lighting constant buffer layout (must match HLSL LightingCB) */
typedef struct {
    float global_ambient[4];
    float mat_diffuse[4];
    float mat_ambient[4];
    float mat_specular[4];
    float mat_emissive[4];
    float mat_power;
    UINT  num_lights;
    float _lpad0[2];
    struct {
        float pos_type[4];       /* xyz=position, w=type */
        float dir_range[4];      /* xyz=direction, w=range */
        float diffuse[4];
        float ambient[4];
        float specular[4];
        float atten[4];          /* const, linear, quad, falloff */
        float spot[4];           /* cos(theta/2), cos(phi/2), 0, 0 */
    } lights[8];
} VSLightingConstants;

/* PS constant buffer layout (must match HLSL PixelCB) */
typedef struct {
    float tex_factor[4];
    float fog_color[4];
    float fog_params[4];         /* start, end, density, table fog mode (D3DFOGMODE) */
    float alpha_ref;
    UINT  alpha_func;
    UINT  ps_flags;              /* bit 0: alpha test, bit 1: vertex fog, bit 2: specular add, bit 3: table fog, bit 4: range fog, bit 5: program fog coordinate */
    UINT  _pad0;
    UINT  stage_color[4][4];     /* [stage][x=colorop, y=arg1, z=arg2, w=alphaop] */
    UINT  stage_alpha[4][4];     /* [stage][x=alphaarg1, y=alphaarg2, z=0, w=0] */
    UINT  alpha_only[4];
    float tex_scale[4][4];       /* [stage][1/w, 1/h, 0, 0] for a linear texture, else 1 */
} PSConstants;

/* ================================================================
 * Input layout cache (FVF -> vertex layout)
 * ================================================================ */

#define MAX_LAYOUT_CACHE 32

typedef struct {
    DWORD            fvf;
    RhiVertexLayout *layout;
} LayoutCacheEntry;

static LayoutCacheEntry g_layout_cache[MAX_LAYOUT_CACHE];
static int g_layout_cache_count = 0;

/* Number of floats in texcoord set `t` (0-3) according to the FVF.
 * Each set uses 2 bits at bit position (16 + t*2); 0 means default (2). */
static UINT fvf_texcoord_size(DWORD fvf, UINT t)
{
    /* D3DFVF_TEXTUREFORMAT1..4 are 3, 0, 1, 2 on both the PC and the Xbox
     * (Cxbx-Reloaded, XbD3D8Types.h), so the field is not the count. */
    static const UINT floats[4] = { 2, 3, 4, 1 };
    return floats[(fvf >> (16 + t * 2)) & 0x3];
}

/*
 * Build input layout for a given FVF.
 *
 * The vertex shader declares all 8 inputs. For FVF components that are
 * missing, we add dummy elements at offset 0 — the shader ignores them
 * via the Flags constant buffer.
 */
static RhiVertexLayout *get_or_create_layout(DWORD fvf)
{
    RhiVertexElement elems[16];
    UINT elem_count = 0;
    UINT offset = 0;
    int i;

    for (i = 0; i < g_layout_cache_count; i++) {
        if (g_layout_cache[i].fvf == fvf)
            return g_layout_cache[i].layout;
    }

    /* POSITION */
    if (d3d8_fvf_transformed(fvf)) {
        elems[elem_count] = (RhiVertexElement){"POSITION", 0, RHI_FORMAT_R32G32B32A32_FLOAT, 0, offset};
        elem_count++;
    } else {
        elems[elem_count] = (RhiVertexElement){"POSITION", 0, RHI_FORMAT_R32G32B32_FLOAT, 0, offset};
        elem_count++;
    }
    offset = d3d8_fvf_position_bytes(fvf);

    /* NORMAL */
    if (fvf & D3DFVF_NORMAL) {
        elems[elem_count] = (RhiVertexElement){"NORMAL", 0, RHI_FORMAT_R32G32B32_FLOAT, 0, offset};
        elem_count++; offset += 12;
    } else {
        elems[elem_count] = (RhiVertexElement){"NORMAL", 0, RHI_FORMAT_R32G32B32_FLOAT, 0, 0};
        elem_count++;
    }

    /* COLOR0 (Diffuse) */
    if (fvf & D3DFVF_DIFFUSE) {
        elems[elem_count] = (RhiVertexElement){"COLOR", 0, RHI_FORMAT_R8G8B8A8_UNORM, 0, offset};
        elem_count++; offset += 4;
    } else {
        elems[elem_count] = (RhiVertexElement){"COLOR", 0, RHI_FORMAT_R8G8B8A8_UNORM, 0, 0};
        elem_count++;
    }

    /* COLOR1 (Specular) */
    if (fvf & D3DFVF_SPECULAR) {
        elems[elem_count] = (RhiVertexElement){"COLOR", 1, RHI_FORMAT_R8G8B8A8_UNORM, 0, offset};
        elem_count++; offset += 4;
    } else {
        elems[elem_count] = (RhiVertexElement){"COLOR", 1, RHI_FORMAT_R8G8B8A8_UNORM, 0, 0};
        elem_count++;
    }

    /* TEXCOORD0-3 */
    {
        UINT tex_count = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
        static const RhiFormat tc_fmt[5] = {
            RHI_FORMAT_R32G32_FLOAT,      /* 2 (default) */
            RHI_FORMAT_R32_FLOAT,         /* 1 */
            RHI_FORMAT_R32G32_FLOAT,      /* 2 */
            RHI_FORMAT_R32G32B32_FLOAT,   /* 3 */
            RHI_FORMAT_R32G32B32A32_FLOAT /* 4 */
        };
        UINT t;
        for (t = 0; t < 4; t++) {
            if (t < tex_count) {
                UINT size = fvf_texcoord_size(fvf, t);
                RhiFormat f = tc_fmt[size];
                elems[elem_count] = (RhiVertexElement){"TEXCOORD", t, f, 0, offset};
                elem_count++; offset += size * 4;
            } else {
                elems[elem_count] = (RhiVertexElement){"TEXCOORD", t, RHI_FORMAT_R32G32_FLOAT, 0, 0};
                elem_count++;
            }
        }
    }

    {
        RhiVertexLayout *layout = rhi_vertex_layout_create(elems, elem_count, g_vs);

        if (!layout)
            fprintf(stderr, "D3D8: CreateInputLayout failed for FVF 0x%lX\n", (unsigned long)fvf);

        if (g_layout_cache_count < MAX_LAYOUT_CACHE) {
            g_layout_cache[g_layout_cache_count].fvf = fvf;
            g_layout_cache[g_layout_cache_count].layout = layout;
            g_layout_cache_count++;
        }

        return layout;
    }
}

/* ================================================================
 * Matrix math helpers
 * ================================================================ */

static void mat4_mul(float *result, const float *a, const float *b)
{
    int i, j, k;
    float tmp[16];
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            tmp[i * 4 + j] = 0;
            for (k = 0; k < 4; k++)
                tmp[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
        }
    memcpy(result, tmp, sizeof(tmp));
}

static void mat4_transpose(float *result, const float *src)
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            result[j * 4 + i] = src[i * 4 + j];
}

/* Compute inverse of a 4x4 matrix (for normal transform).
 * Uses cofactor expansion. Falls back to transpose if singular. */
static void mat4_inverse(float *out, const float *m)
{
    float inv[16], det;
    int i;

    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15]
              + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15]
              - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15]
              + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14]
              - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15]
              - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15]
              + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15]
              - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14]
              + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15]
              + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15]
              - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15]
              + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14]
              - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11]
              - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11]
              + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11]
              - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10]
              + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

    det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (fabsf(det) < 1e-12f) {
        /* Singular — use transpose as fallback */
        mat4_transpose(out, m);
        return;
    }

    det = 1.0f / det;
    for (i = 0; i < 16; i++)
        out[i] = inv[i] * det;
}

/* ================================================================
 * Initialization / Shutdown
 * ================================================================ */

HRESULT d3d8_shaders_init(void)
{
    RhiShaderSource ss;
    RhiBufferDesc cbd;
    char err[4096];

    /* Under D3D9 a fixed-function draw has no shaders, and a program's
     * pixel shader is written per draw (d9_program_ps): only the constant
     * buffer below is wanted, and these sources are not Shader Model 3. */
    if (rhi_d3d9_active())
        goto buffers;

    /* Compile vertex shader */
    memset(&ss, 0, sizeof ss);
    ss.hlsl = g_vs_source;
    ss.len = strlen(g_vs_source);
    ss.name = "vs_ffp";
    ss.entry = "main";
    ss.target = "vs_5_0";
    g_vs = rhi_shader_create(RHI_STAGE_VERTEX, &ss, err, sizeof err);
    if (!g_vs) {
        fprintf(stderr, "D3D8: VS compile failed: %s\n", err[0] ? err : "unknown");
        return E_FAIL;
    }

    /* Compile the default (all-2D) pixel shader */
    g_ff_ps_cache_count = 0;
    memset(g_ff_ps_cache, 0, sizeof(g_ff_ps_cache));
    if (!ff_ps_get_shader(0))
        return E_FAIL;

buffers:
    /* Create VS transform constant buffer (b0) */
    memset(&cbd, 0, sizeof(cbd));
    cbd.size = (sizeof(VSTransformConstants) + 15) & ~15;
    cbd.usage = RHI_USAGE_DYNAMIC;
    cbd.bind = RHI_BIND_UNIFORM;
    cbd.cpu_access = RHI_CPU_WRITE;

    if (!(g_vs_cb = rhi_buffer_create(&cbd, NULL))) return E_FAIL;

    /* Create VS lighting constant buffer (b1) */
    cbd.size = (sizeof(VSLightingConstants) + 15) & ~15;
    if (!(g_vs_light_cb = rhi_buffer_create(&cbd, NULL))) return E_FAIL;

    /* Create PS constant buffer */
    cbd.size = (sizeof(PSConstants) + 15) & ~15;
    if (!(g_ps_cb = rhi_buffer_create(&cbd, NULL))) return E_FAIL;

    fprintf(stderr, "D3D8: Fixed-function shaders compiled OK (multi-texture + lighting + fog)\n");
    return S_OK;
}

static void d9_shutdown(void);

void d3d8_shaders_shutdown(void)
{
    int i;
    for (i = 0; i < g_layout_cache_count; i++)
        rhi_vertex_layout_destroy(g_layout_cache[i].layout);
    g_layout_cache_count = 0;

    for (i = 0; i < g_ff_ps_cache_count; i++)
        rhi_shader_destroy(g_ff_ps_cache[i].ps);
    g_ff_ps_cache_count = 0;
    d9_shutdown();

    rhi_buffer_destroy(g_ps_cb);       g_ps_cb = NULL;
    rhi_buffer_destroy(g_vs_light_cb); g_vs_light_cb = NULL;
    rhi_buffer_destroy(g_vs_cb);       g_vs_cb = NULL;
    rhi_shader_destroy(g_vs);          g_vs = NULL;
}

/* ================================================================
 * Pre-draw binding
 * ================================================================ */

static void ff_vs_prepare_draw(DWORD fvf)
{
    RhiVertexLayout *layout;
    void *mapped;
    const D3DMATRIX *world, *view, *proj;
    const DWORD *rs;
    UINT tex_count;

    if (!rhi_device_ready() || !g_vs) return;

    rs = d3d8_GetRenderStates();
    tex_count = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;

    rhi_set_shader(RHI_STAGE_VERTEX, g_vs);

    /* Bind input layout */
    layout = get_or_create_layout(fvf);
    if (layout)
        rhi_set_vertex_layout(layout);

    /* ---- VS Transform CB (b0) ---- */
    mapped = rhi_buffer_map(g_vs_cb, RHI_MAP_WRITE_DISCARD);
    if (mapped) {
        VSTransformConstants *cb = (VSTransformConstants *)mapped;
        UINT i;
        memset(cb, 0, sizeof(*cb));

        if (d3d8_fvf_transformed(fvf)) {
            float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            memcpy(cb->wvp, identity, sizeof(identity));
            memcpy(cb->world, identity, sizeof(identity));
            memcpy(cb->world_inv_transpose, identity, sizeof(identity));
            memcpy(cb->world_view, identity, sizeof(identity));
            memcpy(cb->world_view_inv_transpose, identity, sizeof(identity));
            /* Pre-transformed vertices are in the guest's pixels of the
             * target they are drawn into: not the host back buffer's (the
             * two differ once the host renders larger than the guest asked,
             * d3d8_GetGuestWidth), and not the screen's when the target is a
             * smaller texture. Measured against the screen, Rainbow Six 3's
             * thermal and night vision blurs into 640x240 and 320x240 targets
             * landed in their top half and top-left quarter, and the vision
             * showed as a 320x120 picture in the corner. */
            {
                UINT tw, th;
                d3d8_GetTargetGuestSize(&tw, &th);
                cb->screen_w = (float)tw;
                cb->screen_h = (float)th;
            }
            cb->flags = 0x01; /* pre-transformed */
        } else {
            float wv[16], wvp[16], wvp_t[16], world_t[16];
            float world_inv[16], world_inv_t[16];
            float wv_t[16], wv_inv[16], wv_inv_t[16];

            world = d3d8_GetTransform(D3DTS_WORLD);
            view  = d3d8_GetTransform(D3DTS_VIEW);
            proj  = d3d8_GetTransform(D3DTS_PROJECTION);

            mat4_mul(wv, (const float *)world, (const float *)view);
            mat4_mul(wvp, wv, (const float *)proj);
            /* Hor+ reaches fixed-function draws too: x is the first column
             * of the row-vector product, scaled by the factor a program's
             * projection register gets for the same draw. Not for an
             * orthographic projection (a constant w column), which places
             * 2D. Pro Evolution Soccer 5 draws its players through a
             * program and their blob shadows through fixed function, and in
             * widescreen each shadow sat outside its player, farther from
             * the middle the farther out he stood. */
            {
                const float *p = (const float *)proj;
                float hp = d3d8_vsh_hor_plus_for_draw();
                BOOL ortho = p[3] == 0.0f && p[7] == 0.0f && p[11] == 0.0f &&
                             p[15] == 1.0f;

                if (hp != 1.0f && !ortho) {
                    int r;
                    for (r = 0; r < 4; r++)
                        wvp[r * 4] *= hp;
                }
            }
            mat4_transpose(wvp_t, wvp);
            memcpy(cb->wvp, wvp_t, sizeof(wvp_t));

            mat4_transpose(world_t, (const float *)world);
            memcpy(cb->world, world_t, sizeof(world_t));

            /* WorldInvTranspose = transpose(inverse(world)) */
            mat4_inverse(world_inv, (const float *)world);
            mat4_transpose(world_inv_t, world_inv);
            memcpy(cb->world_inv_transpose, world_inv_t, sizeof(world_inv_t));

            /* WorldView (camera space) and its inverse-transpose */
            mat4_transpose(wv_t, wv);
            memcpy(cb->world_view, wv_t, sizeof(wv_t));
            mat4_inverse(wv_inv, wv);
            mat4_transpose(wv_inv_t, wv_inv);
            memcpy(cb->world_view_inv_transpose, wv_inv_t, sizeof(wv_inv_t));

            cb->screen_w = (float)d3d8_GetGuestWidth();
            cb->screen_h = (float)d3d8_GetGuestHeight();
            cb->flags = 0;

            /* Compute eye position from inverse view matrix */
            {
                float view_inv[16];
                mat4_inverse(view_inv, (const float *)view);
                cb->eye_pos[0] = view_inv[12];
                cb->eye_pos[1] = view_inv[13];
                cb->eye_pos[2] = view_inv[14];
                cb->eye_pos[3] = 1.0f;
            }
        }

        if (fvf & D3DFVF_DIFFUSE) cb->flags |= 0x02;
        if (fvf & D3DFVF_SPECULAR) cb->flags |= 0x04;
        if (fvf & D3DFVF_NORMAL) cb->flags |= 0x08;
        if (rs && rs[D3DRS_LIGHTING]) cb->flags |= 0x10;
        /* Material sources, two bits each from bit 8 (the shader's
         * mat_source), only while COLORVERTEX is on: off, every colour is
         * the material's. */
        if (rs && rs[D3DRS_COLORVERTEX]) {
            cb->flags |= (rs[D3DRS_DIFFUSEMATERIALSOURCE]  & 3u) << 8;
            cb->flags |= (rs[D3DRS_AMBIENTMATERIALSOURCE]  & 3u) << 10;
            cb->flags |= (rs[D3DRS_SPECULARMATERIALSOURCE] & 3u) << 12;
            cb->flags |= (rs[D3DRS_EMISSIVEMATERIALSOURCE] & 3u) << 14;
        }

        /* Fog parameters */
        if (rs && rs[D3DRS_FOGENABLE]) {
            float fog_start, fog_end, fog_density;
            memcpy(&fog_start, &rs[D3DRS_FOGSTART], sizeof(float));
            memcpy(&fog_end, &rs[D3DRS_FOGEND], sizeof(float));
            memcpy(&fog_density, &rs[D3DRS_FOGDENSITY], sizeof(float));
            cb->fog_params[0] = fog_start;
            cb->fog_params[1] = fog_end;
            cb->fog_params[2] = fog_density;
            if (rs[D3DRS_FOGTABLEMODE] != D3DFOG_NONE) {
                /* Table fog is applied per-pixel in the PS; disable VS fog. */
                cb->fog_params[3] = (float)D3DFOG_NONE;
            } else {
                cb->fog_params[3] = (float)rs[D3DRS_FOGVERTEXMODE];
            }
        }

        /* Per-stage texture matrices and texcoord indices */
        for (i = 0; i < 4; i++) {
            const float *tm = (const float *)d3d8_GetTransform(D3DTS_TEXTURE0 + i);
            const DWORD *ts = d3d8_GetTSS(i);
            /* A stage's matrix applies only while its TEXTURETRANSFORMFLAGS
             * say so. A title sets a matrix for one effect and leaves it
             * behind with the flags back at DISABLE: Max Payne's stage 1
             * keeps a (0.5, -0.5, +0.5) environment-map matrix, and applied
             * regardless it moved every stage 1 lookup -- decal lightmaps
             * landed on other texels (litter drawn black, a graffiti tag at
             * full brightness). */
            if (ts && (ts[D3DTSS_TEXTURETRANSFORMFLAGS] & 0xFFu) == D3DTTFF_DISABLE)
                tm = NULL;
            if (tm) {
                float tm_t[16];
                mat4_transpose(tm_t, tm);
                memcpy(cb->tex_mat[i], tm_t, sizeof(tm_t));
            } else {
                memset(cb->tex_mat[i], 0, sizeof(cb->tex_mat[i]));
                cb->tex_mat[i][0] = cb->tex_mat[i][5] = cb->tex_mat[i][10] = cb->tex_mat[i][15] = 1.0f;
            }
            cb->texcoord_index[i] = ts ? ts[D3DTSS_TEXCOORDINDEX] : (UINT)i;
        }

        rhi_buffer_unmap(g_vs_cb);
    }

    /* ---- VS Lighting CB (b1) ---- */
    mapped = rhi_buffer_map(g_vs_light_cb, RHI_MAP_WRITE_DISCARD);
    if (mapped) {
        VSLightingConstants *lb = (VSLightingConstants *)mapped;
        const D3DMATERIAL8 *mat = d3d8_GetMaterial();
        UINT i, active_count = 0;
        memset(lb, 0, sizeof(*lb));

        /* Global ambient */
        if (rs) {
            DWORD amb = rs[D3DRS_AMBIENT];
            lb->global_ambient[0] = ((amb >> 16) & 0xFF) / 255.0f;
            lb->global_ambient[1] = ((amb >>  8) & 0xFF) / 255.0f;
            lb->global_ambient[2] = ((amb >>  0) & 0xFF) / 255.0f;
            lb->global_ambient[3] = ((amb >> 24) & 0xFF) / 255.0f;
        }

        /* Material */
        if (mat) {
            memcpy(lb->mat_diffuse,  &mat->Diffuse, 16);
            memcpy(lb->mat_ambient,  &mat->Ambient, 16);
            memcpy(lb->mat_specular, &mat->Specular, 16);
            memcpy(lb->mat_emissive, &mat->Emissive, 16);
            lb->mat_power = mat->Power;
        } else {
            lb->mat_diffuse[0] = lb->mat_diffuse[1] = lb->mat_diffuse[2] = lb->mat_diffuse[3] = 1.0f;
            lb->mat_ambient[0] = lb->mat_ambient[1] = lb->mat_ambient[2] = lb->mat_ambient[3] = 1.0f;
        }

        /* Lights — pack enabled lights contiguously */
        for (i = 0; i < d3d8_GetNumLights() && active_count < 8; i++) {
            const D3DLIGHT8 *light;
            if (!d3d8_GetLightEnable(i)) continue;
            light = d3d8_GetLight(i);
            if (!light) continue;

            lb->lights[active_count].pos_type[0] = light->Position.x;
            lb->lights[active_count].pos_type[1] = light->Position.y;
            lb->lights[active_count].pos_type[2] = light->Position.z;
            lb->lights[active_count].pos_type[3] = (float)light->Type;

            lb->lights[active_count].dir_range[0] = light->Direction.x;
            lb->lights[active_count].dir_range[1] = light->Direction.y;
            lb->lights[active_count].dir_range[2] = light->Direction.z;
            lb->lights[active_count].dir_range[3] = light->Range;

            memcpy(lb->lights[active_count].diffuse,  &light->Diffuse, 16);
            memcpy(lb->lights[active_count].ambient,  &light->Ambient, 16);
            memcpy(lb->lights[active_count].specular, &light->Specular, 16);

            lb->lights[active_count].atten[0] = light->Attenuation0;
            lb->lights[active_count].atten[1] = light->Attenuation1;
            lb->lights[active_count].atten[2] = light->Attenuation2;
            lb->lights[active_count].atten[3] = light->Falloff;

            lb->lights[active_count].spot[0] = cosf(light->Theta * 0.5f);
            lb->lights[active_count].spot[1] = cosf(light->Phi * 0.5f);

            active_count++;
        }
        lb->num_lights = active_count;

        rhi_buffer_unmap(g_vs_light_cb);
    }

    {
        RhiBuffer *vs_cbs[2] = { g_vs_cb, g_vs_light_cb };
        rhi_set_uniform_buffers(RHI_STAGE_VERTEX, 0, 2, vs_cbs);
    }
}

/* Whether the draw being prepared runs a vertex program, whose fog output is
 * a fog coordinate rather than a factor (program_fog above). */
static int g_draw_is_program;

int d3d8_shaders_draw_is_program(void)
{
    return g_draw_is_program;
}

/* Whether a texture-stage operation reads D3DTA_TEXTURE (base 2) through
 * one of the arguments it uses. */
static int ff_op_reads_texture(DWORD op, DWORD arg1, DWORD arg2)
{
    if (op <= D3DTOP_DISABLE)
        return 0;
    if (op == D3DTOP_SELECTARG1)
        return (arg1 & 0x0Fu) == 2u;
    if (op == D3DTOP_SELECTARG2)
        return (arg2 & 0x0Fu) == 2u;
    return (arg1 & 0x0Fu) == 2u || (arg2 & 0x0Fu) == 2u;
}

/* ================================================================
 * Direct3D 9, for RTX Remix (rhi_d3d9.h)
 *
 * Under the D3D9 backend a fixed-function draw is drawn by D3D9's own
 * fixed-function pipeline: no shader, and the title's transforms, lights,
 * material, render states and texture stages handed over as they are, which
 * is what Remix rebuilds a scene from. A vertex program's draw is Shader
 * Model 3 (d3d8_vsh.c), which D3D9 pairs only with a ps_3_0, so its pixel
 * stage is the texture stages again as a pixel shader written for the
 * stages' operations (d9_ps_*): the shader above picks its operations at run
 * time with integer constants and bit tests, which Shader Model 3 does not
 * have. The alpha test is D3D9's own under both. Register combiner programs
 * are not translated here: their draws take stage 0's texture times the
 * diffuse colour (docs/technical/d3d9-remix.md).
 * ================================================================ */

static const float g_d9_identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

/* A stage's operations as they are drawn: the D3D8 defaults for an unset
 * operation, and a stage that reads a texture it does not have disabled, as
 * in the pixel shader above. A combiner program has stage 0 modulate its
 * texture by the diffuse colour and nothing after it. */
typedef struct {
    DWORD cop, ca1, ca2, aop, aa1, aa2;
} D9StageOps;

static void d9_stage_ops(UINT stage, IDirect3DBaseTexture8 *tex, D9StageOps *o)
{
    const DWORD *tss = d3d8_GetTSS(stage);

    if (d3d8_combiners_active()) {
        o->cop = o->aop = stage == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        o->ca1 = o->aa1 = D3DTA_TEXTURE;
        o->ca2 = o->aa2 = D3DTA_DIFFUSE;
    } else if (!tss) {
        memset(o, 0, sizeof *o);
        o->cop = o->aop = D3DTOP_DISABLE;
        return;
    } else {
        o->cop = tss[D3DTSS_COLOROP] ? tss[D3DTSS_COLOROP]
                                     : (stage == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE);
        o->ca1 = tss[D3DTSS_COLORARG1];
        o->ca2 = tss[D3DTSS_COLORARG2];
        o->aop = tss[D3DTSS_ALPHAOP] ? tss[D3DTSS_ALPHAOP]
                                     : (stage == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE);
        o->aa1 = tss[D3DTSS_ALPHAARG1];
        o->aa2 = tss[D3DTSS_ALPHAARG2];
    }
    if (!tex && (ff_op_reads_texture(o->cop, o->ca1, o->ca2) ||
                 ff_op_reads_texture(o->aop, o->aa1, o->aa2)))
        o->cop = D3DTOP_DISABLE;
}

/* The scale a linear texture's coordinates take (they are in texels on the
 * NV2A; NV2APSConstants.tex_scale in d3d8_combiners.h), or 0 for none. */
static int d9_texel_scale(IDirect3DBaseTexture8 *tex, float *sx, float *sy)
{
    UINT w = 0, h = 0;

    if (!tex || !d3d8_format_is_linear(d3d8_base_format(tex)) ||
        !d3d8_base_size(tex, &w, &h) || !w || !h)
        return 0;
    *sx = 1.0f / (float)w;
    *sy = 1.0f / (float)h;
    return 1;
}

/* The render states every title draw takes, fixed-function or not. */
static void d9_title_states(DWORD fvf, int program)
{
    static const DWORD forwarded[] = {
        D3DRS_AMBIENT, D3DRS_COLORVERTEX, D3DRS_LOCALVIEWER, D3DRS_NORMALIZENORMALS,
        D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_SPECULARMATERIALSOURCE,
        D3DRS_AMBIENTMATERIALSOURCE, D3DRS_EMISSIVEMATERIALSOURCE, D3DRS_SPECULARENABLE,
        D3DRS_FOGCOLOR, D3DRS_FOGTABLEMODE, D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY,
        D3DRS_RANGEFOGENABLE, D3DRS_FOGVERTEXMODE, D3DRS_TEXTUREFACTOR, D3DRS_SHADEMODE,
        D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC,
    };
    const DWORD *rs = d3d8_GetRenderStates();
    int pre = !program && d3d8_fvf_transformed(fvf);
    size_t i;

    if (!rs)
        return;
    for (i = 0; i < sizeof forwarded / sizeof forwarded[0]; i++)
        rhi_d3d9_set_render_state(forwarded[i], rs[forwarded[i]]);
    /* Lit only with normals to light, as the shader above; never a
     * pre-transformed vertex, nor fogged (its fog would be its specular
     * alpha, which the shader above does not take either). A program's fog
     * is in its pixel shader: D3D9 fogs nothing under a ps_3_0. */
    rhi_d3d9_set_render_state(D3DRS_LIGHTING,
                              !program && !pre && rs[D3DRS_LIGHTING] && (fvf & D3DFVF_NORMAL));
    rhi_d3d9_set_render_state(D3DRS_FOGENABLE, !program && !pre && rs[D3DRS_FOGENABLE]);
}

/* ---- fixed function --------------------------------------------------------------- */

static LayoutCacheEntry g_d9_layout_cache[MAX_LAYOUT_CACHE];
static int g_d9_layout_count;

/* The vertex declaration for an FVF: only what the vertex carries, since
 * D3D9's fixed function reads whatever is declared (a colour declared at
 * offset 0 would be the position's bytes). A pre-transformed position is
 * declared as x, y, z, its rhw skipped, and placed through the projection
 * d9_ff_prepare_draw gives it, so that the viewport (the widescreen squeeze,
 * the host's scale) still applies and Remix sees a 2D draw as orthographic. */
static RhiVertexLayout *d9_ff_layout(DWORD fvf)
{
    static const RhiFormat tc_fmt[5] = {
        RHI_FORMAT_R32G32_FLOAT, RHI_FORMAT_R32_FLOAT, RHI_FORMAT_R32G32_FLOAT,
        RHI_FORMAT_R32G32B32_FLOAT, RHI_FORMAT_R32G32B32A32_FLOAT
    };
    RhiVertexElement elems[16];
    RhiVertexLayout *layout;
    UINT n = 0, offset, t, tex_count;
    int i;

    for (i = 0; i < g_d9_layout_count; i++)
        if (g_d9_layout_cache[i].fvf == fvf)
            return g_d9_layout_cache[i].layout;

    elems[n++] = (RhiVertexElement){"POSITION", 0, RHI_FORMAT_R32G32B32_FLOAT, 0, 0};
    offset = d3d8_fvf_position_bytes(fvf);
    if (fvf & D3DFVF_NORMAL) {
        elems[n++] = (RhiVertexElement){"NORMAL", 0, RHI_FORMAT_R32G32B32_FLOAT, 0, offset};
        offset += 12;
    }
    /* A D3DCOLOR, which is B8G8R8A8 in memory */
    if (fvf & D3DFVF_DIFFUSE) {
        elems[n++] = (RhiVertexElement){"COLOR", 0, RHI_FORMAT_B8G8R8A8_UNORM, 0, offset};
        offset += 4;
    }
    if (fvf & D3DFVF_SPECULAR) {
        elems[n++] = (RhiVertexElement){"COLOR", 1, RHI_FORMAT_B8G8R8A8_UNORM, 0, offset};
        offset += 4;
    }
    tex_count = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (t = 0; t < tex_count && t < 8; t++) {
        UINT size = fvf_texcoord_size(fvf, t);
        elems[n++] = (RhiVertexElement){"TEXCOORD", t, tc_fmt[size], 0, offset};
        offset += size * 4;
    }

    layout = rhi_vertex_layout_create(elems, n, NULL);
    if (!layout)
        fprintf(stderr, "D3D8: D3D9 vertex declaration failed for FVF 0x%lX\n", (unsigned long)fvf);
    if (g_d9_layout_count < MAX_LAYOUT_CACHE) {
        g_d9_layout_cache[g_d9_layout_count].fvf = fvf;
        g_d9_layout_cache[g_d9_layout_count].layout = layout;
        g_d9_layout_count++;
    }
    return layout;
}

static void d9_ff_lights_and_material(void)
{
    static D3DLIGHT8 sent[8];
    static BOOL sent_valid[8], sent_on[8];
    static D3DMATERIAL8 sent_mat;
    static BOOL sent_mat_valid;
    const D3DMATERIAL8 *mat = d3d8_GetMaterial();
    D3DMATERIAL8 white;
    UINT i, n = 0;

    /* The enabled lights, packed into the first eight as the shader above
     * packs them. */
    for (i = 0; i < d3d8_GetNumLights() && n < 8; i++) {
        const D3DLIGHT8 *light;

        if (!d3d8_GetLightEnable(i) || !(light = d3d8_GetLight(i)))
            continue;
        if (!sent_valid[n] || memcmp(&sent[n], light, sizeof *light) != 0) {
            rhi_d3d9_set_light(n, light, 1);
            sent[n] = *light;
            sent_valid[n] = sent_on[n] = TRUE;
        } else if (!sent_on[n]) {
            rhi_d3d9_set_light(n, NULL, 1);
            sent_on[n] = TRUE;
        }
        n++;
    }
    for (; n < 8; n++)
        if (sent_on[n]) {
            rhi_d3d9_set_light(n, NULL, 0);
            sent_on[n] = FALSE;
        }

    if (!mat) {
        memset(&white, 0, sizeof white);
        white.Diffuse.r = white.Diffuse.g = white.Diffuse.b = white.Diffuse.a = 1.0f;
        memcpy(&white.Ambient, &white.Diffuse, sizeof white.Ambient);
        mat = &white;
    }
    if (!sent_mat_valid || memcmp(&sent_mat, mat, sizeof *mat) != 0) {
        rhi_d3d9_set_material(mat);
        sent_mat = *mat;
        sent_mat_valid = TRUE;
    }
}

static void d9_ff_prepare_draw(DWORD fvf)
{
    float proj[16];
    RhiVertexLayout *layout;
    UINT i;

    rhi_set_shader(RHI_STAGE_VERTEX, NULL);
    rhi_set_shader(RHI_STAGE_PIXEL, NULL);
    layout = d9_ff_layout(fvf);
    if (layout)
        rhi_set_vertex_layout(layout);

    if (d3d8_fvf_transformed(fvf)) {
        /* Guest pixels of the target to clip space, as the shader above
         * (d3d8_GetTargetGuestSize says why the target's). */
        UINT tw = 0, th = 0;

        d3d8_GetTargetGuestSize(&tw, &th);
        memset(proj, 0, sizeof proj);
        proj[0] = 2.0f / (float)(tw ? tw : 1);
        proj[5] = -2.0f / (float)(th ? th : 1);
        proj[10] = 1.0f;
        proj[12] = -1.0f;
        proj[13] = 1.0f;
        proj[15] = 1.0f;
        rhi_d3d9_set_transform(D3DTS_WORLD, g_d9_identity);
        rhi_d3d9_set_transform(D3DTS_VIEW, g_d9_identity);
    } else {
        /* Hor+ as above: the projection's x column, unless orthographic. */
        float hp = d3d8_vsh_hor_plus_for_draw();
        BOOL ortho;

        memcpy(proj, d3d8_GetTransform(D3DTS_PROJECTION), sizeof proj);
        ortho = proj[3] == 0.0f && proj[7] == 0.0f && proj[11] == 0.0f && proj[15] == 1.0f;
        if (hp != 1.0f && !ortho) {
            int r;
            for (r = 0; r < 4; r++)
                proj[r * 4] *= hp;
        }
        rhi_d3d9_set_transform(D3DTS_WORLD, (const float *)d3d8_GetTransform(D3DTS_WORLD));
        rhi_d3d9_set_transform(D3DTS_VIEW, (const float *)d3d8_GetTransform(D3DTS_VIEW));
        d9_ff_lights_and_material();
    }
    rhi_d3d9_set_transform(D3DTS_PROJECTION, proj);
    d9_title_states(fvf, 0);

    /* The texture stages. Stages after the first one disabled are not
     * reached, and stages 4-7 are left as D3D9 starts them, disabled. */
    for (i = 0; i < 4; i++) {
        IDirect3DBaseTexture8 *tex = d3d8_GetStageTexture(i);
        const DWORD *tss = d3d8_GetTSS(i);
        DWORD ttff = tss ? tss[D3DTSS_TEXTURETRANSFORMFLAGS] : D3DTTFF_DISABLE;
        float m[16], sx, sy;
        D9StageOps o;

        d9_stage_ops(i, tex, &o);
        rhi_d3d9_set_texture_stage(i, D3DTSS_COLOROP, o.cop);
        if (o.cop == D3DTOP_DISABLE)
            break;
        rhi_d3d9_set_texture_stage(i, D3DTSS_COLORARG1, o.ca1);
        rhi_d3d9_set_texture_stage(i, D3DTSS_COLORARG2, o.ca2);
        /* An alpha operation of DISABLE keeps the alpha before it, which in
         * D3D9 is selecting CURRENT. */
        if (o.aop == D3DTOP_DISABLE) {
            o.aop = D3DTOP_SELECTARG1;
            o.aa1 = D3DTA_CURRENT;
        }
        rhi_d3d9_set_texture_stage(i, D3DTSS_ALPHAOP, o.aop);
        rhi_d3d9_set_texture_stage(i, D3DTSS_ALPHAARG1, o.aa1);
        rhi_d3d9_set_texture_stage(i, D3DTSS_ALPHAARG2, o.aa2);
        rhi_d3d9_set_texture_stage(i, D3DTSS_TEXCOORDINDEX, tss ? tss[D3DTSS_TEXCOORDINDEX] : i);
        if (tss) {
            DWORD k;
            for (k = D3DTSS_BUMPENVMAT00; k <= D3DTSS_BUMPENVMAT11; k++)
                rhi_d3d9_set_texture_stage(i, k, tss[k]);
        }

        /* The texture matrix, while the flags apply it, and a linear
         * texture's texel scale after it. */
        if ((ttff & 0xFFu) == D3DTTFF_DISABLE)
            memcpy(m, g_d9_identity, sizeof m);
        else
            memcpy(m, d3d8_GetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + i)), sizeof m);
        if (d9_texel_scale(tex, &sx, &sy)) {
            int r;
            for (r = 0; r < 4; r++) {
                m[r * 4 + 0] *= sx;
                m[r * 4 + 1] *= sy;
            }
            if ((ttff & 0xFFu) == D3DTTFF_DISABLE)
                ttff = D3DTTFF_COUNT2;
        }
        rhi_d3d9_set_transform(D3DTS_TEXTURE0 + i, m);
        rhi_d3d9_set_texture_stage(i, RHI_D3D9_TSS_TEXTURETRANSFORMFLAGS, ttff);
    }
}

/* ---- the pixel stage under a vertex program --------------------------------------- */

/* What a program draw's pixel shader is written for. */
enum { D9_FOG_NONE, D9_FOG_PROGRAM, D9_FOG_EXP, D9_FOG_EXP2, D9_FOG_LINEAR };

typedef struct {
    D9StageOps op[4];
    uint8_t    dim[4];      /* 0 none, 1 2D, 2 cube, 3 volume */
    uint8_t    fog;         /* D9_FOG_* */
    uint8_t    range_fog;
    uint8_t    specular;
    uint8_t    _pad;
} D9PsKey;

/* Its constants, from c16 (rhi_d3d9.c's register for uniform buffer b0) */
typedef struct {
    float tex_factor[4];
    float fog_color[4];
    float fog_params[4];        /* start, end, density */
    float tex_scale[4][4];      /* 1/w, 1/h for a linear texture, else 1 */
} D9PSConstants;

#define D9_PS_CACHE_SIZE 128

typedef struct {
    D9PsKey    key;
    RhiShader *ps;
} D9PsCacheEntry;

static D9PsCacheEntry g_d9_ps_cache[D9_PS_CACHE_SIZE];
static int g_d9_ps_count;

static int d9_arg(char *out, size_t n, DWORD arg, int stage, int has_tex)
{
    char tex[8];
    const char *base;

    snprintf(tex, sizeof tex, "t%d", stage);
    switch (arg & 0x0Fu) {
    case D3DTA_DIFFUSE:  base = "input.diffuse"; break;
    case D3DTA_TEXTURE:  base = has_tex ? tex : "float4(1, 1, 1, 1)"; break;
    case D3DTA_TFACTOR:  base = "TexFactor"; break;
    case D3DTA_SPECULAR: base = "input.specular"; break;
    default:             base = "current"; break;
    }
    if (arg & D3DTA_COMPLEMENT)
        return snprintf(out, n, (arg & D3DTA_ALPHAREPLICATE) ? "(1.0 - %s).aaaa" : "(1.0 - %s)",
                        base);
    return snprintf(out, n, (arg & D3DTA_ALPHAREPLICATE) ? "%s.aaaa" : "%s", base);
}

/* One operation on a1 and a2's components `c` (rgb or a), as apply_op in the
 * shader above, including its fallback to MODULATE. */
static int d9_op(char *out, size_t n, DWORD op, const char *c, const char *tex_a)
{
    char x[8], y[8];

    snprintf(x, sizeof x, "a1.%s", c);
    snprintf(y, sizeof y, "a2.%s", c);
    switch (op) {
    case D3DTOP_SELECTARG1:         return snprintf(out, n, "%s", x);
    case D3DTOP_SELECTARG1 + 1:     return snprintf(out, n, "%s", y);
    case D3DTOP_MODULATE:           return snprintf(out, n, "%s * %s", x, y);
    case 5:  return snprintf(out, n, "saturate(%s * %s * 2.0)", x, y);
    case 6:  return snprintf(out, n, "saturate(%s * %s * 4.0)", x, y);
    case 7:  return snprintf(out, n, "saturate(%s + %s)", x, y);
    case 8:  return snprintf(out, n, "saturate(%s + %s - 0.5)", x, y);
    case 9:  return snprintf(out, n, "saturate((%s + %s - 0.5) * 2.0)", x, y);
    case 10: return snprintf(out, n, "saturate(%s - %s)", x, y);
    case 11: return snprintf(out, n, "saturate(%s + %s - %s * %s)", x, y, x, y);
    case D3DTOP_BLENDDIFFUSEALPHA:  return snprintf(out, n, "lerp(%s, %s, input.diffuse.a)", y, x);
    case D3DTOP_BLENDTEXTUREALPHA:  return snprintf(out, n, "lerp(%s, %s, %s)", y, x, tex_a);
    case D3DTOP_BLENDFACTORALPHA:   return snprintf(out, n, "lerp(%s, %s, TexFactor.a)", y, x);
    case D3DTOP_BLENDCURRENTALPHA:  return snprintf(out, n, "lerp(%s, %s, current.a)", y, x);
    case D3DTOP_DOTPRODUCT3:
        return snprintf(out, n, "saturate(4.0 * (%s - 0.5) * (%s - 0.5))", x, y);
    default:                        return snprintf(out, n, "%s * %s", x, y);
    }
}

static void d9_ps_source(const D9PsKey *k, char *buf, int size)
{
    static const char *const sampler[4] = { "", "sampler2D", "samplerCUBE", "sampler3D" };
    static const char *const fetch[4] = { "", "tex2D", "texCUBE", "tex3D" };
    char a1[48], a2[48], expr[160], tex_a[24];
    int off = 0, i;

#define EMIT(...) do { if (off < size) off += snprintf(buf + off, (size_t)(size - off), __VA_ARGS__); } while (0)
    EMIT("float4 TexFactor   : register(c16);\n"
        "float4 FogColor    : register(c17);\n"
        "float4 FogParams   : register(c18);\n"
        "float4 TexScale[4] : register(c19);\n");
    for (i = 0; i < 4; i++)
        if (k->dim[i])
            EMIT("%s s%d : register(s%d);\n", sampler[k->dim[i]], i, i);
    EMIT("\n"
        "struct PS_IN {\n"
        "    float4 diffuse  : COLOR0;\n"
        "    float4 specular : COLOR1;\n"
        "    float4 tex0     : TEXCOORD0;\n"
        "    float4 tex1     : TEXCOORD1;\n"
        "    float4 tex2     : TEXCOORD2;\n"
        "    float4 tex3     : TEXCOORD3;\n"
        "    float  fog      : TEXCOORD4;\n"
        "};\n"
        "\n"
        "float4 main(PS_IN input) : COLOR0 {\n"
        "    float4 current = input.diffuse;\n"
        "    float4 a1, a2, r;\n");
    for (i = 0; i < 4; i++) {
        const D9StageOps *o = &k->op[i];
        int has_tex = k->dim[i] != 0;

        if (o->cop == D3DTOP_DISABLE)
            break;
        if (has_tex) {
            if (k->dim[i] == 1)
                EMIT("    float4 t%d = tex2D(s%d, input.tex%d.xy * TexScale[%d].xy);\n", i, i, i, i);
            else
                EMIT("    float4 t%d = %s(s%d, input.tex%d.xyz);\n", i, fetch[k->dim[i]], i, i);
        }
        snprintf(tex_a, sizeof tex_a, has_tex ? "t%d.a" : "1.0", i);

        d9_arg(a1, sizeof a1, o->ca1, i, has_tex);
        d9_arg(a2, sizeof a2, o->ca2, i, has_tex);
        EMIT("    a1 = %s;\n    a2 = %s;\n", a1, a2);
        if (o->cop == D3DTOP_DOTPRODUCT3) {
            EMIT("    r.rgb = saturate(4.0 * dot(a1.rgb - 0.5, a2.rgb - 0.5)).xxx;\n");
        } else {
            d9_op(expr, sizeof expr, o->cop, "rgb", tex_a);
            EMIT("    r.rgb = %s;\n", expr);
        }
        if (o->aop == D3DTOP_DISABLE) {
            EMIT("    r.a = current.a;\n");
        } else {
            d9_arg(a1, sizeof a1, o->aa1, i, has_tex);
            d9_arg(a2, sizeof a2, o->aa2, i, has_tex);
            d9_op(expr, sizeof expr, o->aop, "a", tex_a);
            EMIT("    a1 = %s;\n    a2 = %s;\n    r.a = %s;\n", a1, a2, expr);
        }
        EMIT("    current = r;\n");
    }
    if (k->specular)
        EMIT("    current.rgb = saturate(current.rgb + input.specular.rgb);\n");
    switch (k->fog) {
    case D9_FOG_PROGRAM:
        /* program_fog above; an infinite coordinate is no fog */
        EMIT("    {\n"
            "        float span = FogParams.y - FogParams.x;\n"
            "        float f = (span == 0.0 || abs(input.fog) > 3.0e38) ? 1.0\n"
            "                : saturate((FogParams.y - input.fog) / span);\n"
            "        current.rgb = lerp(FogColor.rgb, current.rgb, f);\n"
            "    }\n");
        break;
    case D9_FOG_EXP:
    case D9_FOG_EXP2:
    case D9_FOG_LINEAR:
        /* Table fog, from the program's fog coordinate as the distance (the
         * view position the shader above reads is made from it) */
        EMIT("    {\n"
            "        float d = %s;\n", k->range_fog ? "abs(input.fog)" : "input.fog");
        if (k->fog == D9_FOG_EXP)
            EMIT("        float f = saturate(exp(-FogParams.z * d));\n");
        else if (k->fog == D9_FOG_EXP2)
            EMIT("        float e = FogParams.z * d;\n"
                "        float f = saturate(exp(-e * e));\n");
        else
            EMIT("        float f = saturate((FogParams.y - d) / max(FogParams.y - FogParams.x, 1e-6));\n");
        EMIT("        current.rgb = lerp(FogColor.rgb, current.rgb, f);\n"
            "    }\n");
        break;
    default:
        break;
    }
    EMIT("    return current;\n}\n");
#undef EMIT
}

static RhiShader *d9_ps_get(const D9PsKey *key)
{
    RhiShaderSource ss;
    RhiShader *ps;
    char *src, err[4096];
    int i;

    for (i = 0; i < g_d9_ps_count; i++)
        if (memcmp(&g_d9_ps_cache[i].key, key, sizeof *key) == 0)
            return g_d9_ps_cache[i].ps;

    src = (char *)malloc(FF_PS_SRC_SIZE);
    if (!src)
        return NULL;
    d9_ps_source(key, src, FF_PS_SRC_SIZE);
    memset(&ss, 0, sizeof ss);
    ss.hlsl = src;
    ss.len = strlen(src);
    ss.name = "ps_stages_d3d9";
    ss.entry = "main";
    ss.target = "ps_3_0";
    {
        long long started = d3d8_compile_clock();
        ps = rhi_shader_create(RHI_STAGE_PIXEL, &ss, err, sizeof err);
        d3d8_compile_note(1, started);
    }
    if (!ps)
        fprintf(stderr, "D3D8: D3D9 texture-stage PS compile failed: %s\n--- source ---\n%s\n",
                err[0] ? err : "unknown", src);
    free(src);
    if (!ps)
        return NULL;

    if (g_d9_ps_count >= D9_PS_CACHE_SIZE) {
        rhi_shader_destroy(g_d9_ps_cache[0].ps);
        memmove(&g_d9_ps_cache[0], &g_d9_ps_cache[1],
                (D9_PS_CACHE_SIZE - 1) * sizeof(D9PsCacheEntry));
        g_d9_ps_count--;
    }
    g_d9_ps_cache[g_d9_ps_count].key = *key;
    g_d9_ps_cache[g_d9_ps_count].ps = ps;
    g_d9_ps_count++;
    return ps;
}

static void d9_program_ps(void)
{
    const DWORD *rs = d3d8_GetRenderStates();
    D9PSConstants *pc;
    D9PsKey key;
    RhiShader *ps;
    UINT i;

    memset(&key, 0, sizeof key);
    for (i = 0; i < 4; i++) {
        IDirect3DBaseTexture8 *tex = d3d8_GetStageTexture(i);

        d9_stage_ops(i, tex, &key.op[i]);
        if (key.op[i].cop == D3DTOP_DISABLE) {
            /* the key names only the stages that run */
            memset(&key.op[i], 0, sizeof key.op[i]);
            key.op[i].cop = D3DTOP_DISABLE;
            break;
        }
        if (tex) {
            D3DRESOURCETYPE type = IDirect3DBaseTexture8_GetType(tex);
            key.dim[i] = type == D3DRTYPE_CUBETEXTURE ? 2 : type == D3DRTYPE_VOLUMETEXTURE ? 3 : 1;
        }
    }
    if (rs) {
        key.specular = rs[D3DRS_SPECULARENABLE] != 0;
        if (rs[D3DRS_FOGENABLE]) {
            switch (rs[D3DRS_FOGTABLEMODE]) {
            case D3DFOG_NONE:   key.fog = D9_FOG_PROGRAM; break;
            case D3DFOG_EXP:    key.fog = D9_FOG_EXP; break;
            case D3DFOG_EXP2:   key.fog = D9_FOG_EXP2; break;
            default:            key.fog = D9_FOG_LINEAR; break;
            }
            key.range_fog = key.fog != D9_FOG_PROGRAM && rs[D3DRS_RANGEFOGENABLE];
        }
    }
    ps = d9_ps_get(&key);
    if (!ps)
        return;
    rhi_set_shader(RHI_STAGE_PIXEL, ps);

    pc = (D9PSConstants *)rhi_buffer_map(g_ps_cb, RHI_MAP_WRITE_DISCARD);
    if (pc) {
        memset(pc, 0, sizeof *pc);
        if (rs) {
            DWORD tf = rs[D3DRS_TEXTUREFACTOR], fc = rs[D3DRS_FOGCOLOR];
            pc->tex_factor[0] = ((tf >> 16) & 0xFF) / 255.0f;
            pc->tex_factor[1] = ((tf >>  8) & 0xFF) / 255.0f;
            pc->tex_factor[2] = ((tf >>  0) & 0xFF) / 255.0f;
            pc->tex_factor[3] = ((tf >> 24) & 0xFF) / 255.0f;
            pc->fog_color[0] = ((fc >> 16) & 0xFF) / 255.0f;
            pc->fog_color[1] = ((fc >>  8) & 0xFF) / 255.0f;
            pc->fog_color[2] = ((fc >>  0) & 0xFF) / 255.0f;
            pc->fog_color[3] = 1.0f;
            memcpy(&pc->fog_params[0], &rs[D3DRS_FOGSTART], sizeof(float));
            memcpy(&pc->fog_params[1], &rs[D3DRS_FOGEND], sizeof(float));
            memcpy(&pc->fog_params[2], &rs[D3DRS_FOGDENSITY], sizeof(float));
        }
        for (i = 0; i < 4; i++) {
            float sx = 1.0f, sy = 1.0f;
            d9_texel_scale(d3d8_GetStageTexture(i), &sx, &sy);
            pc->tex_scale[i][0] = sx;
            pc->tex_scale[i][1] = sy;
        }
        rhi_buffer_unmap(g_ps_cb);
    }
    rhi_set_uniform_buffers(RHI_STAGE_PIXEL, 0, 1, &g_ps_cb);
}

static void d9_shutdown(void)
{
    int i;

    for (i = 0; i < g_d9_layout_count; i++)
        rhi_vertex_layout_destroy(g_d9_layout_cache[i].layout);
    g_d9_layout_count = 0;
    for (i = 0; i < g_d9_ps_count; i++)
        rhi_shader_destroy(g_d9_ps_cache[i].ps);
    g_d9_ps_count = 0;
}

static void d9_prepare_draw(DWORD handle)
{
    if (d3d8_vsh_prepare_draw(handle)) {
        g_draw_is_program = 1;
        d3d8_SetTwoDSqueeze(!d3d8_vsh_bound_uses_projection()
                            || d3d8_vsh_bound_projection_is_ortho());
        d9_title_states(0, 1);
        d9_program_ps();
    } else {
        d9_ff_prepare_draw(handle);
        d3d8_SetTwoDSqueeze(d3d8_fvf_transformed(handle));
    }
    rhi_d3d9_title_draw();
}

void d3d8_shaders_prepare_draw(DWORD handle)
{
    RhiShader *ps;
    void *mapped;
    const DWORD *rs;

    if (!rhi_device_ready()) return;
    g_draw_is_program = 0;
    if (rhi_d3d9_active()) {
        d9_prepare_draw(handle);
        return;
    }
    if (d3d8_vsh_prepare_draw(handle)) {
        g_draw_is_program = 1;
        /* A program that never reads the projection's first column is
         * placing vertices in screen coordinates it worked out itself; one
         * that reads it but with an orthographic matrix in it is doing the
         * same through the matrix. */
        d3d8_SetTwoDSqueeze(!d3d8_vsh_bound_uses_projection()
                            || d3d8_vsh_bound_projection_is_ortho());
    } else {
        ff_vs_prepare_draw(handle);
        /* The fixed-function equivalent: pre-transformed vertices arrive
         * already in screen pixels. Judged by the handle, which is the FVF
         * here, the same one ff_vs_prepare_draw just laid the vertices out
         * by. d3d8_GetCurrentFVF() is the bound vertex buffer's, and a
         * DrawPrimitiveUP binds none: it answered 0 for every user-pointer
         * draw, so OutRun 2's HUD (XYZRHW, 0x144, all UP) was never a
         * screen-space draw to this test and was left unsqueezed in
         * widescreen -- a third wider on screen while the race was right. */
        d3d8_SetTwoDSqueeze(d3d8_fvf_transformed(handle));
    }

    /* Pixel state is independent of whether the vertex shader is programmable.
     * A combiner shader, when one is active, binds its own pixel shader and
     * constants and the fixed-function ones below would only be replaced;
     * they are still the fallback when the combiner shader cannot be built. */
    if (d3d8_combiners_prepare_draw())
        return;
    if (!g_ps_cb) return;
    ps = ff_ps_get_shader(ff_ps_compute_signature());
    if (!ps) return;
    rhi_set_shader(RHI_STAGE_PIXEL, ps);
    rs = d3d8_GetRenderStates();

    /* ---- PS Constant Buffer ---- */
    mapped = rhi_buffer_map(g_ps_cb, RHI_MAP_WRITE_DISCARD);
    if (mapped) {
        PSConstants *pc = (PSConstants *)mapped;
        UINT stage;
        memset(pc, 0, sizeof(*pc));

        /* Texture factor */
        if (rs) {
            DWORD tf = rs[D3DRS_TEXTUREFACTOR];
            pc->tex_factor[0] = ((tf >> 16) & 0xFF) / 255.0f;
            pc->tex_factor[1] = ((tf >>  8) & 0xFF) / 255.0f;
            pc->tex_factor[2] = ((tf >>  0) & 0xFF) / 255.0f;
            pc->tex_factor[3] = ((tf >> 24) & 0xFF) / 255.0f;
        }

        /* Fog color */
        if (rs && rs[D3DRS_FOGENABLE]) {
            DWORD fc = rs[D3DRS_FOGCOLOR];
            pc->fog_color[0] = ((fc >> 16) & 0xFF) / 255.0f;
            pc->fog_color[1] = ((fc >>  8) & 0xFF) / 255.0f;
            pc->fog_color[2] = ((fc >>  0) & 0xFF) / 255.0f;
            pc->fog_color[3] = 1.0f;

            if (rs[D3DRS_FOGTABLEMODE] != D3DFOG_NONE) {
                /* Table fog: compute the factor per-pixel from view-space depth */
                float fog_start, fog_end, fog_density;
                memcpy(&fog_start, &rs[D3DRS_FOGSTART], sizeof(float));
                memcpy(&fog_end, &rs[D3DRS_FOGEND], sizeof(float));
                memcpy(&fog_density, &rs[D3DRS_FOGDENSITY], sizeof(float));
                pc->fog_params[0] = fog_start;
                pc->fog_params[1] = fog_end;
                pc->fog_params[2] = fog_density;
                pc->fog_params[3] = (float)rs[D3DRS_FOGTABLEMODE];
                pc->ps_flags |= 8; /* table fog */
                if (rs[D3DRS_RANGEFOGENABLE]) pc->ps_flags |= 16; /* range fog */
            } else if (g_draw_is_program) {
                /* The program's oFog is a fog coordinate, mapped linearly
                 * through FOGSTART..FOGEND in the pixel shader. */
                float fog_start, fog_end;
                memcpy(&fog_start, &rs[D3DRS_FOGSTART], sizeof(float));
                memcpy(&fog_end, &rs[D3DRS_FOGEND], sizeof(float));
                pc->fog_params[0] = fog_start;
                pc->fog_params[1] = fog_end;
                pc->ps_flags |= 32; /* program fog coordinate */
            } else {
                pc->ps_flags |= 2; /* vertex fog (factor computed in the VS) */
            }
        }

        /* Alpha test */
        if (rs && rs[D3DRS_ALPHATESTENABLE]) {
            pc->alpha_ref = rs[D3DRS_ALPHAREF] / 255.0f;
            pc->alpha_func = rs[D3DRS_ALPHAFUNC];
            pc->ps_flags |= 1;
        }

        /* Specular add */
        if (rs && rs[D3DRS_SPECULARENABLE])
            pc->ps_flags |= 4;

        /* Per-stage texture state */
        for (stage = 0; stage < 4; stage++) {
            IDirect3DBaseTexture8 *tex = d3d8_GetStageTexture(stage);
            D3DFORMAT format = d3d8_base_format(tex);
            const DWORD *tss = d3d8_GetTSS(stage);
            UINT w = 0, h = 0;
            pc->alpha_only[stage] = format == D3DFMT_A8 || format == D3DFMT_LIN_A8;
            /* Linear textures are addressed in texels on the NV2A; see
             * NV2APSConstants.tex_scale in d3d8_combiners.h. */
            pc->tex_scale[stage][0] = pc->tex_scale[stage][1] = 1.0f;
            pc->tex_scale[stage][2] = pc->tex_scale[stage][3] = 0.0f;
            if (tex && d3d8_format_is_linear(format) && d3d8_base_size(tex, &w, &h) && w && h) {
                pc->tex_scale[stage][0] = 1.0f / (float)w;
                pc->tex_scale[stage][1] = 1.0f / (float)h;
            }
            if (!tss) {
                pc->stage_color[stage][0] = D3DTOP_DISABLE;
                continue;
            }

            DWORD colorop = tss[D3DTSS_COLOROP];
            if (colorop == 0) colorop = (stage == 0) ? D3DTOP_MODULATE : D3DTOP_DISABLE;

            pc->stage_color[stage][0] = colorop;
            /* Arguments are taken as set: 0 is D3DTA_DIFFUSE, and the device
             * starts with the D3D8 defaults (d3d8_init_default_states). */
            pc->stage_color[stage][1] = tss[D3DTSS_COLORARG1];
            pc->stage_color[stage][2] = tss[D3DTSS_COLORARG2];
            pc->stage_color[stage][3] = tss[D3DTSS_ALPHAOP] ? tss[D3DTSS_ALPHAOP] :
                                         (stage == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE);

            pc->stage_alpha[stage][0] = tss[D3DTSS_ALPHAARG1];
            pc->stage_alpha[stage][1] = tss[D3DTSS_ALPHAARG2];
            /* A stage with no texture that reads D3DTA_TEXTURE does not take
             * part. The host device binds white there (so register combiners
             * read what they always did), and as a fixed-function argument
             * that white replaced everything before it: BLiNX draws its
             * title movie with stage 1 still set to MODULATE(TEXTURE,
             * DIFFUSE) over no texture, and the movie came out pure white,
             * where on the console it plays. Disabling the stage, as D3D's
             * DISABLE does, leaves stage 0's result. Inferred from that
             * title's picture, not measured on hardware. */
            if (!tex && (ff_op_reads_texture(colorop, tss[D3DTSS_COLORARG1],
                                             tss[D3DTSS_COLORARG2]) ||
                         ff_op_reads_texture(pc->stage_color[stage][3],
                                             tss[D3DTSS_ALPHAARG1],
                                             tss[D3DTSS_ALPHAARG2])))
                pc->stage_color[stage][0] = D3DTOP_DISABLE;
        }

        rhi_buffer_unmap(g_ps_cb);
    }

    rhi_set_uniform_buffers(RHI_STAGE_PIXEL, 0, 1, &g_ps_cb);
}
