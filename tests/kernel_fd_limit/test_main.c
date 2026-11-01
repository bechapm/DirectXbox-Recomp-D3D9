/*
 * kernel_fd_limit - the POSIX open-file limit (after upstream #263, apexJCL).
 *
 * The console's kernel has no per-process file limit, and a title may create
 * its Z: cache on first boot and keep one handle per file open. Under the
 * soft limit of 256 that macOS gives an app started from Finder, BLiNX 2's
 * 247th open failed with EMFILE and the HLE answered "not found". Checked:
 *   - w32_raise_fd_limit lifts a 256 soft limit past a full cache, and a
 *     second call has nothing left to do;
 *   - the cache's open, close, reopen and keep pattern then opens every
 *     file, with one host fd per kept handle and all of them given back;
 *   - an exhausted limit answers STATUS_TOO_MANY_OPENED_FILES, in the return
 *     value and the IO_STATUS_BLOCK, not "not found".
 *
 * No game files; a temporary game folder and save folder, removed
 * afterwards. POSIX only: Windows has no such limit to raise.
 */
#include "kernel.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>

void *recomp_lookup(ULONG address) { (void)address; return NULL; }
void *recomp_lookup_manual(ULONG address) { (void)address; return NULL; }

#define CACHE_FILES 400   /* more than a full cache, well past 256 */

static int failures;
#define CHECK(name, cond) \
    do { if (cond) {} else { printf("FAIL: %s\n", name); failures++; } } while (0)
#define CHECK_ST(name, got, want) \
    do { NTSTATUS g_ = (got), w_ = (want); if (g_ != w_) { \
        printf("FAIL: %s (status 0x%08X, want 0x%08X)\n", name, \
               (unsigned)g_, (unsigned)w_); failures++; } } while (0)

/* Host fds open in this process. */
static int count_fds(void)
{
    struct rlimit rl;
    int n = 0, top;
    getrlimit(RLIMIT_NOFILE, &rl);
    top = rl.rlim_cur > 8192 ? 8192 : (int)rl.rlim_cur;
    for (int fd = 0; fd < top; fd++)
        if (fcntl(fd, F_GETFD) != -1) n++;
    return n;
}

/* D:\cache\cache_NNN.rec, read-write, FILE_OPEN_IF, as the title opens it. */
static NTSTATUS open_cache(int i, HANDLE *h, NTSTATUS *ios_status)
{
    char path[64];
    XBOX_ANSI_STRING s;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    NTSTATUS st;
    snprintf(path, sizeof path, "D:\\cache\\cache_%03d.rec", i);
    s.Buffer = path;
    s.Length = (USHORT)strlen(path);
    s.MaximumLength = (USHORT)(s.Length + 1);
    oa.RootDirectory = NULL;
    oa.ObjectName = &s;
    oa.Attributes = 0;
    memset(&ios, 0, sizeof ios);
    *h = NULL;
    st = xbox_NtCreateFile(h, XBOX_GENERIC_READ | XBOX_GENERIC_WRITE | XBOX_SYNCHRONIZE,
                           &oa, &ios, NULL, 0, 0x3 /* share read|write */,
                           XBOX_FILE_OPEN_IF,
                           XBOX_FILE_NON_DIRECTORY_FILE | XBOX_FILE_SYNCHRONOUS_IO_NONALERT);
    if (ios_status) *ios_status = ios.Status;
    return st;
}

int main(void)
{
    static HANDLE kept[CACHE_FILES];
    char game[] = "/tmp/xboxrecomp-fdlimit-XXXXXX";
    char save[64], cache[96];
    struct rlimit rl, saved;
    unsigned long long was = 0, now = 0;
    NTSTATUS st = STATUS_SUCCESS, ios_st = 0;
    int base_fds, opened = 0, enough;

    printf("kernel_fd_limit: running\n");
    if (!mkdtemp(game)) {
        printf("FAIL: could not make a temporary folder\n");
        return 2;
    }
    snprintf(save, sizeof save, "%s-save", game);
    snprintf(cache, sizeof cache, "%s/cache", game);
    mkdir(save, 0755);
    mkdir(cache, 0755);
    xbox_path_init(game, save);

    /* The 256 soft limit is raised. */
    getrlimit(RLIMIT_NOFILE, &saved);
    rl = saved;
    rl.rlim_cur = 256;
    enough = rl.rlim_max == RLIM_INFINITY || rl.rlim_max >= CACHE_FILES + 64;
    if (!enough) {
        printf("  hard limit %llu is below what the test needs: raise and "
               "cache checks skipped\n", (unsigned long long)rl.rlim_max);
    } else {
        int rc;
        CHECK("soft limit set to 256", setrlimit(RLIMIT_NOFILE, &rl) == 0);
        rc = w32_raise_fd_limit(&was, &now);
        printf("  raised %llu -> %llu\n", was, now);
        CHECK("w32_raise_fd_limit lifts 256 past a full cache",
              rc == 1 && was == 256 && now >= CACHE_FILES + 64);
        CHECK("a second call has nothing left to do",
              w32_raise_fd_limit(&was, &now) == 0 && was == now);
    }

    /* The cache pattern: open and close to size it, then reopen and keep. */
    base_fds = count_fds();
    for (int i = 0; enough && i < CACHE_FILES; i++) {
        HANDLE probe;
        if (open_cache(i, &probe, NULL) != STATUS_SUCCESS) break;
        xbox_NtClose(probe);
        if (open_cache(i, &kept[i], NULL) != STATUS_SUCCESS) break;
        opened++;
    }
    if (enough) {
        CHECK("every cache file opened", opened == CACHE_FILES);
        CHECK("one host fd per kept handle, none per closed one",
              count_fds() - base_fds == opened);
        for (int i = 0; i < opened; i++)
            xbox_NtClose(kept[i]);
        CHECK("NtClose gives every fd back", count_fds() == base_fds);
    }

    /* An exhausted limit. */
    rl = saved;
    rl.rlim_cur = (rlim_t)(count_fds() + 8);
    CHECK("soft limit set just above the open fds", setrlimit(RLIMIT_NOFILE, &rl) == 0);
    opened = 0;
    for (int i = 0; i < 64 && st == STATUS_SUCCESS; i++) {
        st = open_cache(i, &kept[i], &ios_st);
        if (st == STATUS_SUCCESS) opened++;
    }
    CHECK_ST("the failed open says TOO_MANY_OPENED_FILES", st, STATUS_TOO_MANY_OPENED_FILES);
    CHECK_ST("and so does its IO_STATUS_BLOCK", ios_st, STATUS_TOO_MANY_OPENED_FILES);
    for (int i = 0; i < opened; i++)
        xbox_NtClose(kept[i]);
    setrlimit(RLIMIT_NOFILE, &saved);

    for (int i = 0; i < CACHE_FILES; i++) {
        char p[160];
        snprintf(p, sizeof p, "%s/cache_%03d.rec", cache, i);
        unlink(p);
    }
    rmdir(cache);
    rmdir(save);
    rmdir(game);

    printf("kernel_fd_limit: %s (%d failure%s)\n", failures ? "FAILED" : "OK",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
