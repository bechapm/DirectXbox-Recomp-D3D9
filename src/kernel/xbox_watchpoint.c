/**
 * Guest memory watchpoints.
 *
 * Every hard bug in this project so far has come down to the same question:
 * *what wrote this?* A structure holds a value it should not, the code walks
 * into it and faults, and the fault tells you where the damage was read, not
 * where it was done. TimeSplitters 2's dark level, Marvel vs Capcom 2's
 * stalled frame loop and Outrun 2's wild pointer were all that shape.
 *
 * There is already a watch of a sort: RECOMP_WATCH_VA in recomp_trace.c polls
 * an address at every function entry. It has two limits that matter. It needs
 * a --trace-all-entries build, which costs a few percent of frame time and
 * has to be lifted specially; and it can only say "it changed somewhere
 * between entering A and entering B", which on a hot path is thousands of
 * instructions of suspects.
 *
 * This is the exact version, and it works in a plain build. It protects the
 * page, lets the write fault, and names the instruction:
 *
 *     RECOMP_WATCH_WRITE=0x001A5F0C:4 titles/outrun2/.../outrun2_recomp.exe
 *
 *     [WATCH] write to 0x001A5F0C from sub_001BB69D+0x2A
 *     [WATCH]   0x00000000 -> 0xF8604020
 *     [WATCH]   guest esp=0x00F7EF2C eax=0xF8604020 ecx=0x001A5F08
 *     [WATCH]   callers: 0x001BC6D2 <- 0x001BD546 <- 0x001BD668
 *
 * How it works: the watched page is made read-only, so a write to it raises
 * an access violation. The handler reports it, restores the page, and sets
 * the processor's trap flag so the very next instruction raises a
 * single-step exception -- at which point the write has happened, the new
 * value can be read, and the page is protected again.
 *
 * Two consequences worth knowing:
 *
 *   - Protection is per page and per process, so a watch on one word traps
 *     every write to the 4 KB around it, and during the one-instruction
 *     window when the page is restored another thread's write to it is
 *     missed. Guest code is cooperatively single-threaded here, so in
 *     practice that window is the vblank thread and the ISR.
 *
 *   - Mirror regions are separate mappings of the same memory, so a write
 *     through a different alias of the same physical page does not trap.
 *     Watch the address the writer actually uses.
 *
 * The companion is RECOMP_FIND_VALUE, which scans guest RAM for a value and
 * prints every address holding it. That is how you get an address to watch
 * when all you have is a bad pointer: find who is holding it, then watch that
 * field and re-run. The two together turn "it crashed on garbage" into a
 * named function in two runs, provided the failure is deterministic.
 */

/* dladdr and Dl_info are GNU extensions in glibc. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "xbox_memory_layout.h"
#include "xbox_watchpoint.h"
#include "platform/fault_emulate.h"

#ifdef _WIN32
#include <dbghelp.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include "win32_compat.h"
#endif

extern uint32_t g_xbox_code_lo, g_xbox_code_hi;

#define MAX_WATCH 8
#define TRAP_FLAG 0x100u

typedef struct {
    uint32_t va;
    uint32_t len;
    uint32_t last;      /* value at the last report, for the -> arrow */
    DWORD    orig_prot; /* the page's own protection before the watch (POSIX) */
} Watch;

static Watch   g_watch[MAX_WATCH];
static int     g_n_watch;
static int     g_watch_reads;       /* trap reads as well as writes */
static long    g_watch_budget = 200;
static int     g_disarmed;
static char    g_arm_on[128];       /* RECOMP_WATCH_ARM_ON, until it fires */
static long    g_arm_on_nth = 1;    /* ...on the Nth matching open ("text#N") */

#ifdef _WIN32
/* Single-step state is per thread: two threads can be mid-step at once, and
 * each has to put back its own page. */
static RECOMP_TLS void    *g_step_page;
static RECOMP_TLS uint32_t g_step_va;
static RECOMP_TLS uint32_t g_step_before;
static RECOMP_TLS int      g_step_pending;
#endif

static uint8_t *guest_base(void)
{
    return (uint8_t *)xbox_GetMemoryOffset();
}

