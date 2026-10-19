/*
 * fault_win32.c - recomp_fault.h on Windows: a vectored exception handler.
 *
 * This is the handler every title's main.c used to carry (veh_handler),
 * with the device routing and the report moved behind the two callbacks.
 * The order is the one it had, which matters:
 *
 *   1. route first. A watchpoint stepping over the instruction it just
 *      trapped arrives as a single-step exception this process asked for,
 *      and an armed watchpoint or a trapped device register as an access
 *      violation; both are serviced and resumed, never reported.
 *   2. Breakpoints and the debugger's thread-naming exception (0x406D1388)
 *      are passed on silently: not worth a line.
 *   3. Everything else is reported, then passed on, so the process dies --
 *      or a debugger stops -- exactly as it would with no handler. A fault
 *      this handler stayed silent on read as the process simply vanishing:
 *      exit code 0xC0000005 and nothing in the log.
 */
#ifdef _WIN32

#include "recomp_fault.h"

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MS_VC_THREAD_NAME_EXCEPTION 0x406D1388u

static int kind_of(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:     return RECOMP_FAULT_ACCESS;
    case EXCEPTION_SINGLE_STEP:          return RECOMP_FAULT_SINGLE_STEP;
    case EXCEPTION_BREAKPOINT:           return RECOMP_FAULT_BREAKPOINT;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:     return RECOMP_FAULT_ILLEGAL;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:   return RECOMP_FAULT_INT_DIVIDE;
    case EXCEPTION_INT_OVERFLOW:         return RECOMP_FAULT_INT_OVERFLOW;
    default:                             return RECOMP_FAULT_OTHER;
    }
}

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    recomp_fault_route_fn route = recomp_fault_route_cb();
    recomp_crash_fn crash = recomp_fault_crash_cb();
    recomp_fault f;

    f.kind      = kind_of(code);
    f.code      = (uint32_t)code;
    f.pc        = (uintptr_t)ep->ContextRecord->Rip;
    f.sp        = (uintptr_t)ep->ContextRecord->Rsp;
    f.native    = ep;
    f.ctx       = ep->ContextRecord;
    f.host_addr = 0;
    f.is_write  = -1;
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        f.host_addr = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        f.is_write  = ep->ExceptionRecord->ExceptionInformation[0] == 1;
    }

    if (route && route(&f))
        return EXCEPTION_CONTINUE_EXECUTION;

    if (code == EXCEPTION_BREAKPOINT || code == MS_VC_THREAD_NAME_EXCEPTION)
        return EXCEPTION_CONTINUE_SEARCH;

    if (crash)
        crash(&f);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

void recomp_fault_install(recomp_fault_route_fn route, recomp_crash_fn crash)
{
    recomp_fault_set_cbs(route, crash);
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the report prints no names. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);

    /* RECOMP_WINDOW_BACKGROUND=1 is a run nobody is watching. A fault passed
     * on would end in Windows Error Reporting's "has stopped working" box,
     * which takes the focus from whoever is at the desk and holds the process
     * open until it is answered. The report above has already been printed. */
    {
        const char *bg = getenv("RECOMP_WINDOW_BACKGROUND");

        if (bg && *bg && strcmp(bg, "0") != 0)
            SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS |
                         SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    }
}

void recomp_fault_thread_init(void)
{
}

#endif /* _WIN32 */
