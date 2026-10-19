/*
 * Title updates and the launch defaults (src/kernel/xbox_title_update.c):
 * where the updater is found, the way back to the title that launched it,
 * the settings that go along, and the installed update's XBE. The host and
 * the settings are stubbed here, so nothing on this machine is read.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "kernel.h"

/* The module looks for executables by name, with Windows' .exe. */
#ifdef _WIN32
#define X ".exe"
#else
#define X ""
#endif

/* ---- stubs ------------------------------------------------------------ */

static const char *s_exe = "/play/rs3_recomp";
static const char *s_game = "/repo/game";
static const char *s_overlay;
static const char *s_files[8];
static const char *s_settings[8][2];     /* key, value: the title's file */
static uint32_t s_title = 0x55530013u;

int host_exe_path(char *buf, size_t bytes)
{
    return snprintf(buf, bytes, "%s", s_exe) < (int)bytes;
}

void host_path_dirname(char *path)
{
    char *slash = strrchr(path, '/');
    if (slash)
        *slash = '\0';
}

int host_file_exists(const char *path)
{
    size_t i;
    for (i = 0; i < sizeof s_files / sizeof s_files[0]; i++)
        if (s_files[i] && strcmp(s_files[i], path) == 0)
            return 1;
    return 0;
}

static char s_launched[512];
static int s_boxes;

int host_launch_other(const char *exe)
{
    snprintf(s_launched, sizeof s_launched, "%s", exe);
    return -1;                                  /* "could not start": back here */
}

void host_error_box(const char *title, const char *message)
{
    (void)title; (void)message;
    s_boxes++;
}

int xbox_path_game_dir_utf8(char *out, size_t n)
{
    return snprintf(out, n, "%s", s_game) < (int)n;
}

const char *xbox_path_disc_overlay(void)
{
    return s_overlay;
}

const char *recomp_config_lookup(const char *env_name, const char *key)
{
    const char *v = env_name ? getenv(env_name) : NULL;
    size_t i;
    if (v && *v)
        return v;
    for (i = 0; i < sizeof s_settings / sizeof s_settings[0]; i++)
        if (s_settings[i][0] && strcmp(s_settings[i][0], key) == 0)
            return s_settings[i][1];
    return NULL;
}

uint32_t recomp_config_title_id(void)
{
    return s_title;
}

/* ---- the test ----------------------------------------------------------- */

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static void set_env(const char *k, const char *v)
{
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
    SetEnvironmentVariableA(k, v);
#else
    if (v)
        setenv(k, v, 1);
    else
        unsetenv(k);
#endif
}

static void reset(void)
{
    memset(s_files, 0, sizeof s_files);
    memset(s_settings, 0, sizeof s_settings);
    s_overlay = NULL;
    s_launched[0] = 0;
    s_boxes = 0;
    set_env("RECOMP_RETURN_TO", NULL);
    set_env("RECOMP_UPDATER", NULL);
    set_env("RECOMP_HDD_DIR", NULL);
    set_env("RECOMP_CONSOLE_DIR", NULL);
    set_env("RECOMP_SYSLINK", NULL);
    set_env("RECOMP_DISC_OVERLAY", NULL);
    set_env("RECOMP_TITLE_UPDATE_EXE", NULL);
}