/* The contiguous window (MmAllocateContiguousMemory, 0x80000000) is separate
 * storage from low RAM, mapped at the same guest-to-host offset, so the page
 * arithmetic here holds for it unchanged. It used to be refused as "outside
 * mapped guest RAM", which is where a title's pinned pools and a lot of its
 * streamed data live: TimeSplitters: Future Perfect's cutscene animation
 * records are at 0x813A6580. Kept in step with kernel.h's XBOX_CONTIG_*. */
#define WATCH_CONTIG_BASE 0x80000000u
#define WATCH_CONTIG_SIZE (64u * 1024u * 1024u)

static int guest_in_ram(uint32_t va, uint32_t len)
{
    if ((uint64_t)va + len <= (uint64_t)xbox_GetMappedSize())
        return 1;
    return va >= WATCH_CONTIG_BASE
        && (uint64_t)va + len <= (uint64_t)WATCH_CONTIG_BASE + WATCH_CONTIG_SIZE;
}

static uint32_t guest_read32(uint32_t va)
{
    if (!guest_in_ram(va, 4))
        return 0;
#ifdef _WIN32
    return *(const volatile uint32_t *)(guest_base() + va);
#else
    /* Through the backdoor: this runs in the fault handler, where the page
     * may be the trapped one and a second fault would be fatal. */
    return (uint32_t)recomp_guest_read(NULL, va, 4);
#endif
}

/* The page a guest VA lives on, as a host pointer. */
static void *host_page_of(uint32_t va)
{
    uintptr_t p = (uintptr_t)guest_base() + va;
    return (void *)(p & ~(uintptr_t)0xFFF);
}

static DWORD arm_protection(void)
{
    /* Read-only still allows reads, so a write-only watch leaves the title
     * running at full speed apart from the writes themselves. Watching reads
     * needs the page taken away entirely, which is far more expensive. */
    return g_watch_reads ? PAGE_NOACCESS : PAGE_READONLY;
}

static void protect_one(uint32_t va, uint32_t len, DWORD prot)
{
    uintptr_t first = ((uintptr_t)guest_base() + va) & ~(uintptr_t)0xFFF;
    uintptr_t last  = ((uintptr_t)guest_base() + va + len - 1) & ~(uintptr_t)0xFFF;
    DWORD old = 0;
    VirtualProtect((LPVOID)first, (SIZE_T)(last - first) + 0x1000, prot, &old);
}

static void arm_all(void)
{
    int i;
    for (i = 0; i < g_n_watch; i++)
        protect_one(g_watch[i].va, g_watch[i].len, arm_protection());
}

static void disarm_all(const char *why)
{
    int i;
    for (i = 0; i < g_n_watch; i++)
        protect_one(g_watch[i].va, g_watch[i].len, PAGE_READWRITE);
    g_disarmed = 1;
    fprintf(stderr, "[WATCH] disarmed: %s\n", why);
    fflush(stderr);
}

/* Which watch, if any, covers this guest address. -1 for none. */
static int watch_covering(uint32_t va)
{
    int i;
    for (i = 0; i < g_n_watch; i++)
        if (va >= g_watch[i].va && va < g_watch[i].va + g_watch[i].len)
            return i;
    return -1;
}

/* Whether the faulting address is on a page we protected at all. A write to
 * the other 4088 bytes of a watched page faults too, and has to be let
 * through rather than reported -- but it still needs the same step-over
 * dance, or it would fault forever. */
static int on_watched_page(uint32_t va)
{
    int i;
    void *p = host_page_of(va);
    for (i = 0; i < g_n_watch; i++) {
        uintptr_t first = (uintptr_t)host_page_of(g_watch[i].va);
        uintptr_t last  = (uintptr_t)host_page_of(g_watch[i].va + g_watch[i].len - 1);
        if ((uintptr_t)p >= first && (uintptr_t)p <= last)
            return 1;
    }
    return 0;
}

