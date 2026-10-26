/*
 * android_title.c - the title as a library of its own, loaded at start.
 *
 * On Android the title's code is compiled on the player's device from their
 * own disc (android_build.c), so the APK's libmain.so is the engine alone and
 * the title is libgame.so, in the app's internal folder, loaded before it
 * runs. libgame.so's calls into the engine resolve against libmain.so as it
 * loads. The engine's few references to the title cannot -- Android binds
 * every symbol when a library loads, and libgame.so is not there yet -- so in
 * this build (XBOXRECOMP_SPLIT_TITLE) they go through slots filled here:
 *
 *   hle_original_<name>, hle_var_<name>  slots the HLE macros register
 *                                        (src/hle/hle.h), set from the
 *                                        title's definitions of the same name
 *   recomp_lookup, recomp_lookup_manual  defined here, calling the title's
 *   a weak engine default the title       asked for by name where it is used
 *   overrides (recomp_xbe_patches)        (recomp_title_symbol)
 *
 * Nothing in the lifter's output changes for this; the title library defines
 * everything it always did.
 */
#if defined(__ANDROID__) && defined(XBOXRECOMP_SPLIT_TITLE)

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void (*recomp_func_t)(void);

typedef struct {
    const char *symbol;
    void       *slot;
    int         is_var;
} split_slot;

#define MAX_SLOTS 1024
static split_slot g_slots[MAX_SLOTS];
static int        g_slot_count;

void hle_split_note(const char *symbol, void *slot, int is_var)
{
    int i;

    for (i = 0; i < g_slot_count; i++)
        if (g_slots[i].slot == slot)
            return;                 /* one name declared in two files */
    if (g_slot_count < MAX_SLOTS) {
        g_slots[g_slot_count].symbol = symbol;
        g_slots[g_slot_count].slot = slot;
        g_slots[g_slot_count].is_var = is_var;
        g_slot_count++;
    }
}

static void *g_title;            /* the loaded title library */

/* A symbol of the title library by name, for an engine default the title
 * overrides (a weak definition, such as recomp_xbe_patches): in one library
 * the title's wins at link time, here it is not the one the engine linked.
 * NULL before the title is loaded or when it defines none. */
void *recomp_title_symbol(const char *name)
{
    return g_title ? dlsym(g_title, name) : NULL;
}

static recomp_func_t (*g_lookup)(uint32_t);
static recomp_func_t (*g_lookup_manual)(uint32_t);

recomp_func_t recomp_lookup(uint32_t xbox_va)
{
    return g_lookup ? g_lookup(xbox_va) : NULL;
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    return g_lookup_manual ? g_lookup_manual(xbox_va) : NULL;
}

/* Loads the title library and binds the engine to it. Returns the title's
 * main (its main.c's main(), compiled as recomp_title_main), or NULL having
 * said why. */
int (*android_title_load(const char *path))(int, char **)
{
    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    int i, bound = 0, missing = 0;
    int (*title_main)(int, char **);

    if (!h) {
        fprintf(stderr, "[TITLE] cannot load %s: %s\n", path, dlerror());
        return NULL;
    }
    g_title = h;
    /* dlsym on the handle looks in the title library first, so a name the
     * engine also defines (its own slot) is found as the title's. */
    for (i = 0; i < g_slot_count; i++) {
        void *def = dlsym(h, g_slots[i].symbol);
        if (!def || def == g_slots[i].slot) {
            missing++;
            continue;
        }
        if (g_slots[i].is_var)
            *(uint32_t *)g_slots[i].slot = *(const uint32_t *)def;
        else
            *(void **)g_slots[i].slot = *(void *const *)def;
        bound++;
    }
    g_lookup = (recomp_func_t (*)(uint32_t))dlsym(h, "recomp_lookup");
    g_lookup_manual = (recomp_func_t (*)(uint32_t))dlsym(h, "recomp_lookup_manual");
    title_main = (int (*)(int, char **))dlsym(h, "recomp_title_main");
    fprintf(stderr, "[TITLE] %s: %d of %d engine slots bound (%d the title does not define); "
            "lookup %s, manual lookup %s\n", path, bound, g_slot_count, missing,
            g_lookup ? "bound" : "MISSING", g_lookup_manual ? "bound" : "MISSING");
    if (!title_main || !g_lookup)
        fprintf(stderr, "[TITLE] %s has no %s: not a title library\n", path,
                !title_main ? "recomp_title_main" : "recomp_lookup");
    return g_lookup ? title_main : NULL;
}

#endif
