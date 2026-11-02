/*
 * kernel_file_relative - names relative to an open directory (RootDirectory)
 * in NtDeleteFile and NtQueryFullAttributesFile (src/kernel/kernel_file.c).
 *
 * XAPI's save code opens a save folder and then reaches each file in it by
 * its bare name relative to that handle: XDeleteSaveGame deletes every child
 * that way. NtCreateFile and the rename resolved RootDirectory on both
 * backends, but on POSIX the delete and the attribute query sent the bare
 * name through the guest path table, which cannot place it, so a save's
 * files were never deleted and never found (upstream #237, bertibertig, on
 * SSX 3). Checked, on whichever backend this host builds:
 *   - a relative attribute query finds the file and reports its size;
 *   - a relative query in another case finds it too (ext4 is case-sensitive,
 *     the Xbox is not; on a case-insensitive disk this passes regardless);
 *   - a relative delete removes the file, and a missing one says "not found";
 *   - a full guest path with no root still works for both calls.
 *
 * No game files; a temporary game folder and save folder, removed
 * afterwards.
 */
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#define SEP "\\"
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define SEP "/"
#endif

void *recomp_lookup(ULONG address) { (void)address; return NULL; }
void *recomp_lookup_manual(ULONG address) { (void)address; return NULL; }

static int failures;
#define CHECK(name, cond) \
    do { if (cond) {} else { printf("FAIL: %s\n", name); failures++; } } while (0)
#define CHECK_ST(name, got, want) \
    do { NTSTATUS g_ = (got), w_ = (want); if (g_ != w_) { \
        printf("FAIL: %s (status 0x%08X, want 0x%08X)\n", name, \
               (unsigned)g_, (unsigned)w_); failures++; } } while (0)

static char s_game[512], s_save[512];

/* ---- host helpers --------------------------------------------------------- */

static void host_join(char *out, size_t n, const char *dir, const char *rel)
{
    snprintf(out, n, "%s" SEP "%s", dir, rel);
}

static int host_exists(const char *dir, const char *rel)
{
    char p[1024];
    host_join(p, sizeof p, dir, rel);
#if defined(_WIN32)
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(p, &st) == 0;
#endif
}

static void host_write(const char *dir, const char *rel, const char *text)
{
    char p[1024];
    FILE *f;
    host_join(p, sizeof p, dir, rel);
    f = fopen(p, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static void host_mkdir(const char *dir, const char *rel)
{
    char p[1024];
    host_join(p, sizeof p, dir, rel);
#if defined(_WIN32)
    CreateDirectoryA(p, NULL);
#else
    mkdir(p, 0755);
#endif
}

static void host_rmtree(const char *path)
{
#if defined(_WIN32)
    char pat[1024], child[1024];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*", path);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
                continue;
            snprintf(child, sizeof child, "%s\\%s", path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                host_rmtree(child);
            else {
                SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(child);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(path);
#else
    char child[1024];
    struct dirent *e;
    struct stat st;
    DIR *d = opendir(path);
    if (d) {
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
                host_rmtree(child);
            else
                unlink(child);
        }
        closedir(d);
    }
    rmdir(path);
#endif
}

static int make_temp_dirs(void)
{
#if defined(_WIN32)
    char tmp[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, tmp))
        return 0;
    snprintf(s_game, sizeof s_game, "%sxboxrecomp-relative-%lu", tmp,
             (unsigned long)GetCurrentProcessId());
    snprintf(s_save, sizeof s_save, "%s-save", s_game);
    return CreateDirectoryA(s_game, NULL) && CreateDirectoryA(s_save, NULL);
#else
    char tmpl[] = "/tmp/xboxrecomp-relative-XXXXXX";
    if (!mkdtemp(tmpl))
        return 0;
    snprintf(s_game, sizeof s_game, "%s", tmpl);
    snprintf(s_save, sizeof s_save, "%s-save", s_game);
    return mkdir(s_save, 0755) == 0;
#endif
}

/* ---- guest-side helpers --------------------------------------------------- */

static void make_oa(XBOX_OBJECT_ATTRIBUTES *oa, XBOX_ANSI_STRING *s,
                    const char *path, HANDLE root)
{
    s->Buffer = (PCHAR)path;
    s->Length = (USHORT)strlen(path);
    s->MaximumLength = (USHORT)(s->Length + 1);
    oa->RootDirectory = root;
    oa->ObjectName = s;
    oa->Attributes = 0;
}

static NTSTATUS open_dir(const char *path, HANDLE *out)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING s;
    XBOX_IO_STATUS_BLOCK ios;
    make_oa(&oa, &s, path, NULL);
    memset(&ios, 0, sizeof ios);
    *out = NULL;
    return xbox_NtCreateFile(out, XBOX_GENERIC_READ | XBOX_SYNCHRONIZE, &oa, &ios,
                             NULL, 0, 0x3 /* share read|write */, XBOX_FILE_OPEN,
                             XBOX_FILE_DIRECTORY_FILE | XBOX_FILE_SYNCHRONOUS_IO_NONALERT);
}

static NTSTATUS query(const char *name, HANDLE root, XBOX_FILE_NETWORK_OPEN_INFORMATION *info)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING s;
    make_oa(&oa, &s, name, root);
    memset(info, 0, sizeof *info);
    return xbox_NtQueryFullAttributesFile(&oa, info);
}

