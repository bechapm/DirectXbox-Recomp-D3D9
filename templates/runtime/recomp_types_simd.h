/**
 * Xbox Static Recompilation - SSE, XMM and MMX runtime model
 *
 * Split out of recomp_types.h, which includes it unless RECOMP_NO_SIMD is
 * defined (see the note at the end of that header). The content is unchanged
 * from when it lived there. tools/recomp copies this file into gen/ beside
 * recomp_types.h, and reads it to decide which chunks need it, so a helper
 * added here is picked up by that check without touching the lifter.
 */

#ifndef RECOMP_TYPES_SIMD_H
#define RECOMP_TYPES_SIMD_H

#include "recomp_types.h"

/* SSE float-to-int conversions, as x86 does them.
 *
 * cvttss2si/cvttsd2si truncate; cvtss2si/cvtsd2si round under MXCSR.RC,
 * which the guest never changes here (ldmxcsr is not lifted), so to nearest.
 * A NaN or out-of-range input gives 0x80000000, the "integer indefinite".
 * These were plain (int32_t) casts: right for the truncating forms on an x86
 * host, where the cast compiles to cvttss2si; wrong for the rounding forms on
 * every host (they truncated); and on AArch64 a cast saturates instead of
 * giving 0x80000000. x86 hosts use the instructions themselves. */
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
#include <emmintrin.h>
static RECOMP_FORCEINLINE int32_t recomp_cvtss2si(float v)   { return _mm_cvtss_si32(_mm_set_ss(v)); }
static RECOMP_FORCEINLINE int32_t recomp_cvttss2si(float v)  { return _mm_cvttss_si32(_mm_set_ss(v)); }
static RECOMP_FORCEINLINE int32_t recomp_cvtsd2si(double v)  { return _mm_cvtsd_si32(_mm_set_sd(v)); }
static RECOMP_FORCEINLINE int32_t recomp_cvttsd2si(double v) { return _mm_cvttsd_si32(_mm_set_sd(v)); }
#else
static inline int32_t recomp_f2i32_indef(double rounded)
{
    if (!(rounded >= -2147483648.0 && rounded <= 2147483647.0))
        return INT32_MIN;                  /* integer indefinite, NaN included */
    return (int32_t)rounded;
}
/* nearbyint rounds in the host's mode, which nothing here changes: nearest,
 * as MXCSR's default. */
static inline int32_t recomp_cvtss2si(float v)   { return recomp_f2i32_indef(nearbyint((double)v)); }
static inline int32_t recomp_cvttss2si(float v)  { return recomp_f2i32_indef(trunc((double)v)); }
static inline int32_t recomp_cvtsd2si(double v)  { return recomp_f2i32_indef(nearbyint(v)); }
static inline int32_t recomp_cvttsd2si(double v) { return recomp_f2i32_indef(trunc(v)); }
#endif

/* ================================================================
 * SSE / XMM register state
 *
 * XMM is 128 bits of architectural state, not a scalar float. Modelling
 * it as a `float` made movaps/movups transfer 4 of 16 bytes and silently
 * drop the upper three lanes, and left the packed arithmetic with no
 * representation at all.
 *
 * The registers are global for the same reason the volatile GPRs are:
 * one guest routine can lift to several C functions, so a value produced
 * in one body and read in the next has to outlive the body that wrote it.
 * A function-local declaration would also shadow these, and the local
 * starts zeroed -- a returned float would silently read as 0.0.
 *
 * The helpers are lane-wise C rather than host intrinsics: the guest
 * semantics stay explicit (MINPS returning src on unordered, CMPNEQPS
 * being the unordered form) and the header stays portable.
 * ================================================================ */

#ifndef RECOMP_XMM_DEFINED
#define RECOMP_XMM_DEFINED
typedef union RecompXmm {
    float    f[4];
    double   d[2];
    uint32_t u[4];
    int32_t  i[4];
    uint64_t q[2];
} RecompXmm;
#endif

extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;

/* -- construction -- */

static inline RecompXmm XMM_ZERO(void) {
    RecompXmm r; r.q[0] = 0; r.q[1] = 0; return r;
}