#ifndef _WIN32
static void print_symbol(uintptr_t pc)
{
    Dl_info info;
    if (dladdr((void *)pc, &info) && info.dli_sname)
        fprintf(stderr, "%s+0x%llX", info.dli_sname,
                (unsigned long long)(pc - (uintptr_t)info.dli_saddr));
    else
        fprintf(stderr, "pc 0x%llX", (unsigned long long)pc);
}
#else
static void print_symbol(uintptr_t rip)
{
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)rip, &disp, sym))
        fprintf(stderr, "%s+0x%llX", sym->Name, (unsigned long long)disp);
    else
        fprintf(stderr, "RIP 0x%llX", (unsigned long long)rip);
}
#endif

/* The guest return-address chain, as the crash handler recovers it: lifted
 * calls push the guest return address, so scanning up from esp for values
 * inside the code sections names the callers. */
static void print_callers(void)
{
    const uint32_t *sp;
    int i, shown = 0;

    if (!g_esp || !guest_in_ram(g_esp, 256 * 4))
        return;
    sp = (const uint32_t *)(guest_base() + g_esp);
    fprintf(stderr, "[WATCH]   callers:");
    for (i = 0; i < 128 && shown < 5; i++) {
        uint32_t v = sp[i];
        if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
            fprintf(stderr, "%s 0x%08X", shown ? " <-" : "", v);
            shown++;
        }
    }
    if (!shown)
        fprintf(stderr, " (none on the stack)");
    fprintf(stderr, "\n");
}

/* RECOMP_WATCH_STACK=<n>: the first n guest stack words at the write.
 *
 * The writer's registers are printed, but its caller's are not, and they
 * are often the question: Max Payne's index copy overran its buffer, and
 * the object that handed out that buffer was only in the copy's saved edi,
 * which a routine pushes on entry and nothing else records. */
static void print_stack(void)
{
    static int n = -1;
    const uint32_t *sp;
    int i;

    if (n < 0) {
        const char *v = getenv("RECOMP_WATCH_STACK");
        n = v ? atoi(v) : 0;
        if (n > 64)
            n = 64;
    }
    if (n <= 0 || !g_esp || !guest_in_ram(g_esp, (uint32_t)n * 4))
        return;
    sp = (const uint32_t *)(guest_base() + g_esp);
    for (i = 0; i < n; i++)
        fprintf(stderr, "%s%08X", (i % 8) ? " " : (i ? "\n[WATCH]   stack: "
                                                      : "[WATCH]   stack: "),
                sp[i]);
    fprintf(stderr, "\n");
}

/* RECOMP_WATCH_POLL=0xADDR[,0xADDR...]: a host thread reads each dword every
 * 50 us and reports the moment it changes, with the last indirect-call
 * targets. It sees a write by any path -- one through a mirror alias, a
 * device writing guest memory through the arena's second view, a page that
 * was re-protected -- which RECOMP_WATCH_WRITE's page trap cannot. It does
 * not name the writer; the report's place in the log says what was going
 * on, and the icall ring narrows it. Found Pro Evolution Soccer 5's heap
 * free-list heads zeroed between swaps 2 and 4 with no trap ever firing. */
#define MAX_POLL 8
static uint32_t g_poll_va[MAX_POLL];
static int      g_n_poll;
/* RECOMP_WATCH_POLL_SNAP=1: spin rather than sleep, and print every
 * thread's stack at each change (macOS). */
static int      g_poll_snap;

extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;

/* The 4 KB around a polled word, so a report can say what else changed
 * with it: the extent and contents of the write name its kind. */
static uint8_t g_poll_page[MAX_POLL][4096];

static void poll_page_diff(int i)
{
    uint32_t page = g_poll_va[i] & ~0xFFFu;
    const uint8_t *now = guest_base() + page;
    uint32_t o = 0;
    int runs = 0;

    while (o < 4096u && runs < 12) {
        uint32_t start, end;
        if (now[o] == g_poll_page[i][o]) { o++; continue; }
        start = o;
        while (o < 4096u && now[o] != g_poll_page[i][o]) o++;
        end = o;
        fprintf(stderr, "[POLL]   0x%08X..0x%08X changed (%u bytes), now",
                page + start, page + end, end - start);
        for (o = start; o < end && o < start + 16u; o++)
            fprintf(stderr, " %02X", now[o]);
        fprintf(stderr, "%s\n", end - start > 16u ? " ..." : "");
        o = end;
        runs++;
    }
    memcpy(g_poll_page[i], now, 4096);
}

