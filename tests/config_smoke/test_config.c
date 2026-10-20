/*
 * test_config -- the settings file, parsed without a game.
 *
 * Covers the things that decide whether a player's choice actually
 * reaches the runtime: that a value is found, that the environment beats
 * the file, that a boolean reads the way the rest of the runtime reads
 * booleans, and that rubbish falls back instead of failing.
 *
 * And what the one widescreen switch resolves to: for each kind of title,
 * with and without the settings a person can still set by hand, and for
 * the files the two-switch launcher left behind, which must go on meaning
 * what they meant.
 */
#include "recomp_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("  FAIL: %s\n", what);
        failures++;
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    int ok = (got && want) ? strcmp(got, want) == 0 : got == want;

    if (!ok) {
        printf("  FAIL: %s (got %s, want %s)\n", what,
               got ? got : "(null)", want ? want : "(null)");
        failures++;
    }
}

static void check_int(int got, int want, const char *what)
{
    if (got != want) {
        printf("  FAIL: %s (got %d, want %d)\n", what, got, want);
        failures++;
    }
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");

    if (!f) {
        printf("  FAIL: cannot write %s\n", path);
        failures++;
        return;
    }
    fputs(text, f);
    fclose(f);
}

static void check_near(double got, double want, const char *what)
{
    if (got < want - 1e-6 || got > want + 1e-6) {
        printf("  FAIL: %s (got %g, want %g)\n", what, got, want);
        failures++;
    }
}

