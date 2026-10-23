/*
 * mmio_decode.h -- one x86-64 instruction decoder for trapped MMIO.
 *
 * A device whose registers need semantics cannot be plain memory: the reads
 * have to be answered and the writes have to be seen. The way that works here
 * is to leave the page PAGE_NOACCESS, catch the access in a vectored handler,
 * decode the faulting instruction, service it against the device model, and
 * step over it.
 *
 * That decoder existed twice before this file -- once in nv2a_mmio_hook.c and
 * once in apu_mmio_hook.c -- as the same opcode table written out against two
 * different pairs of accessors. This is the same logic with the accessors
 * passed in, so a third device does not need a third copy. The two originals
 * still carry their own and can move onto this whenever they are next touched.
 *
 * Header-only and static inline: one function, one caller per device, and a
 * library for it would be more build wiring than code.
 *
 * The instructions covered are what the XDK device code actually emits against
 * registers -- moves both ways, the immediate forms, the zero-extending loads,
 * and the ALU, read-modify-write and flag-setting forms a poll loop is built
 * from (DirectSound polls with `test dword [mem], imm32`, a USB driver masks a
 * status register with `and edx, [mem]`, and a C compiler folds `x += REG`
 * into an ALU op with a memory operand), and the SSE scalar moves a compiler
 * uses for a value it holds in an XMM register (GCC stores the APU's voice
 * registers with `movd [mem], xmm0`). See mmio_emulate for the list.
 * Anything outside that set returns 0 rather than guessing: stepping over an
 * instruction that was not understood corrupts the guest silently, which is
 * far worse than a fault naming the opcode.
 */
#ifndef MMIO_DECODE_H
#define MMIO_DECODE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* The decoder is chosen by the CPU, not the OS: it runs wherever the host
 * is x86-64. On Windows the register file is the VEH's CONTEXT; on a POSIX
 * x86-64 host it is mmio_x86_ctx, the same field names filled from the
 * signal's ucontext (mmio_x86_from_ucontext). An arm64 host uses
 * mmio_decode_a64.h instead. */
#if defined(_WIN32)
#include <windows.h>
typedef CONTEXT mmio_x86_ctx;
#define MMIO_X86_DECODER 1
#elif defined(__x86_64__)
typedef struct mmio_x86_ctx {
    uint64_t Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi;
    uint64_t R8, R9, R10, R11, R12, R13, R14, R15;
    uint64_t Rip;
    uint32_t EFlags;
    /* XMM0-15, copied in and out like the rest, so that a dry run
     * (fault_emulate.c) changes nothing until it is committed. */
    uint8_t  Xmm[16][16];
} mmio_x86_ctx;
#define MMIO_X86_DECODER 1
#endif

#if defined(MMIO_X86_DECODER)

/* Service one register access. dev is passed straight back to the callbacks.
 * mmio_decode_a64.h declares the same two types. */
#ifndef MMIO_ACCESS_FN_DEFINED
#define MMIO_ACCESS_FN_DEFINED
typedef uint64_t (*mmio_read_fn)(void *dev, uint32_t off, int size);
typedef void     (*mmio_write_fn)(void *dev, uint32_t off, uint64_t val, int size);
#endif

static inline uint64_t *mmio_ctx_reg(mmio_x86_ctx *c, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t *)&c->Rax;   case 1:  return (uint64_t *)&c->Rcx;
    case 2:  return (uint64_t *)&c->Rdx;   case 3:  return (uint64_t *)&c->Rbx;
    case 4:  return (uint64_t *)&c->Rsp;   case 5:  return (uint64_t *)&c->Rbp;
    case 6:  return (uint64_t *)&c->Rsi;   case 7:  return (uint64_t *)&c->Rdi;
    case 8:  return (uint64_t *)&c->R8;    case 9:  return (uint64_t *)&c->R9;
    case 10: return (uint64_t *)&c->R10;   case 11: return (uint64_t *)&c->R11;
    case 12: return (uint64_t *)&c->R12;   case 13: return (uint64_t *)&c->R13;
    case 14: return (uint64_t *)&c->R14;   case 15: return (uint64_t *)&c->R15;
    default: return NULL;
    }
}

