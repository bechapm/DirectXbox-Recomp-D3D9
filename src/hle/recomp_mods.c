/*
 * recomp_mods.c - loads the mods beside the game (recomp_mod.h) and hands
 * them the toolkit's side of the contract.
 *
 * Built into the game's HLE and into d3d8_replay alike: what it needs of
 * the title it is handed (RecompModsHost), so nothing here reaches guest
 * memory or the kernel.
 */
#include "recomp_mod.h"
#include "recomp_mods.h"

#include "platform/xbox_winnt.h"
#include "platform/host_main.h"          /* host_exe_path, host_path_dirname */
#include "rhi_d3d9.h"                     /* the D3D9 renderer's hooks */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dirent.h>
#  include <dlfcn.h>
#endif

#define MAX_MODS        16
#define MAX_ON_FRAME    16
#define MAX_ON_CAPTURE  8
#define MAX_ON_NOTE     8

typedef void (*NoteFn)(uint32_t tag, const void *data, uint32_t bytes, void *user);

static struct { void (*fn)(void *user); void *user; } g_on_frame[MAX_ON_FRAME],
                                                       g_on_capture[MAX_ON_CAPTURE];
static struct { NoteFn fn; void *user; } g_on_note[MAX_ON_NOTE];
static int g_on_frame_n, g_on_capture_n, g_on_note_n;
static int g_loaded;                 /* mods kept */
static RecompModsHost g_host;

/* ---- the api ---------------------------------------------------------------- */

static uint32_t api_title_id(void)
{
    return g_host.title_id ? g_host.title_id() : 0;
}

static const char *api_title_name(void)
{
    return g_host.title_name ? g_host.title_name() : NULL;
}

static void *api_guest_pointer(uint32_t va)
{
    return g_host.guest_pointer ? g_host.guest_pointer(va) : NULL;
}

static int api_on_frame(void (*fn)(void *user), void *user)
{
    if (!fn || g_on_frame_n >= MAX_ON_FRAME)
        return 0;
    g_on_frame[g_on_frame_n].fn = fn;
    g_on_frame[g_on_frame_n].user = user;
    g_on_frame_n++;
    return 1;
}

static uint32_t api_vertex_constants(uint32_t first, uint32_t count, float (*out)[4])
{
    return g_host.vertex_constants ? g_host.vertex_constants(first, count, out) : 0;
}

static void api_log(const char *line)
{
    fprintf(stderr, "[MOD] %s\n", line ? line : "");
    fflush(stderr);
}

static int api_replaying(void)
{
    return g_host.replaying;
}

static int api_capture_on_draw(void (*fn)(void *user), void *user)
{
    if (!fn || g_on_capture_n >= MAX_ON_CAPTURE)
        return 0;
    g_on_capture[g_on_capture_n].fn = fn;
    g_on_capture[g_on_capture_n].user = user;
    g_on_capture_n++;
    return 1;
}

static void api_capture_note(uint32_t tag, const void *data, uint32_t bytes)
{
    if (g_host.capture_note)
        g_host.capture_note(tag, data, bytes);
}

static int api_replay_on_note(NoteFn fn, void *user)
{
    if (!fn || g_on_note_n >= MAX_ON_NOTE)
        return 0;
    g_on_note[g_on_note_n].fn = fn;
    g_on_note[g_on_note_n].user = user;
    g_on_note_n++;
    return 1;
}

/* Named, so that the order of the struct is the header's business alone. */
static const RecompModApi g_api = {
    .version                      = RECOMP_MOD_API_VERSION,
    .size                         = sizeof(RecompModApi),
    .title_id                     = api_title_id,
    .title_name                   = api_title_name,
    .guest_pointer                = api_guest_pointer,
    .on_frame                     = api_on_frame,
    .vertex_constants             = api_vertex_constants,
    .log                          = api_log,
    .d3d9_on_device               = rhi_d3d9_mod_on_device,
    .d3d9_on_draw                 = rhi_d3d9_mod_on_draw,
    .d3d9_on_clear                = rhi_d3d9_mod_on_clear,
    .d3d9_on_present              = rhi_d3d9_mod_on_present,
    .d3d9_vertex_hlsl             = rhi_d3d9_mod_vertex_hlsl,
    .d3d9_render_state            = rhi_d3d9_mod_render_state,
    .d3d9_set_projection          = rhi_d3d9_mod_set_projection,
    .d3d9_remake_written_textures = rhi_d3d9_mod_remake_written_textures,
    .d3d9_set_transform           = rhi_d3d9_mod_set_transform,
    .replaying                    = api_replaying,
    .capture_on_draw              = api_capture_on_draw,
    .capture_note                 = api_capture_note,
    .replay_on_note               = api_replay_on_note,
    .d3d9_on_copy                 = rhi_d3d9_mod_on_copy,
};

