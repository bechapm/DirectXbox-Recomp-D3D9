/*
 * recomp_cxx.c - catching a guest C++ exception.
 *
 * MSVC compiles `throw x` into _CxxThrowException(&x, &throwinfo), which
 * raises an SEH exception that the kernel hands to each frame's handler in
 * the fs:[0] chain. A C++ frame's handler is __CxxFrameHandler with that
 * function's FuncInfo, and it does three things this file does instead:
 *
 *   1. finds the first frame whose try block covers its current state and
 *      has a catch for the thrown type (or catch(...));
 *   2. unwinds every frame below it, and that frame down to the try, by
 *      calling the unwind actions its FuncInfo lists (destructors);
 *   3. calls the catch block with ebp set to the frame, and resumes the
 *      function at the address the block returns, with esp at the frame's
 *      saved esp ([ebp - 0x10]) and fs:[0] back at the frame's record.
 *
 * The unwind actions and catch blocks are guest code the lifter emits as
 * functions of their own (lifter.scan_cxx_eh). The resume is the part lifted
 * C cannot do by itself, because the function is a native frame further up:
 * each function that owns try blocks keeps a catch point for every call
 * (recomp_cxx.h), and the runtime longjmps to it with the continuation.
 *
 * A rethrow (`throw;`, _CxxThrowException(0, 0)) inside a catch block finds
 * the exception being handled and searches on from the catching frame, whose
 * state now lies past its try. The object is destroyed when a catch block
 * completes, through the throw info's destructor.
 *
 * Nothing happens until it is certain to complete: a frame the title was
 * lifted without (an old lift, or a function the lifter did not recognise),
 * or a funclet with no lifted code, leaves the throw as it was -- reported,
 * and returning. RECOMP_CXX_CATCH=0 does that for every throw.
 *
 * Not done: catch-object copies through virtual bases, SEH __except frames
 * in the chain (skipped; their filters are not run), a throw out of a
 * destructor during unwinding (terminate() on the console).
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "xbox_memory_layout.h"
#include "../../templates/runtime/recomp_cxx.h"

extern ptrdiff_t g_xbox_mem_offset;

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);

/* recomp_trace.c: the [THROW] report, unchanged in what it says. */
void recomp_cxx_report_throw(uint32_t object_va, uint32_t throwinfo_va);

#define FUNCINFO_MAGIC_LO 0x19930520u
#define FUNCINFO_MAGIC_HI 0x19930522u
#define HT_IS_REFERENCE   0x08u
#define CT_IS_SIMPLE_TYPE 0x01u
#define CT_HAS_VIRTUAL_BASE 0x04u

static RECOMP_TLS recomp_cxx_frame *t_top;

/* Exceptions whose catch block is running, innermost last: what a rethrow
 * rethrows. sp is the dispatch's native frame, to drop entries a landing
 * abandons. */
static RECOMP_TLS struct inflight { uint32_t obj, ti; uintptr_t sp; } t_exc[8];
static RECOMP_TLS int t_nexc;

/* Guest VA 0 is host address g_xbox_mem_offset: 0 on Windows, where guest
 * memory sits at its own addresses, so this is not a null test. */
static const uint8_t *mem(void) { return (const uint8_t *)(uintptr_t)g_xbox_mem_offset; }

/* Main RAM or the contiguous window, as recomp_trace.c's guest_readable. */
static int readable(uint32_t va, uint32_t bytes)
{
    if (!va)
        return 0;
    if ((uint64_t)va + bytes <= (uint64_t)xbox_GetMappedSize())
        return 1;
    return va >= 0x80000000u && (uint64_t)va + bytes <= 0x84000000ull;
}

static uint32_t rd32(uint32_t va)
{
    return readable(va, 4) ? *(const uint32_t *)(mem() + va) : 0;
}

static void wr32(uint32_t va, uint32_t v)
{
    if (readable(va, 4))
        *(uint32_t *)(uint8_t *)(mem() + va) = v;
}

static const char *type_name(uint32_t type_info)
{
    return readable(type_info + 8, 1) ? (const char *)(mem() + type_info + 8) : "?";
}

