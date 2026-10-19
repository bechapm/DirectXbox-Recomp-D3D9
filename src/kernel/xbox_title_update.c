/**
 * Title updates, as a console runs them.
 *
 * A title update is installed by the dashboard's online updater into
 * E:\TDATA\<title id>\$u. From then on the console starts $u\default.xbe in
 * place of the disc's, with the disc still as D:. Here the update's XBE is
 * lifted into a build of its own (a title project's second executable), so
 * "starts it in place of the disc's" means: the disc's build, finding the
 * update installed and the update's build beside it, hands over to that.
 *
 *   title_update_exe = <path>              [RECOMP_TITLE_UPDATE_EXE]
 *       the update's build, when it is not beside this executable
 *
 * The disc's XAPI itself launches $u\default.xbe at start-up when it is
 * there, so the hand-off is what a console does; the runtime makes it before
 * the disc's code runs at all, and maps that launch the same way.
 *
 * The way there needs no settings either. A title that finds an update
 * required launches the dashboard's update.xbe (XOnlineTitleUpdate); the
 * recompiled updater, xodash_update_recomp, is used when it is beside this
 * executable, or in an updater folder beside it (with the console's C: drive
 * as its game folder), or where the updater setting says, with this title's
 * game folder as its D:. When it is done it launches the title again, and
 * that goes back to the executable that started it. launch_map, where set,
 * wins over both.
 *
 *   updater = <path>                               [RECOMP_UPDATER]
 */
#include "kernel.h"
#include "platform/host_main.h"
#include "recomp_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define TU_EXE_SUFFIX ".exe"
#else
#define TU_EXE_SUFFIX ""
#endif

/* The executable starting a launch, so the image it starts can come back:
 * "<title id>=<exe>|<game folder>". Set before each hand-off. */
#define RETURN_ENV "RECOMP_RETURN_TO"