/** movss from memory: lane 0 set, upper lanes zeroed. */
static inline RecompXmm XMM_SCALAR(float v) {
    RecompXmm r = XMM_ZERO(); r.f[0] = v; return r;
}

/** movsd from memory: low double set, high double zeroed. */
static inline RecompXmm XMM_SCALAR_DOUBLE(double v) {
    RecompXmm r = XMM_ZERO(); r.d[0] = v; return r;
}

/** movd: 32 raw bits into lane 0, upper lanes zeroed. */
static inline RecompXmm XMM_SCALAR_BITS(uint32_t bits) {
    RecompXmm r = XMM_ZERO(); r.u[0] = bits; return r;
}

/* -- guest memory --
 * Addresses are guest VAs, so they go through MEM32 like every other
 * access. Done lane-wise, which is also unaligned-safe for movups. */

static inline RecompXmm XMM_MEM(uint32_t addr) {
    RecompXmm r;
    r.u[0] = MEM32(addr);      r.u[1] = MEM32(addr + 4);
    r.u[2] = MEM32(addr + 8);  r.u[3] = MEM32(addr + 12);
    return r;
}

static inline void XMM_STORE(uint32_t addr, RecompXmm v) {
    MEM32(addr)      = v.u[0]; MEM32(addr + 4)  = v.u[1];
    MEM32(addr + 8)  = v.u[2]; MEM32(addr + 12) = v.u[3];
}

/* movlps/movhps move 8 bytes into or out of one half, leaving the
 * other half alone. */
#define XMM_LOAD_LOW(dst, addr)   recomp_xmm_load_half(&(dst), (addr), 0)
#define XMM_LOAD_HIGH(dst, addr)  recomp_xmm_load_half(&(dst), (addr), 1)
#define XMM_STORE_LOW(addr, src)  recomp_xmm_store_half((addr), (src), 0)
#define XMM_STORE_HIGH(addr, src) recomp_xmm_store_half((addr), (src), 1)

static inline void recomp_xmm_load_half(RecompXmm *dst, uint32_t addr,
                                        int high) {
    dst->u[high * 2]     = MEM32(addr);
    dst->u[high * 2 + 1] = MEM32(addr + 4);
}

static inline void recomp_xmm_store_half(uint32_t addr, RecompXmm src,
                                         int high) {
    MEM32(addr)     = src.u[high * 2];
    MEM32(addr + 4) = src.u[high * 2 + 1];
}

/** movlhps: dst high = src low. */
static inline RecompXmm XMM_MOVE_LOW_TO_HIGH(RecompXmm a, RecompXmm b) {
    RecompXmm r; r.q[0] = a.q[0]; r.q[1] = b.q[0]; return r;
}

/** movhlps: dst low = src high. */
static inline RecompXmm XMM_MOVE_HIGH_TO_LOW(RecompXmm a, RecompXmm b) {
    RecompXmm r; r.q[0] = b.q[1]; r.q[1] = a.q[1]; return r;
}

/* -- packed arithmetic -- */

#define RECOMP_XMM_LANEWISE(name, expr)                                   \
    static inline RecompXmm name(RecompXmm a, RecompXmm b) {              \
        RecompXmm r; int i;                                               \
        for (i = 0; i < 4; ++i) { (void)a; (void)b; r.f[i] = (expr); }    \
        return r;                                                         \
    }

RECOMP_XMM_LANEWISE(XMM_ADD, a.f[i] + b.f[i])
RECOMP_XMM_LANEWISE(XMM_SUB, a.f[i] - b.f[i])
RECOMP_XMM_LANEWISE(XMM_MUL, a.f[i] * b.f[i])
RECOMP_XMM_LANEWISE(XMM_DIV, a.f[i] / b.f[i])
/* MINPS/MAXPS return the second operand when the lanes are unordered or
 * equal -- that is the hardware's tie-break, not a C fmin/fmax. */