/* XMM register n's 16 bytes, low lane first. */
static inline uint8_t *mmio_ctx_xmm(mmio_x86_ctx *c, int n)
{
#if defined(_WIN32)
    return (uint8_t *)&c->Xmm0 + 16 * (n & 15);
#else
    return c->Xmm[n & 15];
#endif
}

static inline int mmio_modrm_len(const uint8_t *ip, int rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm  = (modrm & 7) | (rex_b ? 8 : 0);
    int len = 1;

    if (mod == 3) return 1;
    if ((rm & 7) == 4) {
        len += 1;                                   /* SIB    */
        /* mod 0 with SIB base 5 has no base register and a disp32 instead;
         * missing it left RIP inside the operand (apu_mmio_hook.c's copy had
         * this fixed, this one did not). */
        if (mod == 0 && (ip[1] & 7) == 5) len += 4;
    }
    else if (mod == 0 && (rm & 7) == 5) len += 4;   /* disp32 / RIP-relative */
    /* The displacement for mod 1/2 applies with or without a SIB byte. */
    if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

/* The ALU operations, numbered as the reg field of group 1 (80/81/83) and as
 * bits 5:3 of the classic two-operand opcodes. TEST has no slot of its own. */
enum { MMIO_ALU_ADD = 0, MMIO_ALU_OR = 1, MMIO_ALU_AND = 4, MMIO_ALU_SUB = 5,
       MMIO_ALU_XOR = 6, MMIO_ALU_CMP = 7, MMIO_ALU_TEST = 8 };

static inline uint64_t mmio_size_mask(int size)
{
    return size >= 8 ? ~0ULL : (1ULL << (size * 8)) - 1;
}

/* a OP b at the given width, with the flags a following jcc reads: ZF, SF, CF
 * and OF. PF and AF are left alone; nothing branches on them after a register
 * read. The logical operations clear CF and OF, as the hardware does.
 *
 * Only those four bits of EFlags are touched, so the rest (TF, IF, DF, the
 * reserved bit 1) goes back to the faulting thread as it came: on Windows the
 * VEH's CONTEXT is resumed directly, on POSIX fault_emulate.c copies EFlags
 * back into the ucontext. */
static inline uint64_t mmio_alu(mmio_x86_ctx *ctx, int op, uint64_t a,
                                uint64_t b, int size)
{
    uint64_t m = mmio_size_mask(size), sign = 1ULL << (size * 8 - 1), r;
    int cf = 0, of = 0;

    a &= m; b &= m;
    switch (op) {
    case MMIO_ALU_ADD:
        r  = (a + b) & m;
        cf = r < a;
        of = ((~(a ^ b) & (a ^ r)) & sign) != 0;
        break;
    case MMIO_ALU_SUB: case MMIO_ALU_CMP:
        r  = (a - b) & m;
        cf = a < b;
        of = (((a ^ b) & (a ^ r)) & sign) != 0;
        break;
    case MMIO_ALU_OR:  r = a | b; break;
    case MMIO_ALU_XOR: r = a ^ b; break;
    default:           r = a & b; break;                /* AND, TEST */
    }
    ctx->EFlags &= ~(0x0001u | 0x0040u | 0x0080u | 0x0800u);
    if (cf)       ctx->EFlags |= 0x0001u;               /* CF */
    if (r == 0)   ctx->EFlags |= 0x0040u;               /* ZF */
    if (r & sign) ctx->EFlags |= 0x0080u;               /* SF */
    if (of)       ctx->EFlags |= 0x0800u;               /* OF */
    return r;
}

/* A general register at an operand width. Without a REX prefix, byte
 * registers 4-7 are AH, CH, DH and BH -- bits 15:8 of registers 0-3. */
static inline uint64_t mmio_get_reg(mmio_x86_ctx *c, int reg, int size,
                                    int has_rex)
{
    if (size == 1 && !has_rex && reg >= 4 && reg < 8)
        return (*mmio_ctx_reg(c, reg - 4) >> 8) & 0xFF;
    return *mmio_ctx_reg(c, reg) & mmio_size_mask(size);
}

/* Write with x86-64 width rules: a 32-bit write zeroes bits 63:32, an 8- or
 * 16-bit write merges into what was there. */
static inline void mmio_set_reg(mmio_x86_ctx *c, int reg, uint64_t v, int size,
                                int has_rex)
{
    uint64_t *d;
    if (size == 1 && !has_rex && reg >= 4 && reg < 8) {
        d  = mmio_ctx_reg(c, reg - 4);
        *d = (*d & ~0xFF00ULL) | ((v & 0xFF) << 8);
        return;
    }
    d = mmio_ctx_reg(c, reg);
    if (size == 1)      *d = (*d & ~0xFFULL)   | (v & 0xFF);
    else if (size == 2) *d = (*d & ~0xFFFFULL) | (v & 0xFFFF);
    else if (size == 4) *d = v & 0xFFFFFFFFULL;
    else                *d = v;
}

/* The immediate of an instruction whose operand size is `size`: imm8 for byte
 * forms, imm16 under a 66 prefix, otherwise imm32 sign-extended -- never
 * imm64: REX.W sign-extends the imm32. Reading four bytes for the 16-bit form
 * took two bytes of the next instruction into the value and resumed RIP two
 * bytes past it (GCC emits `movw $imm, (reg)` for a 16-bit MEM store; found
 * on x86-64 Linux). */
static inline uint64_t mmio_imm(const uint8_t *p, int size, int *len)
{
    if (size == 1) { *len = 1; return p[0]; }
    if (size == 2) { *len = 2; return (uint64_t)p[0] | ((uint64_t)p[1] << 8); }
    *len = 4;
    return (uint64_t)(int64_t)(int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)
                                      | ((uint32_t)p[2] << 16)
                                      | ((uint32_t)p[3] << 24));
}