/* NULL unsets. */
static void set_env(const char *name, const char *value)
{
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");     /* empty removes it, on Windows */
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

static int file_has_line(const char *path, const char *line)
{
    char buf[512];
    FILE *f = fopen(path, "rb");
    int found = 0;

    if (!f)
        return 0;
    while (!found && fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\r\n")] = '\0';
        found = strcmp(buf, line) == 0;
    }
    fclose(f);
    return found;
}

/* What one widescreen switch resolves to, from its inputs as text. Each
 * row is a title (what its project said), a player (widescreen on or off)
 * and whatever was set by hand, and what must come out. */
static void test_widescreen_resolution(void)
{
    static const struct {
        const char    *what;
        int            on;
        const char    *hor_plus, *reg;
        RecompWideMode mode;
        int            title_reg;
        double         want_factor;
        int            want_reg, want_named;
    } rows[] = {
        /* The switch alone, which is all a player now has. */
        { "off, unknown title",        0, NULL, NULL, RECOMP_WIDE_UNKNOWN,  -1, 1.0,  60, 0 },
        { "off, Hor+ title",           0, NULL, NULL, RECOMP_WIDE_HOR_PLUS, 60, 1.0,  60, 0 },
        { "on, title with a 16:9 mode", 1, NULL, NULL, RECOMP_WIDE_NATIVE,  -1, 1.0,  60, 0 },
        { "on, Hor+ title",            1, NULL, NULL, RECOMP_WIDE_HOR_PLUS, -1, 0.75, 60, 0 },
        /* The default for a title nobody has described: the flag and 16:9
         * only, never a guess at a register. */
        { "on, unknown title",         1, NULL, NULL, RECOMP_WIDE_UNKNOWN,  -1, 1.0,  60, 0 },

        /* The register is the title's when it names one, either mode. */
        { "on, Hor+ title naming c[96]",   1, NULL, NULL, RECOMP_WIDE_HOR_PLUS, 96,  0.75, 96,  0 },
        { "on, native title naming c[160]", 1, NULL, NULL, RECOMP_WIDE_NATIVE,  160, 1.0,  160, 0 },
        { "a register out of range is not one", 1, NULL, NULL, RECOMP_WIDE_HOR_PLUS, 192, 0.75, 60, 0 },

        /* "auto" is the same as saying nothing, in either setting. */
        { "auto, Hor+ title",          1, "auto", "auto", RECOMP_WIDE_HOR_PLUS, 96, 0.75, 96, 0 },
        { "AUTO, native title",        1, "AUTO", NULL,   RECOMP_WIDE_NATIVE,   -1, 1.0,  60, 0 },

        /* A number set by hand decides, whatever the title is... */
        { "0 leaves a Hor+ title's camera alone", 1, "0", NULL, RECOMP_WIDE_HOR_PLUS, 60, 1.0, 60, 1 },
        { "0.75 widens an unknown title",  1, "0.75", NULL, RECOMP_WIDE_UNKNOWN,  -1, 0.75, 60, 1 },
        { "0.75 widens a native title too", 1, "0.75", NULL, RECOMP_WIDE_NATIVE,  -1, 0.75, 60, 1 },
        { "a factor of one's own",     1, "0.8",  NULL, RECOMP_WIDE_HOR_PLUS, 60, 0.8,  60, 1 },
        /* ...and with widescreen off, as RECOMP_HOR_PLUS always has. */
        { "0.75 with widescreen off",  0, "0.75", NULL, RECOMP_WIDE_UNKNOWN,  -1, 0.75, 60, 1 },
        /* Not a usable factor: the camera is left alone, as before. */
        { "a negative factor",         1, "-1",   NULL, RECOMP_WIDE_HOR_PLUS, 60, 1.0,  60, 1 },
        { "a factor past 4",           1, "9",    NULL, RECOMP_WIDE_HOR_PLUS, 60, 1.0,  60, 1 },
        { "a word that is no factor",  1, "wide", NULL, RECOMP_WIDE_HOR_PLUS, 60, 1.0,  60, 1 },

        /* A register set by hand beats the title's; one that is not a
         * register does not. */
        { "register by hand",          1, NULL, "160", RECOMP_WIDE_HOR_PLUS, 96, 0.75, 160, 0 },
        { "register 0 is a register",  1, NULL, "0",   RECOMP_WIDE_HOR_PLUS, 96, 0.75, 0,   0 },
        { "register past the bank",    1, NULL, "192", RECOMP_WIDE_HOR_PLUS, 96, 0.75, 96,  0 },
        { "register that is a word",   1, NULL, "sixty", RECOMP_WIDE_UNKNOWN, -1, 1.0, 60,  0 },
    };
    size_t i;

    for (i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        RecompWidescreen w;
        char what[160];

        recomp_widescreen_resolve_from(rows[i].on, rows[i].hor_plus, rows[i].reg,
                                       rows[i].mode, rows[i].title_reg, &w);
        snprintf(what, sizeof what, "resolve: %s: on", rows[i].what);
        check_int(w.on, rows[i].on, what);
        snprintf(what, sizeof what, "resolve: %s: factor", rows[i].what);
        check_near(w.hor_plus, rows[i].want_factor, what);
        snprintf(what, sizeof what, "resolve: %s: register", rows[i].what);
        check_int(w.projection_register, rows[i].want_reg, what);
        snprintf(what, sizeof what, "resolve: %s: factor named", rows[i].what);
        check_int(w.hor_plus_named, rows[i].want_named, what);
    }

    {
        RecompWidescreen w;

        recomp_widescreen_resolve_from(1, NULL, "160", RECOMP_WIDE_HOR_PLUS, 96, &w);
        check_int(w.register_named, 1, "resolve: a register by hand is reported as named");
        check_int(w.title_register, 96, "resolve: and the title's is still reported");
        recomp_widescreen_resolve_from(1, NULL, NULL, RECOMP_WIDE_HOR_PLUS, 96, &w);
        check_int(w.register_named, 0, "resolve: the title's register is not 'named'");
    }
}

/* What the guest is told: 16:9 only for a title with a mode of its own.
 * A Hor+ title is told 4:3, so it draws its whole frame for the runtime to
 * widen, instead of fitting a 4:3 picture into a 16:9 one itself. */
static void test_guest_flag(void)
{
    check_int(recomp_widescreen_guest_flag_from(0, RECOMP_WIDE_NATIVE), 0,
              "guest flag: off");
    check_int(recomp_widescreen_guest_flag_from(1, RECOMP_WIDE_NATIVE), 1,
              "guest flag: on, title with a 16:9 mode");
    check_int(recomp_widescreen_guest_flag_from(1, RECOMP_WIDE_UNKNOWN), 1,
              "guest flag: on, title nobody described (resolved as native)");
    check_int(recomp_widescreen_guest_flag_from(1, RECOMP_WIDE_HOR_PLUS), 0,
              "guest flag: on, Hor+ title is told 4:3");
    check_int(recomp_widescreen_guest_flag_from(0, RECOMP_WIDE_HOR_PLUS), 0,
              "guest flag: off, Hor+ title");
}

/* The settings files already out there, written by the launcher that had
 * two switches: `widescreen` 0 or 1, `hor_plus` 0 or 0.75, the register
 * always 60. Each must mean what it meant, on every kind of title, both
 * read as it stands and after the new launcher has saved it untouched. */
static void test_older_files(void)
{
    static const struct { int wide; const char *hor_plus; double factor; } files[] = {
        { 0, "0",    1.0  },
        { 1, "0",    1.0  },    /* widescreen, camera left alone */
        { 1, "0.75", 0.75 },    /* both switches on */
        { 0, "0.75", 0.75 },    /* wide camera alone */
    };
    static const RecompWideMode modes[] = {
        RECOMP_WIDE_UNKNOWN, RECOMP_WIDE_NATIVE, RECOMP_WIDE_HOR_PLUS
    };
    const char *path = "test_config_older.conf";
    size_t i, m;

    for (i = 0; i < sizeof files / sizeof files[0]; i++) {
        char text[160], what[160], saved_hp[32];
        RecompSettings s;

        snprintf(text, sizeof text,
                 "widescreen = %d\nhor_plus = %s\nhor_plus_register = 60\n",
                 files[i].wide, files[i].hor_plus);
        write_file(path, text);

        for (m = 0; m < sizeof modes / sizeof modes[0]; m++) {
            RecompWidescreen w;

            recomp_widescreen_resolve_from(files[i].wide, files[i].hor_plus, "60",
                                           modes[m], -1, &w);
            snprintf(what, sizeof what, "older file %d/%s, title mode %d: factor",
                     files[i].wide, files[i].hor_plus, (int)modes[m]);
            check_near(w.hor_plus, files[i].factor, what);
            check_int(w.projection_register, 60, "older file: register 60");
        }

        /* Through the launcher with the switch left where it was. */
        check(recomp_settings_read(path, &s) != 0, "older file read");
        check_int(s.widescreen, files[i].wide, "older file: widescreen as written");
        check_near(s.hor_plus, atof(files[i].hor_plus), "older file: hor_plus as written");
        check_int(s.hor_plus_register, 60, "older file: register as written");
        recomp_settings_set_widescreen(&s, files[i].wide);
        check_near(s.hor_plus, atof(files[i].hor_plus),
                   "switch not moved: hor_plus is kept");
        check_int(s.hor_plus_register, RECOMP_REGISTER_AUTO,
                  "the old default register is handed back to the title");
        check(recomp_settings_write(path, &s) != 0, "older file saved");
        snprintf(saved_hp, sizeof saved_hp, "hor_plus = %s", files[i].hor_plus);
        check(file_has_line(path, saved_hp), "saved file keeps its hor_plus line");
        check(file_has_line(path, "# hor_plus_register = auto"),
              "saved file leaves the register to the title");
    }

    /* The switch moved: the player is asking, so the title decides. */
    {
        RecompSettings s;

        write_file(path, "widescreen = 0\nhor_plus = 0\nhor_plus_register = 60\n");
        check(recomp_settings_read(path, &s) != 0, "both-off file read");
        recomp_settings_set_widescreen(&s, 1);
        check_int(s.widescreen, 1, "switch turned on");
        check_near(s.hor_plus, RECOMP_HOR_PLUS_AUTO, "turned on: hor_plus goes to auto");

        write_file(path, "widescreen = 1\nhor_plus = 0.75\nhor_plus_register = 160\n");
        check(recomp_settings_read(path, &s) != 0, "both-on file read");
        recomp_settings_set_widescreen(&s, 0);
        check_int(s.widescreen, 0, "switch turned off");
        check_near(s.hor_plus, RECOMP_HOR_PLUS_AUTO,
                   "turned off: no factor is left behind to widen a 4:3 picture");
        check_int(s.hor_plus_register, 160, "a register set by hand is kept either way");
    }
    remove(path);
}

int main(void)
{
    const char *path = "test_config_tmp.conf";

    write_file(path,
        "# a comment\n"
        "; another\n"
        "\n"
        "resolution_scale = 2\n"
        "widescreen = 1\n"
        "hor_plus=0.75\n"                 /* no spaces */
        "   anisotropy   =   16   \n"     /* lots of spaces */
        "frame_cap = adaptive\n"
        "empty =\n"
        "nonsense line with no equals\n"
        "unknown_key = 7\n");

    /* The named file wins outright, which is how the test reaches the
     * parser without a per-user directory or a title id. */
#if defined(_WIN32)
    _putenv_s("RECOMP_DISPLAY_CONFIG", path);
#else
    setenv("RECOMP_DISPLAY_CONFIG", path, 1);
#endif

    printf("test_config\n");

    check_str(recomp_config_lookup(NULL, "resolution_scale"), "2", "plain value");
    check_str(recomp_config_lookup(NULL, "hor_plus"), "0.75", "no spaces around =");
    check_str(recomp_config_lookup(NULL, "anisotropy"), "16", "spaces trimmed");
    check_str(recomp_config_lookup(NULL, "frame_cap"), "adaptive", "word value");
    check_str(recomp_config_lookup(NULL, "empty"), NULL, "empty value reads as absent");
    check_str(recomp_config_lookup(NULL, "missing"), NULL, "absent key");
    check(recomp_config_path() != NULL, "path reported");

    check_int(recomp_config_int(NULL, "resolution_scale", 1), 2, "int");
    check_int(recomp_config_int(NULL, "frame_cap", 99), 99, "int fallback on a word");
    check_int(recomp_config_int(NULL, "missing", 5), 5, "int fallback when absent");
    check(recomp_config_float(NULL, "hor_plus", 0.0) > 0.74 &&
          recomp_config_float(NULL, "hor_plus", 0.0) < 0.76, "float");

    /* The boolean is the one that decides widescreen, so it gets the
     * whole vocabulary the rest of the runtime accepts. */
    check_int(recomp_config_bool(NULL, "widescreen", 0), 1, "bool 1 from file");
    check_int(recomp_config_bool(NULL, "missing", 0), 0, "bool fallback off");
    check_int(recomp_config_bool(NULL, "missing", 1), 1, "bool fallback on");

    /* Environment beats the file, and an empty variable means on. */
#if defined(_WIN32)
    _putenv_s("RECOMP_TEST_SCALE", "4");
    _putenv_s("RECOMP_TEST_WIDE", "0");
    _putenv_s("RECOMP_TEST_EMPTY", "");
#else
    setenv("RECOMP_TEST_SCALE", "4", 1);
    setenv("RECOMP_TEST_WIDE", "0", 1);
    setenv("RECOMP_TEST_EMPTY", "", 1);
#endif
    check_str(recomp_config_lookup("RECOMP_TEST_SCALE", "resolution_scale"), "4",
              "environment beats the file");
    check_int(recomp_config_bool("RECOMP_TEST_WIDE", "widescreen", 0), 0,
              "environment off beats file on");
    check_int(recomp_config_bool("RECOMP_TEST_EMPTY", "widescreen", 0), 1,
              "empty environment variable means on");
    check_str(recomp_config_lookup("RECOMP_TEST_UNSET", "resolution_scale"), "2",
              "unset variable falls through to the file");

    /* The round trip is the contract between the launcher, which writes
     * the file, and the runtime, which reads it. Write settings out, read
     * them back, and they must be the same settings. */
    {
        const char *rt = "test_config_roundtrip.conf";
        RecompSettings out, back;

        recomp_settings_defaults(&out);
        out.resolution_scale  = 3;
        out.widescreen        = 1;
        out.hor_plus          = 0.75;
        out.hor_plus_register = 60;
        out.anisotropy        = 16;
        out.fps_overlay       = 1;
        snprintf(out.frame_cap, sizeof out.frame_cap, "60");
        snprintf(out.game_dir, sizeof out.game_dir, "D:\\games\\ts2");

        check(recomp_settings_write(rt, &out) != 0, "settings written");
        check(recomp_settings_read(rt, &back) != 0, "settings read back");
        check_int(back.resolution_scale, 3, "round trip: scale");
        check_int(back.widescreen, 1, "round trip: widescreen");
        check(back.hor_plus > 0.74 && back.hor_plus < 0.76, "round trip: hor_plus");
        check_int(back.hor_plus_register, 60, "round trip: register");
        check_int(back.anisotropy, 16, "round trip: anisotropy");
        check_int(back.fps_overlay, 1, "round trip: overlay");
        check_str(back.frame_cap, "60", "round trip: frame cap");
        check_str(back.game_dir, "D:\\games\\ts2", "round trip: game dir");

        /* System Link, as the launcher's tab writes it for a joining copy. */
        snprintf(out.syslink, sizeof out.syslink, "join");
        snprintf(out.syslink_host, sizeof out.syslink_host, "friend.example.net:30000");
        out.syslink_port = 30000;
        snprintf(out.syslink_code, sizeof out.syslink_code, "Siberia");
        out.syslink_upnp = 0;
        check(recomp_settings_write(rt, &out) != 0, "System Link settings written");
        check(recomp_settings_read(rt, &back) != 0, "System Link settings read back");
        check_str(back.syslink, "join", "round trip: System Link mode");
        check_str(back.syslink_host, "friend.example.net:30000", "round trip: host");
        check_int(back.syslink_port, 30000, "round trip: port");
        check_str(back.syslink_code, "Siberia", "round trip: join code");
        check_int(back.syslink_upnp, 0, "round trip: router left alone");

        /* Empty host and code are written as comments and come back empty;
         * an unset port comes back as the default, and the router is asked. */
        recomp_settings_defaults(&out);
        check(recomp_settings_write(rt, &out) != 0, "default System Link written");
        check(file_has_line(rt, "syslink = off"), "System Link is off by default");
        check(file_has_line(rt, "# syslink_host ="), "no host is a comment");
        check(file_has_line(rt, "# syslink_code ="), "no code is a comment");
        check(recomp_settings_read(rt, &back) != 0, "default System Link read back");
        check_str(back.syslink_host, "", "default: no host");
        check_str(back.syslink_code, "", "default: no code");
        check_int(back.syslink_port, RECOMP_SYSLINK_DEFAULT_PORT, "default: port");
        check_int(back.syslink_upnp, 1, "default: the router is asked");
        write_file(rt, "syslink_port = 99999\n");
        check(recomp_settings_read(rt, &back) != 0, "out-of-range port read");
        check_int(back.syslink_port, 65535, "clamp: port to 65535");

        /* A file written by an older build, missing newer keys, must keep
         * the defaults for them rather than zeroing them. */
        write_file(rt, "resolution_scale = 4\n");
        check(recomp_settings_read(rt, &back) != 0, "partial file read");
        check_int(back.resolution_scale, 4, "partial: the key that is there");
        check_int(back.anisotropy, 1, "partial: absent key keeps its default");
        check_str(back.frame_cap, "adaptive", "partial: absent word keeps default");
        check_near(back.hor_plus, RECOMP_HOR_PLUS_AUTO, "partial: no hor_plus is auto");
        check_int(back.hor_plus_register, RECOMP_REGISTER_AUTO,
                  "partial: no register is auto");

        /* Auto is written as a commented-out line, so a build from before
         * the word existed reads the file as it read a missing key, and
         * comes back as auto. The word itself is accepted from a person. */
        recomp_settings_defaults(&out);
        check_near(out.hor_plus, RECOMP_HOR_PLUS_AUTO, "default: hor_plus is auto");
        check_int(out.hor_plus_register, RECOMP_REGISTER_AUTO, "default: register is auto");
        check(recomp_settings_write(rt, &out) != 0, "default settings written");
        check(file_has_line(rt, "# hor_plus = auto"), "auto hor_plus is a comment");
        check(file_has_line(rt, "# hor_plus_register = auto"), "auto register is a comment");
        check(file_has_line(rt, "widescreen = 0"), "widescreen is off by default");
        check(recomp_settings_read(rt, &back) != 0, "default settings read back");
        check_near(back.hor_plus, RECOMP_HOR_PLUS_AUTO, "round trip: auto hor_plus");
        check_int(back.hor_plus_register, RECOMP_REGISTER_AUTO, "round trip: auto register");

        write_file(rt, "hor_plus = Auto\nhor_plus_register = auto\n");
        check(recomp_settings_read(rt, &back) != 0, "auto words read");
        check_near(back.hor_plus, RECOMP_HOR_PLUS_AUTO, "the word auto: hor_plus");
        check_int(back.hor_plus_register, RECOMP_REGISTER_AUTO, "the word auto: register");

        /* 0 is an override -- leave the camera alone -- and must not be
         * taken for auto on the way through; nor may a negative number. */
        write_file(rt, "hor_plus = 0\n");
        check(recomp_settings_read(rt, &back) != 0, "zero hor_plus read");
        check_near(back.hor_plus, 0.0, "hor_plus 0 stays 0");
        check(recomp_settings_write(rt, &back) != 0, "zero hor_plus written");
        check(file_has_line(rt, "hor_plus = 0"), "hor_plus 0 is written as a line");
        write_file(rt, "hor_plus = -1\n");
        check(recomp_settings_read(rt, &back) != 0, "negative hor_plus read");
        check_near(back.hor_plus, 0.0, "a negative hor_plus is 0, not auto");

        /* Out-of-range values are clamped, not taken literally: a hand
         * edited 99 must not ask for a 99x render. */
        write_file(rt, "resolution_scale = 99\nanisotropy = 0\n");
        check(recomp_settings_read(rt, &back) != 0, "clamped file read");
        check_int(back.resolution_scale, 8, "clamp: scale to 8");
        check_int(back.anisotropy, 1, "clamp: anisotropy to 1");

        remove(rt);
    }

    /* Reading settings must not disturb the lookup table the runtime is
     * already using -- the launcher does both in one process. */
    check_str(recomp_config_lookup(NULL, "resolution_scale"), "2",
              "lookups survive a settings read");

    test_widescreen_resolution();
    test_guest_flag();
    test_older_files();

    /* The same resolution as the runtime asks for it: from the settings
     * of this process -- the file above says widescreen = 1 and
     * hor_plus = 0.75 -- and what the title has said. */
    {
        RecompWidescreen w;
        unsigned serial;

        set_env("RECOMP_WIDESCREEN", NULL);
        set_env("RECOMP_HOR_PLUS", NULL);
        set_env("RECOMP_HOR_PLUS_REG", NULL);

        serial = recomp_widescreen_title_serial();
        recomp_widescreen_title(RECOMP_WIDE_NATIVE, 96);
        check(recomp_widescreen_title_serial() != serial, "the title speaking is noticed");
        check_int((int)recomp_widescreen_title_mode(), (int)RECOMP_WIDE_NATIVE, "title mode");

        recomp_widescreen_resolve(&w);
        check_int(w.on, 1, "process: widescreen from the file");
        check_near(w.hor_plus, 0.75, "process: the file's hor_plus beats the title");
        check_int(w.hor_plus_named, 1, "process: and is reported as named");
        check_int(w.projection_register, 96, "process: the title's register");

        /* The environment over the file: auto hands it back to the title. */
        set_env("RECOMP_HOR_PLUS", "auto");
        recomp_widescreen_resolve(&w);
        check_near(w.hor_plus, 1.0, "process: auto on a native title widens nothing");
        recomp_widescreen_title(RECOMP_WIDE_HOR_PLUS, -1);
        recomp_widescreen_resolve(&w);
        check_near(w.hor_plus, 0.75, "process: auto on a Hor+ title is 0.75");
        check_int(w.projection_register, 96, "process: a register is kept when not renamed");

        set_env("RECOMP_WIDESCREEN", "0");
        recomp_widescreen_resolve(&w);
        check_int(w.on, 0, "process: the environment turns widescreen off");
        check_near(w.hor_plus, 1.0, "process: and with it the widening");

        set_env("RECOMP_WIDESCREEN", "1");
        set_env("RECOMP_HOR_PLUS", "0");
        set_env("RECOMP_HOR_PLUS_REG", "160");
        recomp_widescreen_resolve(&w);
        check_near(w.hor_plus, 1.0, "process: RECOMP_HOR_PLUS=0 leaves a Hor+ title alone");
        check_int(w.projection_register, 160, "process: RECOMP_HOR_PLUS_REG beats the title");
        check_int(w.title_register, 96, "process: the title's register is still known");

        set_env("RECOMP_WIDESCREEN", NULL);
        set_env("RECOMP_HOR_PLUS", NULL);
        set_env("RECOMP_HOR_PLUS_REG", NULL);
    }

    remove(path);
    if (failures)
        printf("test_config: %d FAILURE(S)\n", failures);
    else
        printf("test_config: all checks passed\n");
    return failures ? 1 : 0;
}