RECOMP_XMM_LANEWISE(XMM_MIN, (a.f[i] < b.f[i]) ? a.f[i] : b.f[i])
RECOMP_XMM_LANEWISE(XMM_MAX, (a.f[i] > b.f[i]) ? a.f[i] : b.f[i])

#define RECOMP_XMM_BITWISE(name, expr)                                    \
    static inline RecompXmm name(RecompXmm a, RecompXmm b) {              \
        RecompXmm r; int i;                                               \
        for (i = 0; i < 4; ++i) { (void)a; (void)b; r.u[i] = (expr); }    \
        return r;                                                         \
    }

RECOMP_XMM_BITWISE(XMM_AND,  a.u[i] & b.u[i])
RECOMP_XMM_BITWISE(XMM_OR,   a.u[i] | b.u[i])
RECOMP_XMM_BITWISE(XMM_XOR,  a.u[i] ^ b.u[i])
/* ANDNPS is ~dst & src, not dst & ~src. */
RECOMP_XMM_BITWISE(XMM_ANDN, (~a.u[i]) & b.u[i])

/* Compares produce an all-ones or all-zero mask per lane. EQ/LT/LE are
 * the ordered forms (false when either lane is NaN); NEQ is the
 * unordered form, so it is true when a lane is NaN. */
RECOMP_XMM_BITWISE(XMM_CMP_EQ,  (a.f[i] == b.f[i]) ? 0xFFFFFFFFu : 0u)
RECOMP_XMM_BITWISE(XMM_CMP_LT,  (a.f[i] <  b.f[i]) ? 0xFFFFFFFFu : 0u)
RECOMP_XMM_BITWISE(XMM_CMP_LE,  (a.f[i] <= b.f[i]) ? 0xFFFFFFFFu : 0u)
RECOMP_XMM_BITWISE(XMM_CMP_NEQ, (a.f[i] == b.f[i]) ? 0u : 0xFFFFFFFFu)

/* The full SSE compare predicate set, by CMPPS/CMPSS immediate:
 * 0 EQ, 1 LT, 2 LE, 3 UNORD, 4 NEQ, 5 NLT, 6 NLE, 7 ORD. The N forms are
 * the negations, so they are true when either side is NaN. */
static inline int recomp_cmp_pred(float a, float b, int p) {
    switch (p & 7) {
    case 0:  return a == b;
    case 1:  return a < b;
    case 2:  return a <= b;
    case 3:  return a != a || b != b;
    case 4:  return !(a == b);
    case 5:  return !(a < b);
    case 6:  return !(a <= b);
    default: return !(a != a || b != b);
    }
}
static inline RecompXmm XMM_CMP_PRED(RecompXmm a, RecompXmm b, int p) {
    RecompXmm r; int i;
    for (i = 0; i < 4; ++i)
        r.u[i] = recomp_cmp_pred(a.f[i], b.f[i], p) ? 0xFFFFFFFFu : 0u;
    return r;
}
/* Packed unary ops; the first argument is unused, as for the binary forms. */
RECOMP_XMM_LANEWISE(XMM_SQRT,  sqrtf(b.f[i]))
RECOMP_XMM_LANEWISE(XMM_RSQRT, 1.0f / sqrtf(b.f[i]))
RECOMP_XMM_LANEWISE(XMM_RCP,   1.0f / b.f[i])

/** movmskps: the four lane sign bits, packed into the low nibble. */
static inline uint32_t XMM_MOVEMASK(RecompXmm a) {
    return ((a.u[0] >> 31) & 1u) | (((a.u[1] >> 31) & 1u) << 1)
         | (((a.u[2] >> 31) & 1u) << 2) | (((a.u[3] >> 31) & 1u) << 3);
}

/** shufps: lanes 0-1 selected out of `a`, lanes 2-3 out of `b`. */
static inline RecompXmm XMM_SHUFFLE(RecompXmm a, RecompXmm b, uint32_t imm) {
    RecompXmm r;
    r.u[0] = a.u[(imm >> 0) & 3u]; r.u[1] = a.u[(imm >> 2) & 3u];
    r.u[2] = b.u[(imm >> 4) & 3u]; r.u[3] = b.u[(imm >> 6) & 3u];
    return r;
}