static void poll_loop(void)
{
    uint32_t last[MAX_POLL];
    int i, reports = 0;

    for (i = 0; i < g_n_poll; i++) {
        last[i] = guest_read32(g_poll_va[i]);
        memcpy(g_poll_page[i], guest_base() + (g_poll_va[i] & ~0xFFFu), 4096);
    }
    for (;;) {
        for (i = 0; i < g_n_poll; i++) {
            uint32_t now = guest_read32(g_poll_va[i]);
            if (now == last[i])
                continue;
            if (reports++ < 400) {
                uint32_t k, idx = g_icall_trace_idx;
                if (g_poll_snap)
                    xbox_SamplerSnapshotNow();     /* first: the writer moves on */
                fprintf(stderr, "[POLL] 0x%08X: 0x%08X -> 0x%08X; last icalls:",
                        g_poll_va[i], last[i], now);
                for (k = 1; k <= 6; k++)
                    fprintf(stderr, " %08X", g_icall_trace[(idx - k) & 15]);
                fprintf(stderr, "\n");
                poll_page_diff(i);
                fflush(stderr);
            }
            last[i] = now;
        }
#ifdef _WIN32
        Sleep(0);
#else
        if (!g_poll_snap) {
            struct timespec ts = { 0, 50000 };
            nanosleep(&ts, NULL);
        }
#endif
    }
}

#ifdef _WIN32
static DWORD WINAPI poll_thread(LPVOID unused) { (void)unused; poll_loop(); return 0; }
#else
static void *poll_thread(void *unused) { (void)unused; poll_loop(); return NULL; }
#endif

static void poll_init(void)
{
    const char *spec = getenv("RECOMP_WATCH_POLL");
    char *end;

    g_poll_snap = getenv("RECOMP_WATCH_POLL_SNAP") != NULL;

    while (spec && *spec && g_n_poll < MAX_POLL) {
        uint32_t va = (uint32_t)strtoul(spec, &end, 0);
        if (end == spec)
            break;
        if (guest_in_ram(va, 4)) {
            g_poll_va[g_n_poll++] = va;
            fprintf(stderr, "[POLL] polling 0x%08X, currently 0x%08X\n",
                    va, guest_read32(va));
        }
        spec = *end == ',' ? end + 1 : end;
    }
    if (!g_n_poll)
        return;
#ifdef _WIN32
    CloseHandle(CreateThread(NULL, 0, poll_thread, NULL, 0, NULL));
#else
    {
        pthread_t t;
        if (pthread_create(&t, NULL, poll_thread, NULL) == 0)
            pthread_detach(t);
    }
#endif
}