/* 1 if the instruction at ctx->Rip was serviced and Rip advanced past it.
 *
 * Covered: MOV both ways (88/89 8A/8B C6/C7), MOVZX (0F B6/B7), the
 * two-operand ALU forms in both directions for ADD/OR/AND/SUB/XOR/CMP
 * (00-03 08-0B 20-23 28-2B 30-33 38-3B), group 1 with an immediate
 * (80/81/83, not ADC/SBB), and TEST against a register or an immediate
 * (84/85, F6/F7 /0 and /1), and the SSE scalar moves between an XMM
 * register and memory: MOVD/MOVQ (66 0F 6E/7E, 66 0F D6, F3 0F 7E) and
 * MOVSS/MOVSD (F3/F2 0F 10/11), a load zeroing the rest of the register as
 * the hardware does. A register-destination form reads the device
 * once; a memory-destination form reads, then writes (CMP and TEST only
 * read). ADC/SBB, the rest of F6/F7 (NOT/NEG/MUL/DIV), string moves and
 * anything else return 0.
 *
 * The order of the callbacks is part of the contract: fault_emulate.c's
 * guest-memory path runs the instruction once against checking callbacks
 * (a dry run, nothing committed) and then for real, and expects the same
 * reads and writes both times. */
static inline int mmio_emulate(mmio_x86_ctx *ctx, uint32_t off, void *dev,
                               mmio_read_fn rd, mmio_write_fn wr)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;
    int prefix = 0, has66 = 0, hasF2 = 0, hasF3 = 0, rex = 0, has_rex = 0;
    const uint8_t *op;
    int size, rex_w, rex_r, rex_b, mlen, reg, ilen, alu;
    uint64_t m, v, imm;

    if (!rd || !wr)
        return 0;

    for (;;) {
        uint8_t b = ip[prefix];
        if (b == 0x66)                   { has66 = 1; prefix++; }
        else if (b == 0xF2)              { hasF2 = 1; prefix++; }
        else if (b == 0xF3)              { hasF3 = 1; prefix++; }
        else if (b >= 0x40 && b <= 0x4F) { rex = b; has_rex = 1; prefix++; }
        else break;
    }
    rex_w = has_rex && (rex & 0x08);
    rex_r = has_rex && (rex & 0x04);
    rex_b = has_rex && (rex & 0x01);

    op   = ip + prefix;
    /* REX.W wins over 66: with both, the operand is 64-bit. */
    size = rex_w ? 8 : (has66 ? 2 : 4);

    /* Two-operand ALU: 00-03 ADD, 08-0B OR, 20-23 AND, 28-2B SUB, 30-33 XOR,
     * 38-3B CMP. Bit 0 clear is the byte form; bit 1 set makes the register
     * the destination. 10-1B (ADC/SBB) are not decoded. */
    if (op[0] < 0x40 && (op[0] & 7) < 4) {
        alu = op[0] >> 3;
        if (alu == 2 || alu == 3)
            return 0;
        if (!(op[0] & 1)) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        m    = rd(dev, off, size);
        if (op[0] & 2) {                             /* op r, r/m      */
            v = mmio_alu(ctx, alu, mmio_get_reg(ctx, reg, size, has_rex), m,
                         size);
            if (alu != MMIO_ALU_CMP)
                mmio_set_reg(ctx, reg, v, size, has_rex);
        } else {                                     /* op r/m, r      */
            v = mmio_alu(ctx, alu, m, mmio_get_reg(ctx, reg, size, has_rex),
                         size);
            if (alu != MMIO_ALU_CMP)
                wr(dev, off, v, size);
        }
        ctx->Rip += prefix + 1 + mlen;
        return 1;
    }

    switch (op[0]) {
    case 0x88: case 0x89:                            /* MOV r/m, r   (write) */
        if (op[0] == 0x88) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        wr(dev, off, mmio_get_reg(ctx, reg, size, has_rex), size);
        ctx->Rip += prefix + 1 + mlen;
        return 1;

    case 0xC6: case 0xC7:                            /* MOV r/m, imm         */
        if (op[0] == 0xC6) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        imm  = mmio_imm(op + 1 + mlen, size, &ilen);
        wr(dev, off, imm & mmio_size_mask(size), size);
        ctx->Rip += prefix + 1 + mlen + ilen;
        return 1;

    case 0x8A: case 0x8B:                            /* MOV r, r/m   (read)  */
        if (op[0] == 0x8A) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        mmio_set_reg(ctx, reg, rd(dev, off, size), size, has_rex);
        ctx->Rip += prefix + 1 + mlen;
        return 1;

    case 0x84: case 0x85:                            /* TEST r/m, r          */
        if (op[0] == 0x84) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        mmio_alu(ctx, MMIO_ALU_TEST, rd(dev, off, size),
                 mmio_get_reg(ctx, reg, size, has_rex), size);
        ctx->Rip += prefix + 1 + mlen;
        return 1;

    case 0x80: case 0x81: case 0x83:                 /* group 1 r/m, imm     */
        alu = (op[1] >> 3) & 7;
        if (alu == 2 || alu == 3)                    /* ADC, SBB             */
            return 0;
        if (op[0] == 0x80) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        if (op[0] == 0x83) {                         /* imm8, sign-extended  */
            imm  = (uint64_t)(int64_t)(int8_t)op[1 + mlen];
            ilen = 1;
        } else {
            imm = mmio_imm(op + 1 + mlen, size, &ilen);
        }
        v = mmio_alu(ctx, alu, rd(dev, off, size), imm, size);
        if (alu != MMIO_ALU_CMP)
            wr(dev, off, v, size);
        ctx->Rip += prefix + 1 + mlen + ilen;
        return 1;

    case 0xF6: case 0xF7:                            /* TEST r/m, imm        */
        if (((op[1] >> 3) & 7) > 1)                  /* /1 aliases /0        */
            return 0;
        if (op[0] == 0xF6) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        imm  = mmio_imm(op + 1 + mlen, size, &ilen);
        mmio_alu(ctx, MMIO_ALU_TEST, rd(dev, off, size), imm, size);
        ctx->Rip += prefix + 1 + mlen + ilen;
        return 1;

    case 0x0F:
        if (op[1] == 0xB6 || op[1] == 0xB7) {        /* MOVZX r, r/m8|16     */
            int s = (op[1] == 0xB6) ? 1 : 2;
            mlen  = mmio_modrm_len(op + 2, rex_b);
            reg   = ((op[2] >> 3) & 7) | (rex_r ? 8 : 0);
            mmio_set_reg(ctx, reg, rd(dev, off, s) & mmio_size_mask(s),
                         rex_w ? 8 : (has66 ? 2 : 4), 1);
            ctx->Rip += prefix + 2 + mlen;
            return 1;
        }
        {
            /* SSE: 66 is the opcode's mandatory prefix here, not an operand
             * size. `size` is the memory operand; `load` names the
             * direction. */
            int load = -1, n;
            uint8_t *x;
            uint8_t b2 = op[1];

            if (has66 && b2 == 0x6E)      { load = 1; size = rex_w ? 8 : 4; }   /* MOVD/Q xmm, m  */
            else if (has66 && b2 == 0x7E) { load = 0; size = rex_w ? 8 : 4; }   /* MOVD/Q m, xmm  */
            else if (has66 && b2 == 0xD6) { load = 0; size = 8; }               /* MOVQ m64, xmm  */
            else if (hasF3 && b2 == 0x7E) { load = 1; size = 8; }               /* MOVQ xmm, m64  */
            else if ((hasF3 || hasF2) && (b2 == 0x10 || b2 == 0x11)) {          /* MOVSS / MOVSD  */
                load = b2 == 0x10;
                size = hasF2 ? 8 : 4;
            }
            if (load < 0)
                return 0;
            mlen = mmio_modrm_len(op + 2, rex_b);
            reg  = ((op[2] >> 3) & 7) | (rex_r ? 8 : 0);
            x    = mmio_ctx_xmm(ctx, reg);
            n    = size;
            if (load) {
                v = rd(dev, off, size) & mmio_size_mask(size);
                memset(x, 0, 16);
                memcpy(x, &v, (size_t)n);
            } else {
                v = 0;
                memcpy(&v, x, (size_t)n);
                wr(dev, off, v, size);
            }
            ctx->Rip += prefix + 2 + mlen;
            return 1;
        }

    default:
        return 0;
    }
}