/** unpcklps / unpckhps: interleave the low or high halves. */
static inline RecompXmm XMM_UNPACK_LOW(RecompXmm a, RecompXmm b) {
    RecompXmm r;
    r.u[0] = a.u[0]; r.u[1] = b.u[0]; r.u[2] = a.u[1]; r.u[3] = b.u[1];
    return r;
}

static inline RecompXmm XMM_UNPACK_HIGH(RecompXmm a, RecompXmm b) {
    RecompXmm r;
    r.u[0] = a.u[2]; r.u[1] = b.u[2]; r.u[2] = a.u[3]; r.u[3] = b.u[3];
    return r;
}

#ifdef RECOMP_GENERATED_CODE

/* ================================================================
 * MMX register file
 *
 * The Xbox is a Pentium III and every XDK codec leans on MMX: the WMV
 * decoder's IDCT and motion compensation are almost nothing else. Modelled the
 * same way as RecompXmm -- a union of lane views over one 64-bit register --
 * because that is what the instructions are: the same bits read as bytes,
 * words or dwords.
 *
 * mm0..mm7 alias the x87 stack on real hardware. Nothing here does, and
 * nothing needs to: a title that interleaves the two calls emms between, and
 * emms is a no-op for us. Modelling the aliasing would mean giving up the
 * separate x87 model that the FPU work depends on, to reproduce a hazard the
 * hardware exists to let software avoid.
 * ================================================================ */

#ifndef RECOMP_MMX_DEFINED
#define RECOMP_MMX_DEFINED
typedef union RecompMmx {
    int8_t   b[8];
    uint8_t  ub[8];
    int16_t  w[4];
    uint16_t uw[4];
    int32_t  d[2];
    uint32_t ud[2];
    uint64_t q;
} RecompMmx;
#endif

extern RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
extern RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;

static inline RecompMmx MMX_ZERO(void) { RecompMmx r; r.q = 0; return r; }

/* CVTPS2PI follows MXCSR; CVTTPS2PI truncates regardless of its rounding mode.
 * Use SSE scalar conversions to avoid touching the host x87/MMX register file.
 * Non-x86 hosts use their floating-point environment for rounding instead. */
static inline int32_t MMX_CVT_F2I(float v, int truncate)
{
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
    return truncate ? _mm_cvttss_si32(_mm_set_ss(v))
                    : _mm_cvtss_si32(_mm_set_ss(v));
#else
    double rounded = truncate ? trunc((double)v) : nearbyint((double)v);
    if (!(rounded >= -2147483648.0 && rounded <= 2147483647.0))
        return (int32_t)0x80000000u;       /* integer indefinite */
    return (int32_t)rounded;
#endif
}

static inline RecompMmx MMX_FROM_PS(float lo, float hi, int truncate)
{
    RecompMmx r;
    r.d[0] = MMX_CVT_F2I(lo, truncate);
    r.d[1] = MMX_CVT_F2I(hi, truncate);
    return r;
}

/** cvtpi2ps: two signed dwords in, two singles out, into the LOW half of the
 * destination -- lanes 2 and 3 keep whatever they held. That detail is the
 * whole instruction: code that builds a float4 from two of these relies on
 * the first one surviving the second. */
static inline RecompXmm XMM_FROM_PI(RecompXmm dst, RecompMmx src)
{
    dst.f[0] = (float)src.d[0];
    dst.f[1] = (float)src.d[1];
    return dst;
}

static inline RecompMmx MMX_MEM(uint32_t addr) {
    RecompMmx r;
    memcpy(&r, (const void *)XBOX_PTR(addr), 8);
    return r;
}

static inline void MMX_STORE(uint32_t addr, RecompMmx v) {
    memcpy((void *)XBOX_PTR(addr), &v, 8);
}

static inline RecompMmx MMX_FROM32(uint32_t v) {
    RecompMmx r; r.q = 0; r.ud[0] = v; return r;
}

