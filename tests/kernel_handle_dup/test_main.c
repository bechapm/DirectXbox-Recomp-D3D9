/*
 * kernel_handle_dup - NtDuplicateObject (ordinal 197) and NtClose (187)
 * through the kernel bridge's handle table.
 *
 * A guest handle is a token into a table of host HANDLEs. Off Windows,
 * DuplicateHandle returns the source object itself with its reference count
 * raised, and the table used to hand an already-seen HANDLE its existing
 * token: the guest's "duplicate" was the source's own token, and closing the
 * source freed the slot both of them named. Turok: Evolution duplicates each
 * of its archive handles and closes the original, and every archive then
 * answered STATUS_INVALID_HANDLE.
 *
 * Checked, on whichever file backend this host builds:
 *   - the duplicate is a token of its own, not the source's;
 *   - it carries the source's per-token flags (asynchronous I/O, no
 *     intermediate buffering), which decide how reads and writes on it go;
 *   - closing the source leaves the duplicate resolving and readable;
 *   - closing the duplicate too frees both slots, and nothing holds the
 *     host handle any more;
 *   - a duplicate of an untagged handle (a pseudo handle) still gets a
 *     token, with no flags.
 *
 * Includes src/kernel/kernel_bridge.c for its private table and bridges, as
 * kernel_directory does with kernel_file.c. Guest memory is a plain buffer
 * (g_xbox_mem_offset points at it), so no memory layout and no game files;
 * one temporary file, removed afterwards.
 */
#include "kernel_bridge.c"   /* the bridges and their handle table */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int failures;
#define CHECK(name, cond) \
    do { if (cond) {} else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } } while (0)

/* A small guest address space: the stack the bridges read their arguments
 * from, and a slot for the handle NtDuplicateObject writes back. */
static uint8_t s_guest[0x10000];
#define GUEST_STACK  0x8000u
#define GUEST_OUT    0x9000u

#define DUP_SAME_ACCESS 0x2u     /* NtDuplicateObject's Options */

static void call3(void (*bridge)(void), uint32_t a0, uint32_t a1, uint32_t a2)
{
    g_esp = GUEST_STACK;
    BRIDGE_MEM32(GUEST_STACK + 0) = a0;
    BRIDGE_MEM32(GUEST_STACK + 4) = a1;
    BRIDGE_MEM32(GUEST_STACK + 8) = a2;
    g_eax = 0xFFFFFFFFu;
    bridge();
}

static uint32_t duplicate(uint32_t token)
{
    BRIDGE_MEM32(GUEST_OUT) = 0;
    call3(bridge_NtDuplicateObject, token, GUEST_OUT, DUP_SAME_ACCESS);
    return BRIDGE_MEM32(GUEST_OUT);
}

static void nt_close(uint32_t token)
{
    call3(bridge_NtClose, token, 0, 0);
}

static int tagged(uint32_t token)
{
    return (token & 0xFF000000u) == BRIDGE_HANDLE_TAG;
}

/* Read the file's first bytes through a token's host handle. */
static int reads_back(uint32_t token, const char *want)
{
    char buf[16] = {0};
    DWORD got = 0;
    HANDLE h = bridge_resolve_handle(token);
    LARGE_INTEGER zero;

    if (!h || h == INVALID_HANDLE_VALUE)
        return 0;

    zero.QuadPart = 0;
    SetFilePointerEx(h, zero, NULL, FILE_BEGIN);
    if (!ReadFile(h, buf, (DWORD)strlen(want), &got, NULL))
        return 0;
    return got == strlen(want) && memcmp(buf, want, got) == 0;
}

int main(void)
{
    char path[512];
    FILE *f;
    HANDLE h;
    uint32_t src, dup;

    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)s_guest;

#if defined(_WIN32)
    {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        snprintf(path, sizeof path, "%sxboxrecomp-dup-%lu.bin", tmp,
                 (unsigned long)GetCurrentProcessId());
    }
#else
    snprintf(path, sizeof path, "/tmp/xboxrecomp-dup-%ld.bin", (long)getpid());
#endif
    f = fopen(path, "wb");
    if (!f) {
        printf("cannot create %s\n", path);
        return 2;
    }
    fputs("archive", f);
    fclose(f);

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("cannot open %s\n", path);
        remove(path);
        return 2;
    }

    /* The source, as NtCreateFile would leave it: a token, and the flags
     * its CreateOptions asked for. */
    src = bridge_handle_token(h);
    CHECK("source is a token", tagged(src));
    s_handle_async[src & BRIDGE_HANDLE_MASK] = 1;
    s_handle_nobuf[src & BRIDGE_HANDLE_MASK] = 1;

    dup = duplicate(src);
    CHECK("NtDuplicateObject succeeds", g_eax == 0);
    CHECK("the duplicate is a token", tagged(dup));
    CHECK("the duplicate is not the source's token", dup != src);
    CHECK("the duplicate keeps asynchronous I/O", bridge_handle_is_async(dup));
    CHECK("the duplicate keeps no intermediate buffering", bridge_handle_is_nobuf(dup));
    CHECK("the duplicate reads the file", reads_back(dup, "archive"));

    /* What Turok does: close the original, keep the duplicate. */
    nt_close(src);
    CHECK("NtClose on the source succeeds", g_eax == 0);
    CHECK("the source no longer resolves", bridge_resolve_handle(src) == NULL);
    CHECK("the duplicate still resolves", bridge_resolve_handle(dup) != NULL);
    CHECK("the duplicate still reads the file after the source is closed",
          reads_back(dup, "archive"));
    CHECK("the duplicate keeps its flags after the source is closed",
          bridge_handle_is_async(dup) && bridge_handle_is_nobuf(dup));

    {
        HANDLE last = bridge_resolve_handle(dup);
        nt_close(dup);
        CHECK("the duplicate no longer resolves once closed",
              bridge_resolve_handle(dup) == NULL);
        CHECK("nothing holds the host handle once both are closed",
              !bridge_handle_held(last));
    }

    /* A pseudo handle is not in the table; its duplicate is, with nothing
     * carried over from a slot that does not exist. NtCurrentThread() is
     * -2, which X-Men Legends' CRT duplicates. */
    {
        uint32_t t = duplicate(0xFFFFFFFEu);
        CHECK("a pseudo handle's duplicate succeeds", g_eax == 0);
        CHECK("a pseudo handle's duplicate is a token", tagged(t));
        CHECK("a pseudo handle's duplicate has no file flags",
              !bridge_handle_is_async(t) && !bridge_handle_is_nobuf(t));
        nt_close(t);
    }

    remove(path);
    if (failures) {
        printf("kernel_handle_dup: %d failure(s)\n", failures);
        return 1;
    }
    printf("kernel_handle_dup: all checks passed\n");
    return 0;
}
