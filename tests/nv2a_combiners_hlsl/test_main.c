/*
 * nv2a_combiners_hlsl -- compile the HLSL the register combiner translator
 * generates, for many combiner configurations.
 *
 * d3d8_combiners_generate_hlsl() writes a pixel shader from an
 * NV2ACombinerState, and the state comes from whatever a title wrote into its
 * D3DRS_PS* render states. A configuration that makes the generator emit
 * something a compiler refuses shows up only when a title reaches it, as a
 * draw that silently falls back. DXC is also stricter than D3DCompile
 * (docs/technical/vulkan-backend.md, section 4.3), so a source D3D11 takes
 * can still fail under Vulkan.
 *
 * So this builds states the way the device does -- d3d8_combiners_parse_token
 * over a render state array -- from a fixed pseudo-random sequence: every
 * stage count, every input register, mapping and alpha replicate, every
 * output destination with every dot, mux and scale flag, the final
 * combiner's inputs and flags, the combiner-count flags, both texture-mode
 * encodings (the 4-bit sampler kinds, and the title's own PS_TEXTUREMODES
 * 0x00-0x12 with dot mappings and input stages), and shadow-map stages.
 * Only encodings the hardware accepts are made: a general stage cannot read
 * or write V1R0_SUM or EF_PROD (final combiner only), registers 6 and 7 do
 * not exist, and a shadow-map stage has a texture. Each configuration is
 * built twice, without and with the alpha test (NV2ACombinerState.alpha_test,
 * the only variant that may contain a discard), and checked to contain a
 * discard exactly when the alpha test is on. Each is generated and compiled the way the
 * runtime compiles a combiner shader: D3DCompile ps_5_0 on Windows, DXC to
 * SPIR-V elsewhere (rhi_vulkan_dxc.cpp). Every failure prints its seed, so
 * it can be reproduced alone: nv2a_combiners_hlsl_test <seed>.
 *
 * Random configurations are not what titles write, and that is the point:
 * any legal one may be, and the generator must never emit invalid HLSL.
 * No device, window or game files. Without a compiler it is skipped (77).
 */
#include "d3d8_internal.h"
#include "d3d8_combiners.h"
#if defined(_WIN32)
#include <d3d11.h>
#include <d3dcompiler.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The combiner sources link against the device layer for state this test
 * never touches. The same stubs as tests/d3d8_smoke/test_final_default.c. */
static DWORD states[512], stages[4][64];
static const D3DMATRIX identity = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
#if defined(_WIN32)
ID3D11Device *d3d8_GetD3D11Device(void) { return NULL; }
ID3D11DeviceContext *d3d8_GetD3D11Context(void) { return NULL; }
ID3D11RenderTargetView *d3d8_GetDefaultRTV(void) { return NULL; }
#else
int rhi_vk_dxc_compile(uint32_t stage, const RhiShaderSource *src,
                       uint32_t **spirv, size_t *spirv_bytes, char *err, size_t err_len);
#endif
IDirect3DDevice8 *xbox_GetD3DDevice(void) { return NULL; }
const DWORD *d3d8_GetPalette(DWORD stage) { (void)stage; return states; }
const DWORD *d3d8_GetRenderStates(void) { return states; }
const DWORD *d3d8_GetTSS(DWORD stage) { return stages[stage]; }
IDirect3DBaseTexture8 *d3d8_GetStageTexture(DWORD stage) { (void)stage; return NULL; }
UINT d3d8_GetGuestWidth(void) { return 1; }
UINT d3d8_GetGuestHeight(void) { return 1; }
void d3d8_GetTargetGuestSize(UINT *w, UINT *h) { *w = d3d8_GetGuestWidth(); *h = d3d8_GetGuestHeight(); }
UINT d3d8_GetBackbufferWidth(void) { return 1; }
UINT d3d8_GetBackbufferHeight(void) { return 1; }
const D3DMATRIX *d3d8_GetTransform(D3DTRANSFORMSTATETYPE type) { (void)type; return &identity; }
const D3DLIGHT8 *d3d8_GetLight(DWORD index) { (void)index; return NULL; }
BOOL d3d8_GetLightEnable(DWORD index) { (void)index; return FALSE; }
const D3DMATERIAL8 *d3d8_GetMaterial(void) { return NULL; }
UINT d3d8_GetNumLights(void) { return 0; }
BOOL d3d8_vsh_prepare_draw(DWORD handle) { (void)handle; return FALSE; }
int  d3d8_vsh_bound_uses_projection(void) { return 1; }
int  d3d8_vsh_bound_projection_is_ortho(void) { return 0; }
float d3d8_vsh_hor_plus_for_draw(void) { return 1.0f; }
DWORD d3d8_GetCurrentFVF(void) { return 0; }
void d3d8_SetTwoDSqueeze(BOOL on) { (void)on; }