/* -- saturation helpers ---------------------------------------- */
static inline int16_t recomp_sat_i16(int32_t v) {
    return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}
static inline int8_t recomp_sat_i8(int32_t v) {
    return (int8_t)(v > 127 ? 127 : (v < -128 ? -128 : v));
}
static inline uint8_t recomp_sat_u8(int32_t v) {
    return (uint8_t)(v > 255 ? 255 : (v < 0 ? 0 : v));
}

static inline uint16_t recomp_sat_u16(int32_t v) {
    return (uint16_t)(v > 65535 ? 65535 : (v < 0 ? 0 : v));
}

/* -- integer arithmetic, lane-wise, wrapping -------------------- */
#define RECOMP_MMX_BINOP(NAME, LANES, FIELD, EXPR)                      \
    static inline RecompMmx NAME(RecompMmx a, RecompMmx b) {            \
        RecompMmx r; int i;                                             \
        for (i = 0; i < (LANES); i++) { (void)b; r.FIELD[i] = (EXPR); } \
        return r;                                                       \
    }

RECOMP_MMX_BINOP(MMX_PADDB, 8, b, (int8_t)((uint8_t)a.b[i] + (uint8_t)b.b[i]))
RECOMP_MMX_BINOP(MMX_PADDW, 4, w, (int16_t)((uint16_t)a.w[i] + (uint16_t)b.w[i]))
RECOMP_MMX_BINOP(MMX_PADDD, 2, d, (int32_t)((uint32_t)a.d[i] + (uint32_t)b.d[i]))
RECOMP_MMX_BINOP(MMX_PSUBB, 8, b, (int8_t)((uint8_t)a.b[i] - (uint8_t)b.b[i]))
RECOMP_MMX_BINOP(MMX_PSUBW, 4, w, (int16_t)((uint16_t)a.w[i] - (uint16_t)b.w[i]))
RECOMP_MMX_BINOP(MMX_PSUBD, 2, d, (int32_t)((uint32_t)a.d[i] - (uint32_t)b.d[i]))
RECOMP_MMX_BINOP(MMX_PADDSB, 8, b, recomp_sat_i8((int32_t)a.b[i] + b.b[i]))
RECOMP_MMX_BINOP(MMX_PADDSW, 4, w, recomp_sat_i16((int32_t)a.w[i] + b.w[i]))
RECOMP_MMX_BINOP(MMX_PSUBSB, 8, b, recomp_sat_i8((int32_t)a.b[i] - b.b[i]))
RECOMP_MMX_BINOP(MMX_PSUBSW, 4, w, recomp_sat_i16((int32_t)a.w[i] - b.w[i]))
RECOMP_MMX_BINOP(MMX_PADDUSB, 8, ub,
                 recomp_sat_u8((int32_t)a.ub[i] + b.ub[i]))
RECOMP_MMX_BINOP(MMX_PSUBUSB, 8, ub,
                 recomp_sat_u8((int32_t)a.ub[i] - b.ub[i]))
RECOMP_MMX_BINOP(MMX_PADDUSW, 4, uw,
                 recomp_sat_u16((int32_t)a.uw[i] + b.uw[i]))
RECOMP_MMX_BINOP(MMX_PSUBUSW, 4, uw,
                 recomp_sat_u16((int32_t)a.uw[i] - b.uw[i]))