void xbox_watch_init(void)
{
    const char *spec = getenv("RECOMP_WATCH_WRITE");
    const char *p;
    const char *budget = getenv("RECOMP_WATCH_BUDGET");

    if (xbox_GetMemoryOffset())
        poll_init();
    if (!spec || !*spec)
        return;
    if (!xbox_GetMemoryOffset()) {
        fprintf(stderr, "[WATCH] guest memory is not mapped yet; not armed\n");
        return;
    }

    g_watch_reads = getenv("RECOMP_WATCH_READS") != NULL;
    if (budget && *budget)
        g_watch_budget = strtol(budget, NULL, 0);

    /* "0xADDR[:len][,0xADDR[:len]]" -- length defaults to a dword, because
     * the thing being watched is almost always a pointer or a counter. */
    for (p = spec; *p && g_n_watch < MAX_WATCH; ) {
        char *end = NULL;
        uint32_t va = (uint32_t)strtoul(p, &end, 0);
        uint32_t len = 4;
        if (end == p)
            break;
        p = end;
        if (*p == ':') {
            len = (uint32_t)strtoul(p + 1, &end, 0);
            p = end;
            if (len == 0) len = 4;
        }
        if (!guest_in_ram(va, len)) {
            fprintf(stderr, "[WATCH] 0x%08X is outside mapped guest RAM; skipped\n", va);
        } else {
            g_watch[g_n_watch].va = va;
            g_watch[g_n_watch].len = len;
            g_watch[g_n_watch].last = guest_read32(va);
#ifndef _WIN32
            /* What the guest itself asked for on that page, so a watch never
             * lets through an access the page would have refused anyway. */
            g_watch[g_n_watch].orig_prot =
                w32_page_protection(guest_base() + va) & 0xFF;
#endif
            fprintf(stderr, "[WATCH] watching 0x%08X (%u bytes), currently 0x%08X\n",
                    va, len, g_watch[g_n_watch].last);
            g_n_watch++;
        }
        while (*p == ',' || *p == ' ')
            p++;
    }

    if (!g_n_watch)
        return;

    /* Armed late, on a file open, when the page is busy long before the
     * write that matters. Every write to a watched page traps, and on a hot
     * page that slows the title enough to change what it does: Future
     * Perfect's cutscene record at 0x813A6580 shares its page with start-up
     * work, and with the watch armed from boot the scripted presses landed on
     * different screens and the run never reached the cutscene at all. */
    {
        const char *on = getenv("RECOMP_WATCH_ARM_ON");
        if (on && *on) {
            /* "text#N": the Nth open of a matching file. A pack a title
             * opens at boot and again at the moment of interest -- Future
             * Perfect's skelts3.pak, once at start-up and once right after a
             * cutscene's data is read -- can only be named that way. */
            char *hash;
            snprintf(g_arm_on, sizeof g_arm_on, "%s", on);
            hash = strrchr(g_arm_on, '#');
            if (hash) {
                *hash = 0;
                g_arm_on_nth = strtol(hash + 1, NULL, 10);
                if (g_arm_on_nth < 1)
                    g_arm_on_nth = 1;
            }
            fprintf(stderr, "[WATCH] %d watchpoint(s) held until open #%ld "
                    "of a file matching \"%s\"\n", g_n_watch, g_arm_on_nth,
                    g_arm_on);
            fflush(stderr);
            return;
        }
    }

    arm_all();
    fprintf(stderr, "[WATCH] %d watchpoint(s) armed on %s, budget %ld reports\n",
            g_n_watch, g_watch_reads ? "reads and writes" : "writes",
            g_watch_budget);
    fflush(stderr);
}

void xbox_watch_note_path(const char *xbox_path)
{
    if (!g_arm_on[0] || !xbox_path || !strstr(xbox_path, g_arm_on))
        return;
    if (--g_arm_on_nth > 0)
        return;
    g_arm_on[0] = 0;                    /* once */
    arm_all();
    fprintf(stderr, "[WATCH] armed on opening %s: %d watchpoint(s) on %s, "
            "budget %ld reports\n", xbox_path, g_n_watch,
            g_watch_reads ? "reads and writes" : "writes", g_watch_budget);
    fflush(stderr);
}

/* The report for a watched access: who, where, the guest registers and the
 * callers. Shared by the Windows step and the POSIX completion. */