static int catch_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_CXX_CATCH");
        on = !(e && e[0] == '0');
    }
    return on;
}

static recomp_func_t lookup(uint32_t va)
{
    recomp_func_t fn = va ? recomp_lookup_manual(va) : NULL;
    return fn ? fn : (va ? recomp_lookup(va) : NULL);
}

/* FuncInfo behind a frame handler `mov eax, FuncInfo; jmp __CxxFrameHandler`. */
static uint32_t funcinfo(uint32_t handler)
{
    uint32_t fi, magic;
    const uint8_t *h;

    if (!readable(handler, 10))
        return 0;
    h = mem() + handler;
    if (h[0] != 0xB8 || h[5] != 0xE9)
        return 0;
    fi = *(const uint32_t *)(h + 1);
    magic = rd32(fi);
    return magic >= FUNCINFO_MAGIC_LO && magic <= FUNCINFO_MAGIC_HI ? fi : 0;
}

/* Call guest code at va as the frame handler would: ebp is the frame's, the
 * return address a dummy the code pops. Returns eax. */
static uint32_t call_guest(recomp_func_t fn, uint32_t frame_ebp)
{
    uint32_t esp = g_esp;

    g_ebp = g_seh_ebp = frame_ebp;
    g_esp -= 4;
    wr32(g_esp, 0);
    fn();
    g_esp = esp;
    return g_eax;
}

/* A __thiscall method with up to one argument. */
static void call_method(recomp_func_t fn, uint32_t self, int nargs, uint32_t arg)
{
    uint32_t esp = g_esp;

    g_ecx = self;
    if (nargs) {
        g_esp -= 4;
        wr32(g_esp, arg);
    }
    g_esp -= 4;
    wr32(g_esp, 0);
    fn();
    g_esp = esp;
}

/* Whether every unwind action from `state` down to `target` has code. */
static int unwind_ready(uint32_t fi, int state, int target)
{
    uint32_t map = rd32(fi + 8), max = rd32(fi + 4);
    int guard = 0;

    while (state != target && state >= 0 && (uint32_t)state < max && guard++ < 4096) {
        uint32_t action = rd32(map + 8u * (uint32_t)state + 4);
        if (action && !lookup(action))
            return 0;
        state = (int)rd32(map + 8u * (uint32_t)state);
    }
    return 1;
}

/* Run a frame's unwind map from its state down to `target`, as
 * __FrameUnwindToState does: the state moves before each action runs. */
static void unwind_frame(uint32_t rec, uint32_t fi, int target)
{
    uint32_t map = rd32(fi + 8), max = rd32(fi + 4);
    int state = (int)rd32(rec + 8), guard = 0;

    while (state != target && state >= 0 && (uint32_t)state < max && guard++ < 4096) {
        int to = (int)rd32(map + 8u * (uint32_t)state);
        uint32_t action = rd32(map + 8u * (uint32_t)state + 4);
        wr32(rec + 8, (uint32_t)to);
        if (action)
            call_guest(lookup(action), rec + 0x0Cu);
        state = to;
    }
    wr32(rec + 8, (uint32_t)target);
}

/* Whether HandlerType `ht` catches the thrown type; the CatchableType that
 * matched in *ct (0 for catch(...)). */
static int catches(uint32_t ht, uint32_t ti, uint32_t *ct)
{
    uint32_t want = rd32(ht + 4), cta, n, i;

    *ct = 0;
    if (!want)
        return 1;                                   /* catch (...) */
    cta = rd32(ti + 12);
    n = rd32(cta);
    for (i = 0; i < n && i < 64; i++) {
        uint32_t c = rd32(cta + 4 + 4 * i), type = rd32(c + 4);
        if (type == want || strcmp(type_name(type), type_name(want)) == 0) {
            *ct = c;
            return 1;
        }
    }
    return 0;
}