RECOMP_MMX_BINOP(MMX_PMULLW, 4, w, (int16_t)((int32_t)a.w[i] * b.w[i]))
RECOMP_MMX_BINOP(MMX_PMULHW, 4, w, (int16_t)(((int32_t)a.w[i] * b.w[i]) >> 16))
RECOMP_MMX_BINOP(MMX_PAVGB, 8, ub, (uint8_t)(((int32_t)a.ub[i] + b.ub[i] + 1) >> 1))
RECOMP_MMX_BINOP(MMX_PAVGW, 4, uw, (uint16_t)(((int32_t)a.uw[i] + b.uw[i] + 1) >> 1))
RECOMP_MMX_BINOP(MMX_PMINSW, 4, w, (a.w[i] < b.w[i] ? a.w[i] : b.w[i]))
RECOMP_MMX_BINOP(MMX_PMAXSW, 4, w, (a.w[i] > b.w[i] ? a.w[i] : b.w[i]))
RECOMP_MMX_BINOP(MMX_PCMPEQB, 8, b, (int8_t)(a.b[i] == b.b[i] ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPEQW, 4, w, (int16_t)(a.w[i] == b.w[i] ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPEQD, 2, d, (int32_t)(a.d[i] == b.d[i] ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPGTB, 8, b, (int8_t)(a.b[i] > b.b[i] ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPGTW, 4, w, (int16_t)(a.w[i] > b.w[i] ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPGTD, 2, d, (int32_t)(a.d[i] > b.d[i] ? -1 : 0))

/* pmaddwd: multiply signed words, add adjacent pairs into dwords. */
static inline RecompMmx MMX_PMADDWD(RecompMmx a, RecompMmx b) {
    RecompMmx r;
    r.d[0] = (int32_t)a.w[0] * b.w[0] + (int32_t)a.w[1] * b.w[1];
    r.d[1] = (int32_t)a.w[2] * b.w[2] + (int32_t)a.w[3] * b.w[3];
    return r;
}

/* -- bitwise ---------------------------------------------------- */
static inline RecompMmx MMX_PAND(RecompMmx a, RecompMmx b)  { RecompMmx r; r.q = a.q & b.q; return r; }
static inline RecompMmx MMX_PANDN(RecompMmx a, RecompMmx b) { RecompMmx r; r.q = ~a.q & b.q; return r; }
static inline RecompMmx MMX_POR(RecompMmx a, RecompMmx b)   { RecompMmx r; r.q = a.q | b.q; return r; }
static inline RecompMmx MMX_PXOR(RecompMmx a, RecompMmx b)  { RecompMmx r; r.q = a.q ^ b.q; return r; }

/* -- shifts -----------------------------------------------------
 * A count at or past the lane width gives zero for the logical shifts and a
 * full sign fill for the arithmetic ones. The count is the whole 64-bit
 * register, not one lane, and it is unsigned -- a negative-looking count is a
 * huge one, which saturates the same way.
 */
#define RECOMP_MMX_SHIFT(NAME, LANES, FIELD, WIDTH, OP)                 \
    static inline RecompMmx NAME(RecompMmx a, uint64_t cnt) {           \
        RecompMmx r; int i;                                             \
        for (i = 0; i < (LANES); i++)                                   \
            r.FIELD[i] = (cnt >= (WIDTH)) ? 0 : (OP);                   \
        return r;                                                       \
    }
RECOMP_MMX_SHIFT(MMX_PSLLW, 4, uw, 16, (uint16_t)(a.uw[i] << cnt))
RECOMP_MMX_SHIFT(MMX_PSRLW, 4, uw, 16, (uint16_t)(a.uw[i] >> cnt))
RECOMP_MMX_SHIFT(MMX_PSLLD, 2, ud, 32, (uint32_t)(a.ud[i] << cnt))
RECOMP_MMX_SHIFT(MMX_PSRLD, 2, ud, 32, (uint32_t)(a.ud[i] >> cnt))

static inline RecompMmx MMX_PSLLQ(RecompMmx a, uint64_t cnt) {
    RecompMmx r; r.q = (cnt >= 64) ? 0 : (a.q << cnt); return r;
}
static inline RecompMmx MMX_PSRLQ(RecompMmx a, uint64_t cnt) {
    RecompMmx r; r.q = (cnt >= 64) ? 0 : (a.q >> cnt); return r;
}
static inline RecompMmx MMX_PSRAW(RecompMmx a, uint64_t cnt) {
    RecompMmx r; int i; uint64_t c = (cnt >= 16) ? 15 : cnt;
    for (i = 0; i < 4; i++) r.w[i] = (int16_t)(a.w[i] >> c);
    return r;
}
static inline RecompMmx MMX_PSRAD(RecompMmx a, uint64_t cnt) {
    RecompMmx r; int i; uint64_t c = (cnt >= 32) ? 31 : cnt;
    for (i = 0; i < 2; i++) r.d[i] = (int32_t)(a.d[i] >> c);
    return r;
}

/* -- unpack / pack ---------------------------------------------- */
static inline RecompMmx MMX_PUNPCKLBW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) { r.ub[i*2] = a.ub[i]; r.ub[i*2+1] = b.ub[i]; }
    return r;
}
static inline RecompMmx MMX_PUNPCKHBW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) { r.ub[i*2] = a.ub[i+4]; r.ub[i*2+1] = b.ub[i+4]; }
    return r;
}
static inline RecompMmx MMX_PUNPCKLWD(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 2; i++) { r.uw[i*2] = a.uw[i]; r.uw[i*2+1] = b.uw[i]; }
    return r;
}
static inline RecompMmx MMX_PUNPCKHWD(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 2; i++) { r.uw[i*2] = a.uw[i+2]; r.uw[i*2+1] = b.uw[i+2]; }
    return r;
}
static inline RecompMmx MMX_PUNPCKLDQ(RecompMmx a, RecompMmx b) {
    RecompMmx r; r.ud[0] = a.ud[0]; r.ud[1] = b.ud[0]; return r;
}
static inline RecompMmx MMX_PUNPCKHDQ(RecompMmx a, RecompMmx b) {
    RecompMmx r; r.ud[0] = a.ud[1]; r.ud[1] = b.ud[1]; return r;
}
static inline RecompMmx MMX_PACKSSWB(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) r.b[i]     = recomp_sat_i8(a.w[i]);
    for (i = 0; i < 4; i++) r.b[i + 4] = recomp_sat_i8(b.w[i]);
    return r;
}
static inline RecompMmx MMX_PACKUSWB(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) r.ub[i]     = recomp_sat_u8(a.w[i]);
    for (i = 0; i < 4; i++) r.ub[i + 4] = recomp_sat_u8(b.w[i]);
    return r;
}
static inline RecompMmx MMX_PACKSSDW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 2; i++) r.w[i]     = recomp_sat_i16(a.d[i]);
    for (i = 0; i < 2; i++) r.w[i + 2] = recomp_sat_i16(b.d[i]);
    return r;
}

