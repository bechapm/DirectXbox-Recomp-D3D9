/*
 * hle.h -- replacing XDK library functions by name.
 *
 * An implementation is a plain C function marked with HLE_EXPORT(Name), where
 * Name is the XDK function's own name exactly as tools.xdk_symbols reports it
 * (D3DDevice_SetTexture, not SetTexture). tools.recomp scans for the marker,
 * looks the name up in the title's XDK symbols, and generates a thunk from
 * that address's sub_XXXXXXXX to hle_<Name>. See tools/recomp/hle.py.
 *
 * The generated thunk owns the stack. It is reached exactly like a lifted
 * function -- the caller has pushed the guest return address, so at entry
 * [esp] is that address and the first stack argument is at esp+4 -- and after
 * the implementation returns it pops the return address plus the argument
 * bytes the original function's own `ret N` pops. That count is read from the
 * binary, not from the signature database, which gets a few of them wrong;
 * a wrong count leaves esp off in every caller.
 *
 * So an implementation reads its arguments with HLE_ARG (register arguments
 * of a fastcall or thiscall function are g_ecx and g_edx), sets its result
 * with HLE_RETURN, and never moves esp.
 */
#ifndef XBOXRECOMP_HLE_H
#define XBOXRECOMP_HLE_H

#include <stddef.h>
#include <stdint.h>
#include "../kernel/xbox_memory_layout.h"   /* the guest registers (g_cpu) */

extern ptrdiff_t g_xbox_mem_offset;

/* Marks an implementation, and names it hle_<name>. */
#define HLE_EXPORT(name) void hle_##name(void)

/* On a line of its own in a file: every HLE_EXPORT in the file belongs to
 * `group`, and can be switched off at run time. tools.recomp keeps each one's
 * original body and has its thunk call that body, untouched, while the
 * group's switch says off (tools/recomp/hle.py GROUPS). The one group is
 * audio: the DirectSound replacement, off under RECOMP_AUDIO=apu. */
#define HLE_GROUP(group)

/* On a line of its own, in a grouped file: this replacement stays on while
 * its group is off. Only for one that runs the original body and records
 * what it was told, so the title's own code still does the work. */
#define HLE_APU_OBSERVE(name)

/* Imports an XDK variable's guest address by name, as hle_var_<name>. Must
 * start a line, like HLE_EXPORT: tools.recomp scans for the marker and writes
 * the definition into recomp_hle.c from the title's XDK symbols. 0 means the
 * symbols could not name it, so check before use. */
#define HLE_IMPORT_VAR(name) extern const uint32_t hle_var_##name

/* Declares the original, lifted body of a replaced function as
 * hle_original_<name>, for a replacement that runs the title's own code and
 * then does its own work. Must start a line, like HLE_EXPORT: tools.recomp
 * scans for it, keeps the body as sub_XXXXXXXX_hle_original, and defines the
 * pointer in recomp_hle.c -- 0 when this title does not replace the function. */
#define HLE_ORIGINAL(name) extern void (*const hle_original_##name)(void)

/* Run the original body with the guest stack exactly as the caller left it.
 * The body pops its own return address and arguments, as its `ret N` does;
 * esp is put back afterwards so the thunk's pop is the only one. g_eax keeps
 * the original's result. */
/* Inside an LTCG thunk (g_hle_ltcg_nargs set, see recomp_cpu.h) the body
 * gets the caller's own esp and registers, not the synthetic frame the
 * replacement reads with HLE_ARG; the context is cleared while it runs so a
 * plain replacement reached from inside it does not take it for its own.
 * Afterwards the synthetic frame is back, and g_eax holds the body's result. */
/* How deep this thread is inside a replaced function's own body: what that
 * body writes to the push buffer belongs to the replacement, so a sync point
 * reached from inside it (hle_d3d8_vertex.c, the GPU wait points) leaves the
 * walk alone. */
extern RECOMP_TLS int g_hle_original_depth;

#define HLE_CALL_ORIGINAL(name)                                                \
    do {                                                                       \
        uint32_t hle_esp_ = g_esp;                                             \
        uint32_t hle_n_ = g_hle_ltcg_nargs;                                    \
        if (hle_n_) {                                                          \
            g_esp = g_hle_ltcg_esp;                                            \
            g_eax = g_hle_ltcg_regs[0]; g_ecx = g_hle_ltcg_regs[1];            \
            g_edx = g_hle_ltcg_regs[2]; g_ebx = g_hle_ltcg_regs[3];            \
            g_esi = g_hle_ltcg_regs[4]; g_edi = g_hle_ltcg_regs[5];            \
            g_hle_ltcg_nargs = 0;                                              \
        }                                                                      \
        g_hle_original_depth++;                                                \
        hle_original_##name();                                                 \
        g_hle_original_depth--;                                                \
        g_hle_ltcg_nargs = hle_n_;                                             \
        g_esp = hle_esp_;                                                      \
    } while (0)

/* Guest memory at a guest address. */
#define HLE_MEM32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define HLE_PTR(va)   ((void *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

/* Stack argument i, counting from 0. */
#define HLE_ARG(i) HLE_MEM32(g_esp + 4u + 4u * (uint32_t)(i))

/* Set the guest's return value and leave. The thunk pops the stack. */
#define HLE_RETURN(value)                                                      \
    do {                                                                       \
        g_eax = (uint32_t)(value);                                             \
        return;                                                                \
    } while (0)

#endif /* XBOXRECOMP_HLE_H */