int main(void)
{
    char out[2048];
    const char *v;

    /* The updater, from updater/ beside the game's executable, with the
     * game's folder as its D:. */
    reset();
    s_files[0] = "/play/updater/xodash_update_recomp" X;
    CHECK(xbox_launch_default("\\Device\\CdRom0;update.xbe", 0x55530013u, out, sizeof out),
          "updater in updater/ not found");
    CHECK(strcmp(out, "/play/updater/xodash_update_recomp" X "|/repo/game") == 0, "got %s", out);

    /* Beside the executable wins over updater/. */
    s_files[1] = "/play/xodash_update_recomp" X;
    CHECK(xbox_launch_default("D:\\UPDATE.XBE", 0x55530013u, out, sizeof out), "beside");
    CHECK(strcmp(out, "/play/xodash_update_recomp" X "|/repo/game") == 0, "got %s", out);

    /* The updater setting wins over both. */
    s_settings[0][0] = "updater"; s_settings[0][1] = "/elsewhere/upd";
    CHECK(xbox_launch_default("\\Device\\CdRom0;update.xbe", 0x55530013u, out, sizeof out), "setting");
    CHECK(strcmp(out, "/elsewhere/upd|/repo/game") == 0, "got %s", out);

    /* No updater anywhere: nothing. */
    reset();
    CHECK(!xbox_launch_default("\\Device\\CdRom0;update.xbe", 0x55530013u, out, sizeof out),
          "found an updater that is not there: %s", out);

    /* Another XBE is not the updater. */
    s_files[0] = "/play/xodash_update_recomp" X;
    CHECK(!xbox_launch_default("\\Device\\CdRom0;downloader.xbe", 0x55530013u, out, sizeof out),
          "downloader.xbe taken for the updater: %s", out);

    /* The way back: the launching title notes itself; the launched image,
     * asked for that title, goes back to it. */
    reset();
    xbox_launch_note_return();
    v = getenv("RECOMP_RETURN_TO");
    CHECK(v && strcmp(v, "55530013=/play/rs3_recomp|/repo/game") == 0, "noted %s", v ? v : "(none)");
    s_title = 0xFFFE0000u;                      /* now in the updater */
    CHECK(xbox_launch_default("", 0x55530013u, out, sizeof out), "no way back");
    CHECK(strcmp(out, "/play/rs3_recomp|/repo/game") == 0, "got %s", out);
    CHECK(!xbox_launch_default("", 0x4D530004u, out, sizeof out), "back to the wrong title: %s", out);
    s_title = 0x55530013u;

    /* What goes along: from the title's settings file, and the overlay as
     * found; the environment, where set, is left as it is. */
    reset();
    s_settings[0][0] = "hdd_dir"; s_settings[0][1] = "/e";
    s_settings[1][0] = "syslink"; s_settings[1][1] = "nat";
    s_overlay = "/play/disc_overlay";
    set_env("RECOMP_CONSOLE_DIR", "/mine");
    xbox_launch_note_return();
    v = getenv("RECOMP_HDD_DIR");
    CHECK(v && strcmp(v, "/e") == 0, "hdd_dir: %s", v ? v : "(none)");
    v = getenv("RECOMP_SYSLINK");
    CHECK(v && strcmp(v, "nat") == 0, "syslink: %s", v ? v : "(none)");
    v = getenv("RECOMP_DISC_OVERLAY");
    CHECK(v && strcmp(v, "/play/disc_overlay") == 0, "overlay: %s", v ? v : "(none)");
    v = getenv("RECOMP_CONSOLE_DIR");
    CHECK(v && strcmp(v, "/mine") == 0, "console_dir: %s", v ? v : "(none)");

    /* The installed update: E: is hdd_dir, else the game folder. */
    reset();
    CHECK(!xbox_title_update_xbe(0x55530013u, "/repo/game", out, sizeof out), "none installed");
    s_files[0] = "/repo/game/TDATA/55530013/$u/default.xbe";
    CHECK(xbox_title_update_xbe(0x55530013u, "/repo/game", out, sizeof out), "in the game folder");
    CHECK(strcmp(out, s_files[0]) == 0, "got %s", out);
    s_settings[0][0] = "hdd_dir"; s_settings[0][1] = "/e";
    CHECK(!xbox_title_update_xbe(0x55530013u, "/repo/game", out, sizeof out),
          "hdd_dir set, but found in the game folder: %s", out);
    s_files[1] = "/e/TDATA/4D530004/$u/default.xbe";       /* upper-case hex */
    CHECK(xbox_title_update_xbe(0x4D530004u, "/repo/game", out, sizeof out), "upper case");

    /* The disc's build at start-up: nothing installed, nothing happens. */
    reset();
    xbox_title_update_handoff(0x55530013u, "/repo/game", "rs3_tu501_recomp");
    CHECK(!s_launched[0] && !s_boxes, "acted with no update installed");

    /* Installed, but its build not made: said, not started. */
    s_files[0] = "/repo/game/TDATA/55530013/$u/default.xbe";
    xbox_title_update_handoff(0x55530013u, "/repo/game", "rs3_tu501_recomp");
    CHECK(!s_launched[0] && s_boxes == 1, "launched %s, %d boxes", s_launched, s_boxes);
    CHECK(!xbox_launch_default("\\Device\\Harddisk0\\Partition1\\TDATA\\55530013\\$u\\default.xbe",
                               0x55530013u, out, sizeof out), "mapped $u with no build: %s", out);

    /* And with it beside: started, with the game folder and the XBE. */
    s_files[1] = "/play/rs3_tu501_recomp" X;
    xbox_title_update_handoff(0x55530013u, "/repo/game", "rs3_tu501_recomp");
    CHECK(strcmp(s_launched, "/play/rs3_tu501_recomp" X) == 0, "launched %s", s_launched);
    v = getenv("RECOMP_TITLE_UPDATE_XBE");
    CHECK(v && strcmp(v, s_files[0]) == 0, "update XBE %s", v ? v : "(none)");
    v = getenv("RECOMP_GAME_DIR");
    CHECK(v && strcmp(v, "/repo/game") == 0, "game folder %s", v ? v : "(none)");

    /* The disc's XAPI launching $u maps to the same build; another title's
     * $u does not. */
    CHECK(xbox_launch_default("\\Device\\Harddisk0\\Partition1\\TDATA\\55530013\\$u\\default.xbe",
                              0x55530013u, out, sizeof out), "$u not mapped");
    CHECK(strcmp(out, "/play/rs3_tu501_recomp" X "|/repo/game") == 0, "got %s", out);
    CHECK(!xbox_launch_default("\\Device\\Harddisk0\\Partition1\\TDATA\\4D530004\\$u\\default.xbe",
                               0x4D530004u, out, sizeof out), "another title's $u mapped: %s", out);

    if (failures) {
        printf("%d failed\n", failures);
        return 1;
    }
    printf("title_update: all passed\n");
    return 0;
}
