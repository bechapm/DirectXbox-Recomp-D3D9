/*
 * recomp_cxx.h - catching a guest C++ exception.
 *
 * Shared by generated code (tools/recomp copies this file into gen/ and
 * recomp_types.h includes it) and the runtime (src/kernel/recomp_cxx.c,
 * which includes it from templates/runtime by relative path), so the frame
 * below has one layout.
 *
 * A lifted function is a real C function, so a guest catch cannot simply
 * jump to the catch block's continuation: the native frames between the
 * throw and the catching function are still there. A function that owns try
 * blocks therefore carries a catch point -- a native jmp_buf taken in its own
 * body -- for the duration of each call (translator: _emit_cxx_wrapper and
 * _emit_cxx_landing). When a throw is caught by that function, the runtime
 * runs the guest's own unwind actions and catch block, then longjmps to the
 * catch point with the address the catch block returned, and the body jumps
 * to that label. See docs/technical/cpp-exceptions.md.
 */
#ifndef RECOMP_CXX_H
#define RECOMP_CXX_H

#include <stdint.h>
#include <setjmp.h>

/* _setjmp/_longjmp off Windows: plain setjmp saves the signal mask, which on
 * macOS is a system call on every entry to every function that owns a try. */
#if defined(_WIN32)
#define RECOMP_CXX_SETJMP(jb)      setjmp(jb)
#define RECOMP_CXX_LONGJMP(jb, v)  longjmp(jb, v)
#else
#define RECOMP_CXX_SETJMP(jb)      _setjmp(jb)
#define RECOMP_CXX_LONGJMP(jb, v)  _longjmp(jb, v)
#endif

typedef struct recomp_cxx_frame {
    jmp_buf jb;                         /* taken in the function's body */
    struct recomp_cxx_frame *prev;      /* the enclosing catch point */
    uint32_t func;                      /* guest VA of the function */
    uint32_t entry_esp;                 /* guest esp at entry: its EH record is entry_esp - 0x10 */
    uint32_t ebp;                       /* set by the runtime for the landing */
    uint32_t cont;                      /* the continuation to land at */
} recomp_cxx_frame;

void recomp_cxx_enter(recomp_cxx_frame *f, uint32_t func);
void recomp_cxx_leave(recomp_cxx_frame *f);
/* A landing whose continuation has no label: reports and stops. */
void recomp_cxx_lost(recomp_cxx_frame *f);

#endif /* RECOMP_CXX_H */
