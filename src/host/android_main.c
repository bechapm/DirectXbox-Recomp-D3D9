/*
 * android_main.c - the title's entry on Android.
 *
 * A title is libmain.so there, loaded by SDL's Java activity, which calls
 * SDL_main on a thread of its own. The title's main.c is not changed: its
 * main() is compiled as recomp_title_main (cmake/android.cmake), and this
 * file gives it what a desktop process would already have.
 *
 *   - Output. An app's stdout and stderr go nowhere, so both are sent down a
 *     pipe to logcat (tag "xboxrecomp") and to xboxrecomp.log in the app's
 *     files folder, which `adb pull` can fetch.
 *   - Switches. An app has no environment to start it with, so the RECOMP_*
 *     switches are read from recomp_env.txt in the same folder, one
 *     KEY=VALUE a line (# starts a comment); `adb push` puts one there.
 *   - Folders. The game in <files>/game unless SetupActivity found it
 *     elsewhere (RECOMP_GAME_DIR, set before this runs), the per-user folder
 *     (settings, saves, caches) in <files>/user and E: in <files>/hdd,
 *     unless the file names others.
 *
 * <files> is the app's own external folder,
 * /storage/emulated/0/Android/data/<package>/files: it needs no permission,
 * adb can reach it, and uninstalling the app deletes it.
 */
#if defined(__ANDROID__)

#include <SDL3/SDL.h>
#include <jni.h>
#define SDL_MAIN_HANDLED   /* SDL_main is defined here, by hand */
#include <SDL3/SDL_main.h>
#include <android/log.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "recomp_config.h"
#include "hle_settings.h"
#include "touch_controls.h"
#include "platform/host_main.h"

/* The title's main(), renamed where its main.c is compiled: in this library,
 * or, when the title is a library of its own (XBOXRECOMP_SPLIT_TITLE),
 * found in it when it is loaded (android_title.c). */
#if defined(XBOXRECOMP_SPLIT_TITLE)
int (*android_title_load(const char *path))(int, char **);
#else
extern int recomp_title_main(int argc, char **argv);
#endif

static FILE *g_log_file;

/* Copies the pipe to logcat, a line at a time, and to the log file. */
static void *log_pump(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[4096], line[1024];
    size_t used = 0;
    ssize_t n;

    while ((n = read(fd, buf, sizeof buf)) > 0) {
        ssize_t i;
        if (g_log_file) {
            fwrite(buf, 1, (size_t)n, g_log_file);
            fflush(g_log_file);
        }
        for (i = 0; i < n; i++) {
            if (buf[i] == '\n' || used == sizeof line - 1) {
                line[used] = '\0';
                __android_log_write(ANDROID_LOG_INFO, "xboxrecomp", line);
                used = 0;
                if (buf[i] == '\n')
                    continue;
            }
            line[used++] = buf[i];
        }
    }
    return NULL;
}

static void start_output(const char *files)
{
    char path[1024];
    int fds[2];
    pthread_t thread;

    snprintf(path, sizeof path, "%s/xboxrecomp.log", files);
    g_log_file = fopen(path, "w");
    if (pipe(fds) != 0)
        return;
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (pthread_create(&thread, NULL, log_pump, (void *)(intptr_t)fds[0]) == 0)
        pthread_detach(thread);
}

static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

static void read_switches(const char *files)
{
    char path[1024], line[1024];
    FILE *f;

    snprintf(path, sizeof path, "%s/recomp_env.txt", files);
    if (!(f = fopen(path, "r")))
        return;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line), *eq;
        if (!*s || *s == '#' || !(eq = strchr(s, '=')))
            continue;
        *eq = '\0';
        setenv(trim(s), trim(eq + 1), 1);
        fprintf(stderr, "[ANDROID] %s=%s (recomp_env.txt)\n", trim(s), trim(eq + 1));
    }
    fclose(f);
}

static void default_dir(const char *name, const char *files, const char *leaf)
{
    char path[1024];
    const char *v = getenv(name);

    if (v && *v)
        return;
    snprintf(path, sizeof path, "%s/%s", files, leaf);
    mkdir(path, 0770);
    setenv(name, path, 1);
}

/* ---- the in-game menu (RecompActivity's side panel) -------------------
 *
 * The menu is Android's own views over the game; these are what it calls.
 * Opening it holds the title at its next Swap (host_set_suspended), as
 * leaving the app does. A setting is applied at once where the runtime can
 * change it while the title runs, and written to the title's settings file
 * either way, so the next start has it too; widescreen and the resolution
 * scale are read only at start.
 */

static int settings_file(char *path, size_t n, RecompSettings *s)
{
    if (!recomp_settings_path(recomp_config_title_id(), path, n))
        return 0;
    recomp_settings_read(path, s);          /* the defaults when there is none */
    return 1;
}

JNIEXPORT void JNICALL
Java_org_xboxrecomp_app_RecompActivity_nativeMenu(JNIEnv *env, jclass cls, jboolean open)
{
    (void)env;
    (void)cls;
    host_set_suspended(HOST_SUSPEND_MENU, open ? 1 : 0);
    fprintf(stderr, "[ANDROID] menu %s\n", open ? "open: the title is held" : "closed");
}