static void report_access(int is_write, uint32_t va, uintptr_t pc, const char *insn)
{
    /* The host thread too: two writers of one structure on different
     * threads is a race the uniprocessor console never had. */
    fprintf(stderr, "[WATCH] %s to 0x%08X (thread %lu) from ",
            is_write ? "write" : "read", va,
            (unsigned long)GetCurrentThreadId());
    print_symbol(pc);
    fprintf(stderr, "\n");
    /* The host bytes of the storing instruction. Lifted code is compiled C,
     * so one guest store can become several host instructions and the
     * symbol offset alone does not say which line did it; the encoding names
     * the operand size and the base register, which does. */
    fprintf(stderr, "[WATCH]   host insn: %s\n", insn);
    fprintf(stderr, "[WATCH]   guest esp=0x%08X eax=0x%08X ecx=0x%08X "
                    "edx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_esp, g_eax, g_ecx, g_edx, g_esi, g_edi);
    print_callers();
    print_stack();
}

#ifdef _WIN32
int xbox_watch_handle_av(PEXCEPTION_POINTERS ep, uintptr_t fault_addr,
                         int is_write)
{
    uint32_t va;
    int idx;

    if (!g_n_watch || g_disarmed || !xbox_GetMemoryOffset())
        return 0;
    if (fault_addr < (uintptr_t)guest_base())
        return 0;
    va = (uint32_t)(fault_addr - (uintptr_t)guest_base());
    if (!on_watched_page(va))
        return 0;

    /* Already stepping on this thread means the step itself faulted on
     * something else. Do not recurse: put the page back and decline. */
    if (g_step_pending) {
        protect_one(g_step_va, 4, arm_protection());
        g_step_pending = 0;
        return 0;
    }

    idx = watch_covering(va);
    if (idx >= 0 && (is_write || g_watch_reads)) {
        if (g_watch_budget-- <= 0) {
            disarm_all("report budget exhausted (RECOMP_WATCH_BUDGET)");
            return 1;
        }
        {
            const uint8_t *ip = (const uint8_t *)ep->ContextRecord->Rip;
            char insn[64];
            int b, used = 0;
            for (b = 0; b < 12; b++)
                used += snprintf(insn + used, sizeof insn - used, "%s%02X",
                                 b ? " " : "", ip[b]);
            report_access(is_write, va, (uintptr_t)ep->ContextRecord->Rip, insn);
        }
        g_step_before = guest_read32(g_watch[idx].va);
        g_step_va = g_watch[idx].va;
    } else {
        /* Collateral: some other address on the same page. Step over it
         * silently. */
        g_step_va = g_watch[0].va;
        g_step_before = 0;
        idx = -1;
    }

    /* Let the access happen, then trap immediately after it so the page can
     * be protected again -- and so the value it wrote can be read. */
    protect_one(g_step_va, 4, PAGE_READWRITE);
    ep->ContextRecord->EFlags |= TRAP_FLAG;
    g_step_pending = idx >= 0 ? 1 : 2;   /* 2 = silent */
    fflush(stderr);
    return 1;
}

int xbox_watch_handle_step(PEXCEPTION_POINTERS ep)
{
    int quiet;

    if (!g_step_pending)
        return 0;

    quiet = g_step_pending == 2;
    g_step_pending = 0;
    ep->ContextRecord->EFlags &= ~TRAP_FLAG;

    if (!quiet) {
        uint32_t now = guest_read32(g_step_va);
        fprintf(stderr, "[WATCH]   0x%08X -> 0x%08X%s\n",
                g_step_before, now,
                now == g_step_before ? "  (unchanged)" : "");
        fflush(stderr);
    }

    if (!g_disarmed)
        protect_one(g_step_va, 4, arm_protection());
    (void)g_step_page;
    return 1;
}

int xbox_watch_handle_fault(recomp_fault *f)
{
    if (f->kind != RECOMP_FAULT_ACCESS || !f->native)
        return 0;
    return xbox_watch_handle_av((PEXCEPTION_POINTERS)f->native, f->host_addr,
                                f->is_write == 1);
}

int xbox_watch_handle_trap(recomp_fault *f)
{
    if (f->kind != RECOMP_FAULT_SINGLE_STEP || !f->native)
        return 0;
    return xbox_watch_handle_step((PEXCEPTION_POINTERS)f->native);
}

#else /* POSIX */

/* An element of a watched access may go ahead if the page allows it -- for a
 * watched page, by the protection it had before the watch made it
 * read-only, so a watch never opens a page the guest had closed. */
static int watch_allows(uintptr_t host, int size, int is_write, void *arg)
{
    uintptr_t p, end = host + (uintptr_t)(size > 0 ? size : 1);
    (void)arg;
    for (p = host & ~(uintptr_t)0xFFF; p < end; p += 0x1000) {
        DWORD prot = w32_page_protection((const void *)p) & 0xFF;
        uint32_t pva = (uint32_t)(p - (uintptr_t)guest_base());
        if (on_watched_page(pva)) {
            /* The watch made it read-only; judge it by what it was before. */
            int i;
            for (i = 0; i < g_n_watch; i++) {
                uint32_t first = g_watch[i].va & ~0xFFFu;
                uint32_t last = (g_watch[i].va + g_watch[i].len - 1) & ~0xFFFu;
                if (pva >= first && pva <= last) {
                    prot = g_watch[i].orig_prot;
                    break;
                }
            }
        }
        if (prot == 0 || prot == PAGE_NOACCESS || prot == PAGE_EXECUTE)
            return 0;
        if (is_write && (prot == PAGE_READONLY || prot == PAGE_EXECUTE_READ))
            return 0;
    }
    return 1;
}

/* The POSIX watch completes the trapped access itself, through the arena's
 * backdoor, instead of opening the page and single-stepping. So there is no
 * window in which the page is writable -- another thread's write cannot slip
 * past -- and nothing depends on a trap flag, which arm64 user space does
 * not have. The before/after values are read either side of it. */
int xbox_watch_handle_fault(recomp_fault *f)
{
    uint32_t va, before = 0;
    int idx, report;
    char insn[64];

    if (f->kind != RECOMP_FAULT_ACCESS || !g_n_watch || g_disarmed
        || !xbox_GetMemoryOffset())
        return 0;
    if (f->host_addr < (uintptr_t)guest_base()
        || f->host_addr - (uintptr_t)guest_base() > 0xFFFFFFFFu)
        return 0;
    va = (uint32_t)(f->host_addr - (uintptr_t)guest_base());
    if (!on_watched_page(va))
        return 0;

    idx = watch_covering(va);
    /* -1 (direction unknown) counts as a write: with reads not watched the
     * page is read-only and only a write can fault on it. */
    report = idx >= 0 && (f->is_write != 0 || g_watch_reads);
    if (report) {
        if (g_watch_budget-- <= 0) {
            disarm_all("report budget exhausted (RECOMP_WATCH_BUDGET)");
            return 1;              /* the page is open again: just retry */
        }
        recomp_fault_insn_text(f, insn, sizeof insn);
        report_access(f->is_write != 0, va, f->pc, insn);
        before = guest_read32(g_watch[idx].va);
    }

    if (!recomp_fault_complete_guest(f, watch_allows, NULL)) {
        recomp_fault_insn_text(f, insn, sizeof insn);
        fprintf(stderr, "[WATCH] could not complete the access to 0x%08X "
                        "(host insn %s); letting it fault\n", va, insn);
        fflush(stderr);
        return 0;
    }

    if (report) {
        uint32_t now = guest_read32(g_watch[idx].va);
        fprintf(stderr, "[WATCH]   0x%08X -> 0x%08X%s\n", before, now,
                now == before ? "  (unchanged)" : "");
    }
    fflush(stderr);
    return 1;
}

int xbox_watch_handle_trap(recomp_fault *f)
{
    (void)f;
    return 0;                      /* nothing here sets a trap flag */
}

#endif /* _WIN32 */

void xbox_watch_scan_value(uint32_t value)
{
    const uint32_t *mem = (const uint32_t *)xbox_GetMemoryOffset();
    size_t words = xbox_GetMappedSize() / 4;

#ifndef _WIN32
    /* The RAM object's backdoor view is the whole of RAM, contiguous and
     * never protected: the scan runs inside the crash handler, where a
     * second fault (RECOMP_TRAP_NULL's page zero, a watched page) would be
     * fatal on POSIX. */
    if (mem && w32_backdoor(mem))
        mem = (const uint32_t *)w32_backdoor(mem);
#endif
    size_t i;
    int hits = 0;

    if (!mem || !words)
        return;

    fprintf(stderr, "[FIND] scanning %llu MB of guest RAM for 0x%08X\n",
            (unsigned long long)(words * 4 / (1024 * 1024)), value);
    for (i = 0; i < words && hits < 64; i++) {
        if (mem[i] == value) {
            fprintf(stderr, "[FIND]   0x%08X holds it\n", (uint32_t)(i * 4));
            hits++;
        }
    }
    if (!hits)
        fprintf(stderr, "[FIND]   nowhere in RAM -- it was computed into a "
                        "register, not loaded from memory\n");
    else if (hits >= 64)
        fprintf(stderr, "[FIND]   ... stopped at 64\n");
    fprintf(stderr, "[FIND] watch one of these with "
                    "RECOMP_WATCH_WRITE=<addr> to see what writes it\n");
    fflush(stderr);
}

void xbox_watch_scan_on_crash(void)
{
    const char *v = getenv("RECOMP_FIND_VALUE");
    if (v && *v)
        xbox_watch_scan_value((uint32_t)strtoul(v, NULL, 0));
}