/* Put the exception object where a typed catch block reads it. */
static void copy_catch_object(uint32_t ht, uint32_t ct, uint32_t obj, uint32_t frame_ebp)
{
    uint32_t disp = rd32(ht + 8), adj = rd32(ht), props, size, copy_fn;
    uint32_t dest, src;

    if (!ct || !disp)
        return;
    props = rd32(ct);
    size = rd32(ct + 20);
    copy_fn = rd32(ct + 24);
    dest = frame_ebp + disp;
    src = obj + rd32(ct + 8);                       /* thisDisplacement.mdisp */
    if (adj & HT_IS_REFERENCE) {
        wr32(dest, src);
    } else if ((props & CT_IS_SIMPLE_TYPE) || !copy_fn || !lookup(copy_fn)) {
        if (readable(dest, size) && readable(obj, size))
            memmove((uint8_t *)mem() + dest, mem() + obj, size);
    } else {
        call_method(lookup(copy_fn), dest, 1, src);
        if (props & CT_HAS_VIRTUAL_BASE) {
            static int said;
            if (!said++)
                fprintf(stderr, "[CXX] a caught type has a virtual base; its copy "
                        "constructor ran without the most-derived flag\n");
        }
    }
}

static void destroy_object(uint32_t obj, uint32_t ti)
{
    uint32_t dtor = rd32(ti + 4);
    recomp_func_t fn = dtor ? lookup(dtor) : NULL;

    if (obj && fn)
        call_method(fn, obj, 0, 0);
}