/* ---- loading ---------------------------------------------------------------- */

static int mods_dir(char *out, size_t n)
{
    const char *named = getenv("RECOMP_MOD_LIBS_DIR");

    if (named && *named) {
        snprintf(out, n, "%s", named);
        return 1;
    }
    if (!host_exe_path(out, n))
        return 0;
    host_path_dirname(out);
    if (strlen(out) + 6 >= n)
        return 0;
    strcat(out, "/mods");
    return 1;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* One library: kept if its recomp_mod_init says so. */
static void load_one(const char *dir, const char *name)
{
    char path[1024];
    RecompModInitFn init;
    int keep;

    snprintf(path, sizeof path, "%s/%s", dir, name);
#if defined(_WIN32)
    {
        HMODULE m = LoadLibraryA(path);

        if (!m) {
            fprintf(stderr, "[MOD] %s: could not load (%lu)\n", name, GetLastError());
            return;
        }
        init = (RecompModInitFn)(void (*)(void))GetProcAddress(m, "recomp_mod_init");
        if (!init) {
            fprintf(stderr, "[MOD] %s: no recomp_mod_init, not a mod\n", name);
            FreeLibrary(m);
            return;
        }
        keep = init(&g_api);
        if (!keep)
            FreeLibrary(m);
    }
#else
    {
        void *m = dlopen(path, RTLD_NOW | RTLD_LOCAL);

        if (!m) {
            fprintf(stderr, "[MOD] %s: could not load (%s)\n", name, dlerror());
            return;
        }
        init = (RecompModInitFn)dlsym(m, "recomp_mod_init");
        if (!init) {
            fprintf(stderr, "[MOD] %s: no recomp_mod_init, not a mod\n", name);
            dlclose(m);
            return;
        }
        keep = init(&g_api);
        if (!keep)
            dlclose(m);
    }
#endif
    fprintf(stderr, "[MOD] %s: %s\n", name, keep ? "loaded" : "not for this title, unloaded");
    if (keep)
        g_loaded++;
}

void recomp_mods_load(const RecompModsHost *host)
{
    static int done;
    char dir[1024], *names[MAX_MODS];
    int n = 0, i;

    if (done)
        return;
    done = 1;
    if (host)
        g_host = *host;
    {
        const char *e = getenv("RECOMP_MOD_LIBS");
        if (e && e[0] == '0')
            return;
    }
    if (!mods_dir(dir, sizeof dir))
        return;
#if defined(_WIN32)
    {
        char pattern[1100];
        WIN32_FIND_DATAA fd;
        HANDLE h;

        snprintf(pattern, sizeof pattern, "%s/*.dll", dir);
        h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE)
            return;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n < MAX_MODS)
                names[n++] = _strdup(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR *d = opendir(dir);
        struct dirent *e;

        if (!d)
            return;
        while ((e = readdir(d)) != NULL && n < MAX_MODS) {
            size_t len = strlen(e->d_name);

            if (len > 3 && strcmp(e->d_name + len - 3, ".so") == 0)
                names[n++] = strdup(e->d_name);
        }
        closedir(d);
    }
#endif
    qsort(names, (size_t)n, sizeof names[0], by_name);
    for (i = 0; i < n; i++) {
        if (names[i])
            load_one(dir, names[i]);
        free(names[i]);
    }
    if (n)
        fprintf(stderr, "[MOD] %d of %d in %s loaded\n", g_loaded, n, dir);
}

void recomp_mods_frame(void)
{
    int i;

    for (i = 0; i < g_on_frame_n; i++)
        g_on_frame[i].fn(g_on_frame[i].user);
}

void recomp_mods_capture_draw(void)
{
    int i;

    for (i = 0; i < g_on_capture_n; i++)
        g_on_capture[i].fn(g_on_capture[i].user);
}

void recomp_mods_replay_note(uint32_t tag, const void *data, uint32_t bytes)
{
    int i;

    for (i = 0; i < g_on_note_n; i++)
        g_on_note[i].fn(tag, data, bytes, g_on_note[i].user);
}