/* key=value;key=value..., what the menu shows. */
JNIEXPORT jstring JNICALL
Java_org_xboxrecomp_app_RecompActivity_nativeSettings(JNIEnv *env, jclass cls)
{
    char path[1024], out[512];
    RecompSettings st;

    (void)cls;
    recomp_settings_defaults(&st);
    settings_file(path, sizeof path, &st);
    snprintf(out, sizeof out,
             "fps_overlay=%d;frame_cap=%s;touch_controls=%s;touch_opacity=%.2f;"
             "touch_scale=%.2f;widescreen=%d;resolution_scale=%d",
             hle_fps_overlay(), hle_frame_cap(), recomp_touch_mode(),
             (double)recomp_touch_opacity(), (double)recomp_touch_scale(),
             st.widescreen, st.resolution_scale);
    return (*env)->NewStringUTF(env, out);
}

JNIEXPORT void JNICALL
Java_org_xboxrecomp_app_RecompActivity_nativeSet(JNIEnv *env, jclass cls, jstring jkey, jstring jvalue)
{
    const char *key = (*env)->GetStringUTFChars(env, jkey, NULL);
    const char *value = (*env)->GetStringUTFChars(env, jvalue, NULL);
    char path[1024];
    RecompSettings st;
    int have_file;

    (void)cls;
    recomp_settings_defaults(&st);
    have_file = settings_file(path, sizeof path, &st);
    if (!strcmp(key, "fps_overlay")) {
        hle_set_fps_overlay(atoi(value));
        st.fps_overlay = atoi(value) ? 1 : 0;
    } else if (!strcmp(key, "frame_cap")) {
        hle_set_frame_cap(value);
        snprintf(st.frame_cap, sizeof st.frame_cap, "%s", !strcmp(value, "off") ? "0" : value);
    } else if (!strcmp(key, "touch_controls")) {
        recomp_touch_configure(value, 0.0f, 0.0f);
        snprintf(st.touch_controls, sizeof st.touch_controls, "%s", value);
    } else if (!strcmp(key, "touch_opacity")) {
        recomp_touch_configure(NULL, (float)atof(value), 0.0f);
        st.touch_opacity = atof(value);
    } else if (!strcmp(key, "touch_scale")) {
        recomp_touch_configure(NULL, 0.0f, (float)atof(value));
        st.touch_scale = atof(value);
    } else if (!strcmp(key, "widescreen")) {
        recomp_settings_set_widescreen(&st, atoi(value) ? 1 : 0);
    } else if (!strcmp(key, "resolution_scale")) {
        st.resolution_scale = atoi(value) < 1 ? 1 : atoi(value) > 4 ? 4 : atoi(value);
    }
    /* The title is held while the menu is open: its frame is presented
     * again with the change, so it is seen behind the menu as it is made. */
    host_request_redraw();
    if (have_file && !recomp_settings_write(path, &st))
        fprintf(stderr, "[ANDROID] could not save %s\n", path);
    fprintf(stderr, "[ANDROID] setting %s = %s\n", key, value);
    (*env)->ReleaseStringUTFChars(env, jkey, key);
    (*env)->ReleaseStringUTFChars(env, jvalue, value);
}

__attribute__((visibility("default")))
int SDL_main(int argc, char *argv[])
{
    const char *files;

    SDL_SetMainReady();
    files = SDL_GetAndroidExternalStoragePath();
    if (!files) {
        __android_log_print(ANDROID_LOG_ERROR, "xboxrecomp",
                            "no external files folder: %s", SDL_GetError());
        return 1;
    }
    mkdir(files, 0770);
    start_output(files);
    fprintf(stderr, "[ANDROID] files folder %s\n", files);
    read_switches(files);
    default_dir("RECOMP_GAME_DIR", files, "game");
    default_dir("RECOMP_USER_DIR", files, "user");
    /* E: (\Device\Harddisk0\Partition1) is the game's own folder on a
     * desktop, so a title's UDATA saves land beside it. Here the game may be
     * the player's ROM folder, shared with other programs: E: is the app's. */
    default_dir("RECOMP_HDD_DIR", files, "hdd");
    if (chdir(files) != 0)
        fprintf(stderr, "[ANDROID] chdir %s: %s\n", files, strerror(errno));
    if (getenv("RECOMP_ANDROID_PROBE")) {
        /* The on-device build's feasibility probe (android_build.c). */
        extern int android_probe(const char *files);
        int rc = android_probe(files);
        fprintf(stderr, "[ANDROID] probe finished: %d\n", rc);
        return rc;
    }
#if defined(XBOXRECOMP_SPLIT_TITLE)
    {
        char lib[1024];
        const char *v = getenv("RECOMP_ANDROID_TITLE_LIB");
        int (*title_main)(int, char **);

        if (v && *v) {
            snprintf(lib, sizeof lib, "%s", v);
        } else {
            /* Development: a libgame.so pushed into the files folder (adb
             * cannot reach the internal one) is copied in first, since
             * Android loads code only from the app's internal folder. */
            char pushed[1024];
            snprintf(lib, sizeof lib, "%s/libgame.so", SDL_GetAndroidInternalStoragePath());
            snprintf(pushed, sizeof pushed, "%s/libgame.so", files);
            if (access(pushed, R_OK) == 0) {
                char cmd[2200];
                snprintf(cmd, sizeof cmd, "/system/bin/cp '%s' '%s'", pushed, lib);
                fprintf(stderr, "[ANDROID] using the pushed %s\n", pushed);
                if (system(cmd) != 0)
                    fprintf(stderr, "[ANDROID] could not copy it in\n");
            }
        }
        title_main = android_title_load(lib);
        if (!title_main)
            return 1;
        return title_main(argc, argv);
    }
#else
    return recomp_title_main(argc, argv);
#endif
}

#endif /* __ANDROID__ */