/* ---- POSIX x86-64: the register file from a signal's ucontext ---------- */
#if !defined(_WIN32)
/* Linux names the registers (REG_RAX...) only under _GNU_SOURCE, which has
 * to be defined before the first system header -- something only the
 * including file can do. So the glue exists where that file asked for it
 * (fault_emulate.c does); any other includer just gets the decoder. */
#if defined(__linux__) && defined(_GNU_SOURCE)
#include <ucontext.h>
static inline void mmio_x86_from_ucontext(mmio_x86_ctx *c, const ucontext_t *uc)
{
    const greg_t *g = uc->uc_mcontext.gregs;
    c->Rax = g[REG_RAX]; c->Rcx = g[REG_RCX]; c->Rdx = g[REG_RDX]; c->Rbx = g[REG_RBX];
    c->Rsp = g[REG_RSP]; c->Rbp = g[REG_RBP]; c->Rsi = g[REG_RSI]; c->Rdi = g[REG_RDI];
    c->R8  = g[REG_R8];  c->R9  = g[REG_R9];  c->R10 = g[REG_R10]; c->R11 = g[REG_R11];
    c->R12 = g[REG_R12]; c->R13 = g[REG_R13]; c->R14 = g[REG_R14]; c->R15 = g[REG_R15];
    c->Rip = g[REG_RIP]; c->EFlags = (uint32_t)g[REG_EFL];
    if (uc->uc_mcontext.fpregs)
        memcpy(c->Xmm, uc->uc_mcontext.fpregs->_xmm, sizeof c->Xmm);
    else
        memset(c->Xmm, 0, sizeof c->Xmm);
}
static inline void mmio_x86_to_ucontext(const mmio_x86_ctx *c, ucontext_t *uc)
{
    greg_t *g = uc->uc_mcontext.gregs;
    g[REG_RAX] = c->Rax; g[REG_RCX] = c->Rcx; g[REG_RDX] = c->Rdx; g[REG_RBX] = c->Rbx;
    g[REG_RSP] = c->Rsp; g[REG_RBP] = c->Rbp; g[REG_RSI] = c->Rsi; g[REG_RDI] = c->Rdi;
    g[REG_R8]  = c->R8;  g[REG_R9]  = c->R9;  g[REG_R10] = c->R10; g[REG_R11] = c->R11;
    g[REG_R12] = c->R12; g[REG_R13] = c->R13; g[REG_R14] = c->R14; g[REG_R15] = c->R15;
    g[REG_RIP] = c->Rip; g[REG_EFL] = c->EFlags;
    if (uc->uc_mcontext.fpregs)
        memcpy(uc->uc_mcontext.fpregs->_xmm, c->Xmm, sizeof c->Xmm);
}
#define MMIO_X86_HAVE_UCONTEXT 1
#elif defined(__APPLE__)
#include <sys/ucontext.h>
static inline void mmio_x86_from_ucontext(mmio_x86_ctx *c, const ucontext_t *uc)
{
    const struct __darwin_x86_thread_state64 *s = &uc->uc_mcontext->__ss;
    c->Rax = s->__rax; c->Rcx = s->__rcx; c->Rdx = s->__rdx; c->Rbx = s->__rbx;
    c->Rsp = s->__rsp; c->Rbp = s->__rbp; c->Rsi = s->__rsi; c->Rdi = s->__rdi;
    c->R8  = s->__r8;  c->R9  = s->__r9;  c->R10 = s->__r10; c->R11 = s->__r11;
    c->R12 = s->__r12; c->R13 = s->__r13; c->R14 = s->__r14; c->R15 = s->__r15;
    c->Rip = s->__rip; c->EFlags = (uint32_t)s->__rflags;
    memcpy(c->Xmm, &uc->uc_mcontext->__fs.__fpu_xmm0, sizeof c->Xmm);
}
static inline void mmio_x86_to_ucontext(const mmio_x86_ctx *c, ucontext_t *uc)
{
    struct __darwin_x86_thread_state64 *s = &uc->uc_mcontext->__ss;
    s->__rax = c->Rax; s->__rcx = c->Rcx; s->__rdx = c->Rdx; s->__rbx = c->Rbx;
    s->__rsp = c->Rsp; s->__rbp = c->Rbp; s->__rsi = c->Rsi; s->__rdi = c->Rdi;
    s->__r8  = c->R8;  s->__r9  = c->R9;  s->__r10 = c->R10; s->__r11 = c->R11;
    s->__r12 = c->R12; s->__r13 = c->R13; s->__r14 = c->R14; s->__r15 = c->R15;
    s->__rip = c->Rip; s->__rflags = c->EFlags;
    memcpy(&uc->uc_mcontext->__fs.__fpu_xmm0, c->Xmm, sizeof c->Xmm);
}
#define MMIO_X86_HAVE_UCONTEXT 1
#endif
#endif /* !_WIN32 */

#endif /* MMIO_X86_DECODER */
#endif /* MMIO_DECODE_H */