static void tu_setenv(const char *name, const char *value)
{
#ifdef _WIN32
    /* Both: the process's block is what CreateProcess passes on, the CRT's
     * copy is what getenv (and recomp_config_lookup) reads in this one. */
    _putenv_s(name, value ? value : "");    /* "" removes it */
    SetEnvironmentVariableA(name, value);
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

/* The E: folder as the path rules have it: hdd_dir, else the game folder. */
static int tu_hdd_dir(const char *game_dir, char *out, size_t n)
{
    const char *hdd = recomp_config_lookup("RECOMP_HDD_DIR", "hdd_dir");
    if (hdd && *hdd)
        return snprintf(out, n, "%s", hdd) < (int)n;
    return game_dir && *game_dir && snprintf(out, n, "%s", game_dir) < (int)n;
}

int xbox_title_update_xbe(uint32_t title_id, const char *game_dir, char *out, size_t n)
{
    char hdd[1024];
    int upper;

    if (!out || n == 0)
        return 0;
    out[0] = 0;
    if (!tu_hdd_dir(game_dir, hdd, sizeof hdd))
        return 0;
    /* The updater writes the ID as the title asked for it; a case-sensitive
     * disk may have either. */
    for (upper = 0; upper < 2; upper++) {
        snprintf(out, n, upper ? "%s/TDATA/%08X/$u/default.xbe" : "%s/TDATA/%08x/$u/default.xbe",
                 hdd, title_id);
        if (host_file_exists(out))
            return 1;
    }
    out[0] = 0;
    return 0;
}

/* <dir of this executable>/<name><suffix>, if it exists. */
static int tu_beside_me(const char *name, char *out, size_t n)
{
    char dir[1024];

    if (!host_exe_path(dir, sizeof dir))
        return 0;
    host_path_dirname(dir);
    if (snprintf(out, n, "%s/%s%s", dir, name, TU_EXE_SUFFIX) >= (int)n)
        return 0;
    return host_file_exists(out);
}

/* path ends in tail, ignoring case (ASCII). */
static int tu_ends_with(const char *path, const char *tail)
{
    size_t p = strlen(path), t = strlen(tail), i;

    if (p < t)
        return 0;
    for (i = 0; i < t; i++) {
        char a = path[p - t + i], b = tail[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return 1;
}

/* The update's build, as the title named it at start-up (handoff). */
static char s_update_exe[256];

static int tu_update_build(char *out, size_t n)
{
    const char *named = recomp_config_lookup("RECOMP_TITLE_UPDATE_EXE", "title_update_exe");

    if (named && *named)
        return snprintf(out, n, "%s", named) < (int)n && host_file_exists(out);
    return s_update_exe[0] && tu_beside_me(s_update_exe, out, n);
}

void xbox_title_update_handoff(uint32_t title_id, const char *game_dir, const char *update_exe)
{
    char xbe[1024], exe[1024];

    if (update_exe)
        snprintf(s_update_exe, sizeof s_update_exe, "%s", update_exe);
    if (!xbox_title_update_xbe(title_id, game_dir, xbe, sizeof xbe))
        return;
    if (!tu_update_build(exe, sizeof exe)) {
        /* The disc's XAPI will launch $u\default.xbe in a moment, as on a
         * console, and with nothing to start the title ends: say why. */
        static char message[1600];
        snprintf(message, sizeof message,
                 "The title update is installed (%s), but the program built from it "
                 "(%s%s) is not beside this one. Build it, then start this again.",
                 xbe, s_update_exe[0] ? s_update_exe : "the update's build", TU_EXE_SUFFIX);
        fprintf(stderr, "[UPDATE] %s\n", message);
        fflush(stderr);
        host_error_box("Title update", message);
        return;
    }
    fprintf(stderr, "[UPDATE] title update installed: starting %s, as the console starts"
                    " $u\\default.xbe\n", exe);
    fflush(stderr);
    tu_setenv("RECOMP_GAME_DIR", game_dir);
    tu_setenv("RECOMP_TITLE_UPDATE_XBE", xbe);
    host_launch_other(exe);
    fprintf(stderr, "[UPDATE] could not start %s: running the disc's\n", exe);
}

int xbox_launch_default(const char *xbox_path, uint32_t title_id, char *out, size_t n)
{
    char game[1024], exe[1024];

    if (!out || n == 0)
        return 0;
    out[0] = 0;
    if (!xbox_path)
        xbox_path = "";
    if (!xbox_path_game_dir_utf8(game, sizeof game))
        game[0] = 0;

    /* The dashboard's online updater. */
    if (tu_ends_with(xbox_path, "update.xbe")) {
        const char *named = recomp_config_lookup("RECOMP_UPDATER", "updater");
        if (named && *named)
            snprintf(exe, sizeof exe, "%s", named);
        else if (!tu_beside_me("xodash_update_recomp", exe, sizeof exe) &&
                 !tu_beside_me("updater/xodash_update_recomp", exe, sizeof exe))
            return 0;
        return snprintf(out, n, game[0] ? "%s|%s" : "%s", exe, game) < (int)n;
    }

    /* This title's own update, as its XAPI launches it once installed. */
    if (title_id == recomp_config_title_id() && tu_ends_with(xbox_path, "\\$u\\default.xbe")) {
        if (!tu_update_build(exe, sizeof exe))
            return 0;
        return snprintf(out, n, game[0] ? "%s|%s" : "%s", exe, game) < (int)n;
    }

    /* Back to the executable that launched this one. */
    {
        const char *back = getenv(RETURN_ENV);
        if (back && strlen(back) > 9 && back[8] == '=' &&
                (uint32_t)strtoul(back, NULL, 16) == title_id)
            return snprintf(out, n, "%s", back + 9) < (int)n;
    }
    return 0;
}

/* What the launched image must see the same way: one console, one disk,
 * one network. A setting file is per title, so what this one has from its
 * own file goes along in the environment, where the next one looks first. */
static void tu_carry(const char *env, const char *key)
{
    const char *v;

    if (getenv(env) && *getenv(env))
        return;
    v = recomp_config_lookup(env, key);
    if (v && *v)
        tu_setenv(env, v);
}

void xbox_launch_note_return(void)
{
    static char value[2200];
    char exe[1024], game[1024];
    const char *overlay = xbox_path_disc_overlay();

    tu_carry("RECOMP_HDD_DIR", "hdd_dir");
    tu_carry("RECOMP_CONSOLE_DIR", "console_dir");
    tu_carry("RECOMP_SYSLINK", "syslink");
    if (overlay && !(getenv("RECOMP_DISC_OVERLAY") && *getenv("RECOMP_DISC_OVERLAY")))
        tu_setenv("RECOMP_DISC_OVERLAY", overlay);

    if (!host_exe_path(exe, sizeof exe))
        return;
    if (!xbox_path_game_dir_utf8(game, sizeof game))
        game[0] = 0;
    snprintf(value, sizeof value, "%08X=%s%s%s", recomp_config_title_id(), exe,
             game[0] ? "|" : "", game);
    tu_setenv(RETURN_ENV, value);
}