/* -- word insert / extract / shuffle ----------------------------- */
static inline RecompMmx MMX_PSHUFW(RecompMmx a, uint32_t imm) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) r.uw[i] = a.uw[(imm >> (i * 2)) & 3];
    return r;
}
static inline RecompMmx MMX_PINSRW(RecompMmx a, uint32_t v, uint32_t imm) {
    RecompMmx r = a; r.uw[imm & 3] = (uint16_t)v; return r;
}
static inline uint32_t MMX_PEXTRW(RecompMmx a, uint32_t imm) {
    return (uint32_t)a.uw[imm & 3];
}
static inline uint32_t MMX_PMOVMSKB(RecompMmx a) {
    uint32_t m = 0; int i;
    for (i = 0; i < 8; i++) if (a.ub[i] & 0x80) m |= (1u << i);
    return m;
}
static inline RecompMmx MMX_PSADBW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i; uint32_t s = 0;
    for (i = 0; i < 8; i++)
        s += (uint32_t)(a.ub[i] > b.ub[i] ? a.ub[i] - b.ub[i] : b.ub[i] - a.ub[i]);
    r.q = 0; r.uw[0] = (uint16_t)s;
    return r;
}

#define xmm0 g_xmm0
#define xmm1 g_xmm1
#define xmm2 g_xmm2
#define xmm3 g_xmm3
#define xmm4 g_xmm4
#define xmm5 g_xmm5
#define xmm6 g_xmm6
#define xmm7 g_xmm7
#define mm0 g_mm0
#define mm1 g_mm1
#define mm2 g_mm2
#define mm3 g_mm3
#define mm4 g_mm4
#define mm5 g_mm5
#define mm6 g_mm6
#define mm7 g_mm7
#endif /* RECOMP_GENERATED_CODE */

#endif /* RECOMP_TYPES_SIMD_H */