#define CONFIGS 400

static uint32_t g_rng;

static uint32_t next(void)
{
    /* xorshift32: the same sequence on every platform and compiler. */
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

/* A register a general combiner stage can read: not 6 or 7 (there are
 * none), not V1R0_SUM or EF_PROD (the final combiner's own). */
static DWORD general_reg(void)
{
    static const DWORD regs[] = { 0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13 };
    return regs[next() % (sizeof regs / sizeof regs[0])];
}

/* Four input bytes: register, alpha replicate (bit 4), mapping (bits 5-7).
 * Also the final combiner's E, F and G, which cannot read the sums made
 * from them either. */
static DWORD inputs(void)
{
    DWORD w = 0;
    int k;

    for (k = 0; k < 4; k++) {
        DWORD reg = general_reg();
        w |= (reg | (next() & 0x10) | (next() & 0xE0)) << (8 * k);
    }
    return w;
}

/* An output word: AB, CD and sum destinations (a register a stage can
 * write, or 0 to discard) and the dot, mux, blue-to-alpha and scale bits. */
static DWORD outputs(void)
{
    static const DWORD dst[] = { 0, 0, 0, 4, 5, 8, 9, 10, 11, 12, 12, 13 };
    const DWORD n = sizeof dst / sizeof dst[0];

    return dst[next() % n] | (dst[next() % n] << 4) | (dst[next() % n] << 8) |
           (next() & 0x0007F000u);
}

static void build(uint32_t seed, NV2ACombinerState *state)
{
    DWORD rs[512];
    DWORD token;
    int i;

    g_rng = seed * 2654435761u + 0x9E3779B9u;
    if (!g_rng)
        g_rng = 1;
    memset(rs, 0, sizeof rs);
    for (i = 0; i < 8; i++) {
        rs[D3DRS_PSRGBINPUTS0 + i] = inputs();
        rs[D3DRS_PSALPHAINPUTS0 + i] = inputs();
        rs[D3DRS_PSRGBOUTPUTS0 + i] = outputs();
        rs[D3DRS_PSALPHAOUTPUTS0 + i] = outputs();
        rs[D3DRS_PSCONSTANT0_0 + i] = next();
        rs[D3DRS_PSCONSTANT1_0 + i] = next();
    }
    /* A-D may read anything; E, F and G not the sums made from them. The
     * EFG word's low byte is the final combiner's flags. */
    {
        DWORD abcd = 0;
        int k;
        for (k = 0; k < 4; k++) {
            DWORD reg = next() % 16;
            if (reg == 6 || reg == 7)
                reg = 0;
            abcd |= (reg | (next() & 0x10) | (next() & 0xE0)) << (8 * k);
        }
        rs[D3DRS_PSFINALCOMBINERINPUTSABCD] = abcd;
    }
    rs[D3DRS_PSFINALCOMBINERINPUTSEFG] = (inputs() & 0xFFFFFF00u) | (next() & 0xE0u);
    rs[D3DRS_PSFINALCOMBINERCONSTANT0] = next();
    rs[D3DRS_PSFINALCOMBINERCONSTANT1] = next();
    /* Stage count, and the MUX_MSB / UNIQUE_C0 / UNIQUE_C1 flags. */
    rs[D3DRS_PSCOMBINERCOUNT] = (1 + next() % 8) | (next() & 0x00011100u);
    if (next() & 1) {
        /* The title's own texture modes, 5 bits a stage, sometimes past
         * the last valid one. */
        DWORD tm = D3D8_PSTEXTUREMODES_XBOX;
        for (i = 0; i < 4; i++)
            tm |= (DWORD)(next() % 0x16) << (5 * i);
        rs[D3DRS_PSTEXTUREMODES] = tm;
        rs[D3DRS_PSDOTMAPPING] = next() & 0xFFFu;
        rs[D3DRS_PSINPUTTEXTURE] = next() & 0x00FF0000u;
        token = 1 + next() % 8;                     /* modes from the state */
    } else {
        /* The four sampler kinds, through the token. */
        token = (1 + next() % 8) | ((next() % 4) << 8) | ((next() % 4) << 12) |
                ((next() % 4) << 16) | ((next() % 4) << 20) | (next() & 0xFF000000u);
    }
    memcpy(states, rs, sizeof states);
    d3d8_combiners_parse_token(token, rs, state);
    for (i = 0; i < NV2A_MAX_TEXTURES; i++)
        state->shadow[i] = state->tex_mode[i] == NV2A_TEXMODE_2D && (next() % 5) == 0;
}

/* 1 compiled, 0 did not, -1 no compiler on this machine. */
static int compile(const char *hlsl, int len, char *err, size_t err_len)
{
#if defined(_WIN32)
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = D3DCompile(hlsl, (SIZE_T)len, "combiner", NULL, NULL, "main", "ps_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    snprintf(err, err_len, "%s",
             errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "(no message)");
    if (code) ID3D10Blob_Release(code);
    if (errors) ID3D10Blob_Release(errors);
    return SUCCEEDED(hr);
#else
    RhiShaderSource src;
    uint32_t *spirv = NULL;
    size_t bytes = 0;
    int ok;

    memset(&src, 0, sizeof src);
    src.hlsl = hlsl;
    src.len = (size_t)len;
    src.name = "ps_combiner";
    src.entry = "main";
    src.target = "ps_5_0";
    src.optimize = 1;
    ok = rhi_vk_dxc_compile(RHI_STAGE_PIXEL, &src, &spirv, &bytes, err, err_len);
    if (!ok && strstr(err, "DXC is not available"))
        return -1;
    ok = ok && bytes >= 20 && spirv && spirv[0] == 0x07230203u;
    free(spirv);
    return ok;
#endif
}

int main(int argc, char **argv)
{
    static char hlsl[1 << 18];
    static char err[16384];
    uint32_t first = 0, count = CONFIGS, seed;
    int failures = 0, checks = 0;

    if (argc > 1) {
        first = (uint32_t)strtoul(argv[1], NULL, 0);
        count = 1;
    }
    for (seed = first; seed < first + count; seed++) {
        NV2ACombinerState state;
        int len, r, at;

        build(seed, &state);
        for (at = 0; at < 2; at++) {
            state.alpha_test = (BYTE)at;
            len = d3d8_combiners_generate_hlsl(&state, hlsl, (int)sizeof hlsl);
            checks++;
            if (len <= 0) {
                printf("FAIL seed %u alpha test %d: the generator produced nothing\n", seed, at);
                failures++;
                continue;
            }
            /* The point of the variant: no discard unless the alpha test
             * is on, since a discard alone costs early depth. */
            if ((strstr(hlsl, "discard") != NULL) != at) {
                failures++;
                printf("FAIL seed %u alpha test %d: the source %s a discard\n", seed, at,
                       at ? "lacks" : "contains");
                continue;
            }
            r = compile(hlsl, len, err, sizeof err);
            if (r < 0) {
                printf("nv2a_combiners_hlsl: skipped, no shader compiler on this machine\n");
                return 77;
            }
            if (!r) {
                failures++;
                printf("FAIL seed %u alpha test %d: the generated HLSL does not compile\n%s\n",
                       seed, at, err);
                if (count == 1 || failures == 1)
                    printf("---- source ----\n%s\n", hlsl);
            }
        }
    }
    printf("nv2a_combiners_hlsl: %d shaders (%u configurations, each without and with the "
           "alpha test), %d failed\n", checks, (unsigned)count, failures);
    return failures ? 1 : 0;
}