/* A [CXX] line; the first 64 only, since a title may throw in a loop. */
static void say(const char *fmt, ...)
{
    static int lines;
    va_list ap;

    if (lines++ >= 64)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

/* Catch (obj, ti) if a lifted frame can. Returns only when nothing can. */
static void dispatch(uint32_t obj, uint32_t ti)
{
    uintptr_t here = (uintptr_t)&obj;
    uint32_t rec, found_rec = 0, found_fi = 0, found_ht = 0, found_ct = 0;
    uint32_t try_entry = 0, site = rd32(g_esp);
    recomp_cxx_frame *point;
    uint32_t r, cont, frame_ebp;
    int depth;

    /* Catch points deeper than this are frames a guest longjmp abandoned. */
    while (t_top && (uintptr_t)t_top < here)
        t_top = t_top->prev;

    /* 1. The catching frame, with no side effects yet. */
    rec = rd32(XBOX_FS_BASE);
    for (depth = 0; depth < 256 && rec && rec != 0xFFFFFFFFu && !found_rec; depth++) {
        uint32_t fi = funcinfo(rd32(rec + 4)), t, n_try, map;
        int state = (int)rd32(rec + 8);

        if (fi) {
            n_try = rd32(fi + 12);
            map = rd32(fi + 16);
            for (t = 0; t < n_try && t < 256 && !found_rec; t++) {
                uint32_t e = map + 20 * t, c, n_catch, handlers;
                if (state < (int)rd32(e) || state > (int)rd32(e + 4))
                    continue;
                n_catch = rd32(e + 12);
                handlers = rd32(e + 16);
                for (c = 0; c < n_catch && c < 64; c++) {
                    uint32_t ht = handlers + 16 * c, ct;
                    if (catches(ht, ti, &ct)) {
                        found_rec = rec; found_fi = fi; found_ht = ht;
                        found_ct = ct; try_entry = e;
                        break;
                    }
                }
            }
        }
        if (!found_rec)
            rec = rd32(rec);
    }
    if (!found_rec) {
        say("[CXX] nothing in the frame chain catches the throw from 0x%08X "
            "(object 0x%08X, throw info 0x%08X); it returns as before\n",
            site, obj, ti);
        return;
    }

    /* 2. Everything the catch needs has lifted code, or nothing is done. */
    {
        uint32_t handler = rd32(found_ht + 12);
        int ok = lookup(handler) != NULL;
        for (r = rd32(XBOX_FS_BASE); ok && r != found_rec; r = rd32(r)) {
            uint32_t fi = funcinfo(rd32(r + 4));
            if (fi)
                ok = unwind_ready(fi, (int)rd32(r + 8), -1);
        }
        if (ok)
            ok = unwind_ready(found_fi, (int)rd32(found_rec + 8), (int)rd32(try_entry));
        if (!ok) {
            say("[CXX] the frame at 0x%08X catches the throw from 0x%08X, but its "
                "catch block (0x%08X) or an unwind action has no lifted code: lift "
                "again. The throw returns as before\n",
                found_rec + 0x0Cu, site, handler);
            return;
        }
    }

    /* 3. Unwind the frames below, then the catching frame down to its try,
     *    and run the catch block. */
    for (r = rd32(XBOX_FS_BASE); r != found_rec; r = rd32(r)) {
        uint32_t fi = funcinfo(rd32(r + 4));
        if (fi)
            unwind_frame(r, fi, -1);
    }
    frame_ebp = found_rec + 0x0Cu;
    unwind_frame(found_rec, found_fi, (int)rd32(try_entry));
    wr32(found_rec + 8, rd32(try_entry + 4) + 1);    /* past the try: the catch's own state */
    wr32(XBOX_FS_BASE, found_rec);
    copy_catch_object(found_ht, found_ct, obj, frame_ebp);

    say("[CXX] caught the throw from 0x%08X in the frame at 0x%08X; catch block "
        "0x%08X (%s)\n", site, frame_ebp, rd32(found_ht + 12),
        found_ct ? type_name(rd32(found_ct + 4)) : "...");

    if (t_nexc < (int)(sizeof t_exc / sizeof t_exc[0])) {
        t_exc[t_nexc].obj = obj;
        t_exc[t_nexc].ti = ti;
        t_exc[t_nexc].sp = here;
        t_nexc++;
    }
    cont = call_guest(lookup(rd32(found_ht + 12)), frame_ebp);
    /* The block completed (a rethrow inside it never comes back here). */
    if (t_nexc && t_exc[t_nexc - 1].sp == here)
        t_nexc--;
    destroy_object(obj, ti);

    /* 4. Resume the catching function at cont. */
    for (point = t_top; point; point = point->prev)
        if (point->entry_esp == found_rec + 0x10u)
            break;
    if (!point) {
        fprintf(stderr, "[CXX] the catch block at 0x%08X returned 0x%08X, but the "
                "function owning the frame at 0x%08X has no catch point (its try "
                "was not recognised by the lifter). Stopping: there is nowhere to "
                "resume.\n", rd32(found_ht + 12), cont, frame_ebp);
        fflush(stderr);
        abort();
    }
    /* Catch blocks this landing abandons: their exceptions are finished. */
    while (t_nexc && t_exc[t_nexc - 1].sp < (uintptr_t)point) {
        t_nexc--;
        if (t_exc[t_nexc].obj != obj)
            destroy_object(t_exc[t_nexc].obj, t_exc[t_nexc].ti);
    }
    t_top = point;
    g_esp = rd32(frame_ebp - 0x10u);
    g_ebp = g_seh_ebp = frame_ebp;
    point->ebp = frame_ebp;
    point->cont = cont;
    RECOMP_CXX_LONGJMP(point->jb, 1);
}

void recomp_cxx_throw(uint32_t object_va, uint32_t throwinfo_va)
{
    recomp_cxx_report_throw(object_va, throwinfo_va);
    if (!catch_on())
        return;
    if (!object_va) {                               /* throw; */
        if (!t_nexc) {
            say("[CXX] a rethrow from 0x%08X with no exception being handled\n",
                rd32(g_esp));
            return;
        }
        object_va = t_exc[t_nexc - 1].obj;
        throwinfo_va = t_exc[t_nexc - 1].ti;
    }
    dispatch(object_va, throwinfo_va);
}

void recomp_cxx_enter(recomp_cxx_frame *f, uint32_t func)
{
    f->prev = t_top;
    f->func = func;
    f->entry_esp = g_esp;
    f->ebp = f->cont = 0;
    t_top = f;
}

void recomp_cxx_leave(recomp_cxx_frame *f)
{
    t_top = f->prev;
}

void recomp_cxx_lost(recomp_cxx_frame *f)
{
    fprintf(stderr, "[CXX] sub_%08X was resumed at 0x%08X, which is not one of the "
            "continuations the lifter gave it a label for. Stopping.\n",
            f->func, f->cont);
    fflush(stderr);
    abort();
}