static NTSTATUS delete_file(const char *name, HANDLE root)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING s;
    make_oa(&oa, &s, name, root);
    return xbox_NtDeleteFile(&oa);
}

int main(void)
{
    XBOX_FILE_NETWORK_OPEN_INFORMATION info;
    HANDLE dir = NULL;

    printf("kernel_file_relative: running\n");
    if (!make_temp_dirs()) {
        printf("FAIL: could not make temporary folders\n");
        return 2;
    }
    xbox_path_init(s_game, s_save);
    host_mkdir(s_game, "saves");
    host_mkdir(s_game, "saves" SEP "slot1");
    host_write(s_game, "saves" SEP "slot1" SEP "data.bin", "0123456789");
    host_write(s_game, "saves" SEP "slot1" SEP "Meta.xbx", "meta");
    host_write(s_game, "saves" SEP "slot1" SEP "old.bin", "old");

    CHECK_ST("open D:\\saves\\slot1", open_dir("D:\\saves\\slot1", &dir), STATUS_SUCCESS);
    if (!dir || dir == INVALID_HANDLE_VALUE) {
        printf("FAIL: no directory handle, nothing more to check\n");
        host_rmtree(s_game); host_rmtree(s_save);
        return 1;
    }

    CHECK_ST("query data.bin relative to the folder", query("data.bin", dir, &info),
             STATUS_SUCCESS);
    CHECK("its size is the file's", info.EndOfFile.QuadPart == 10);
    CHECK_ST("query META.XBX in another case", query("META.XBX", dir, &info),
             STATUS_SUCCESS);
    CHECK_ST("query a missing name", query("nothing.bin", dir, &info),
             STATUS_OBJECT_NAME_NOT_FOUND);

    CHECK_ST("delete data.bin relative to the folder", delete_file("data.bin", dir),
             STATUS_SUCCESS);
    CHECK("data.bin is gone", !host_exists(s_game, "saves" SEP "slot1" SEP "data.bin"));
    CHECK("Meta.xbx is still there", host_exists(s_game, "saves" SEP "slot1" SEP "Meta.xbx"));
    CHECK_ST("delete a missing name", delete_file("data.bin", dir),
             STATUS_OBJECT_NAME_NOT_FOUND);

    /* A full guest path, no root: the path table, as before. */
    CHECK_ST("query D:\\saves\\slot1\\Meta.xbx", query("D:\\saves\\slot1\\Meta.xbx", NULL, &info),
             STATUS_SUCCESS);
    CHECK_ST("delete D:\\saves\\slot1\\old.bin", delete_file("D:\\saves\\slot1\\old.bin", NULL),
             STATUS_SUCCESS);
    CHECK("old.bin is gone", !host_exists(s_game, "saves" SEP "slot1" SEP "old.bin"));

    xbox_NtClose(dir);
    host_rmtree(s_game);
    host_rmtree(s_save);

    printf("kernel_file_relative: %s (%d failure%s)\n", failures ? "FAILED" : "OK",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
