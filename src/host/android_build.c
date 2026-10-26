/*
 * android_build.c - recompiling the player's game on the device.
 *
 * An APK carries no game code. On first launch the app makes the title's
 * lifted C from the player's own disc and compiles it, on the device: the
 * pipeline in a Python embedded in the app (python.org's Android build), and
 * zig as the compiler, shipped in the APK as libzig.so because Android runs
 * binaries only from an app's library folder.
 *
 * This first part is the feasibility probe, run instead of the title when
 * recomp_env.txt sets RECOMP_ANDROID_PROBE:
 *
 *   zig     run libzig.so, compile a test library with it and load that
 *   python  start the embedded Python and run scripts/recompile.py on the
 *           game, timing each stage
 *
 * What it needs that is not in the APK is pushed into the app's files folder
 * by hand for now (docs/technical/android.md, "Building on the device").
 */
#if defined(__ANDROID__)

#include <SDL3/SDL.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* Where the APK's libraries are: libmain.so's folder. */
static int lib_dir(char *out, size_t n)
{
    Dl_info info;
    char *slash;

    if (!dladdr((void *)lib_dir, &info) || !info.dli_fname)
        return 0;
    snprintf(out, n, "%s", info.dli_fname);
    slash = strrchr(out, '/');
    if (!slash)
        return 0;
    *slash = '\0';
    return 1;
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* Runs exe with argv, its output into ours (the log), and waits. */
static int run(const char *exe, char *const argv[])
{
    pid_t pid;
    int status = 0, rc, i;
    double t0 = now_s();

    fprintf(stderr, "[BUILD] $");
    for (i = 0; argv[i]; i++)
        fprintf(stderr, " %s", argv[i]);
    fprintf(stderr, "\n");
    rc = posix_spawn(&pid, exe, NULL, NULL, argv, environ);
    if (rc != 0) {
        fprintf(stderr, "[BUILD] cannot start %s: %s\n", exe, strerror(rc));
        return -1;
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "[BUILD] killed by signal %d after %.2f s\n", WTERMSIG(status), now_s() - t0);
        return -1;
    }
    fprintf(stderr, "[BUILD] exit %d after %.2f s\n", WEXITSTATUS(status), now_s() - t0);
    return WEXITSTATUS(status);
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static int probe_zig(const char *files)
{
    char libs[512], zig[600], dir[600], src[700], out[700], sysroot[600], crt[700];
    char libdir[700], zlib[600], cache[600];
    const char *triples[] = { "aarch64-linux-android", "aarch64-linux-android.30", NULL };
    int t;

    if (!lib_dir(libs, sizeof libs))
        return 1;
    snprintf(zig, sizeof zig, "%s/libzig.so", libs);
    snprintf(dir, sizeof dir, "%s/probe", files);
    mkdir(dir, 0770);
    snprintf(zlib, sizeof zlib, "%s/zig/lib", files);
    snprintf(cache, sizeof cache, "%s/zig-cache", files);
    setenv("ZIG_LIB_DIR", zlib, 1);
    setenv("ZIG_GLOBAL_CACHE_DIR", cache, 1);
    setenv("ZIG_LOCAL_CACHE_DIR", cache, 1);
    setenv("HOME", files, 1);

    {
        char *argv[] = { zig, "version", NULL };
        if (run(zig, argv) != 0)
            return 1;
    }

    snprintf(src, sizeof src, "%s/probe.c", dir);
    write_file(src,
        "#include <stdio.h>\n#include <string.h>\n#include <setjmp.h>\n"
        "int probe_add(int a, int b) {\n"
        "    char buf[32]; jmp_buf jb;\n"
        "    if (setjmp(jb)) return -1;\n"
        "    snprintf(buf, sizeof buf, \"%d\", a + b);\n"
        "    return (int)strlen(buf) * 1000 + a + b;\n"
        "}\n");
    snprintf(sysroot, sizeof sysroot, "%s/ndk-sysroot", files);
    snprintf(libdir, sizeof libdir, "%s/usr/lib/aarch64-linux-android/30", sysroot);
    snprintf(crt, sizeof crt, "-L%s", libdir);

    /* zig has no Android C library of its own: a libc file names the NDK
     * sysroot's headers and start files, which zig then links against. */
    {
        char libc_txt[700], text[3000];
        snprintf(libc_txt, sizeof libc_txt, "%s/libc.txt", dir);
        snprintf(text, sizeof text,
                 "include_dir=%s/usr/include\n"
                 "sys_include_dir=%s/usr/include/aarch64-linux-android\n"
                 "crt_dir=%s\n"
                 "cc_dir=%s\n"
                 "msvc_lib_dir=\nkernel32_lib_dir=\ndarwin_sdk_dir=\n",
                 sysroot, sysroot, libdir, libdir);
        write_file(libc_txt, text);
        setenv("ZIG_LIBC", libc_txt, 1);
    }
    (void)crt;
    for (t = 0; triples[t]; t++) {
        char target[64];
        void *h;
        /* Loadable code must be in the app's internal folder: Android's
         * linker refuses a library on shared storage. */
        snprintf(out, sizeof out, "%s/libprobe%d.so", SDL_GetAndroidInternalStoragePath(), t);
        snprintf(target, sizeof target, "%s", triples[t]);
        {
            char *argv[] = { zig, "cc", "-target", target, "-shared", "-fPIC", "-O2",
                             "-o", out, src, NULL };
            if (run(zig, argv) != 0)
                continue;
        }
        h = dlopen(out, RTLD_NOW);
        if (!h) {
            fprintf(stderr, "[BUILD] dlopen %s: %s\n", out, dlerror());
            continue;
        }
        {
            int (*add)(int, int) = (int (*)(int, int))dlsym(h, "probe_add");
            fprintf(stderr, "[BUILD] %s: probe_add(40, 2) = %d (want 2042)\n",
                    target, add ? add(40, 2) : -999);
        }
    }
    return 0;
}

static int probe_python(const char *files)
{
    char lib[600], home[600], site[700], cap[700], script[700];
    char xbe[700], work[700], gen[700], title[600], manual[700], seeds[700], patches[700];
    char symbols[700];
    void *h;
    int (*bytes_main)(int, char **);
    double t0;
    int rc;

    /* Python's own libraries and native modules are loaded code, so its home
     * is in the app's internal folder; copied there from the pushed one. */
    {
        char from[600];
        snprintf(home, sizeof home, "%s/python", SDL_GetAndroidInternalStoragePath());
        snprintf(from, sizeof from, "%s/python", files);
        if (access(home, F_OK) != 0) {
            char *argv[] = { "/system/bin/cp", "-r", from, home, NULL };
            run("/system/bin/cp", argv);
        }
    }
    snprintf(lib, sizeof lib, "%s/lib/libpython3.14.so", home);
    h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        fprintf(stderr, "[BUILD] dlopen %s: %s\n", lib, dlerror());
        return 1;
    }
    bytes_main = (int (*)(int, char **))dlsym(h, "Py_BytesMain");
    if (!bytes_main) {
        fprintf(stderr, "[BUILD] no Py_BytesMain in %s\n", lib);
        return 1;
    }
    snprintf(site, sizeof site, "%s/site", files);
    /* libcapstone.so is in the APK, beside the engine: loadable code. */
    if (!lib_dir(cap, sizeof cap))
        return 1;
    snprintf(title, sizeof title, "%s/title", files);
    setenv("PYTHONHOME", home, 1);
    setenv("PYTHONPATH", site, 1);
    setenv("LIBCAPSTONE_PATH", cap, 1);
    setenv("RECOMP_IN_PROCESS", "1", 1);
    setenv("PYTHONUNBUFFERED", "1", 1);

    snprintf(script, sizeof script, "%s/pipeline/scripts/recompile.py", files);
    snprintf(xbe, sizeof xbe, "%s/game/default.xbe", files);
    snprintf(work, sizeof work, "%s/probe/work", files);
    snprintf(gen, sizeof gen, "%s/probe/gen", files);
    snprintf(manual, sizeof manual, "%s/manual", title);
    snprintf(seeds, sizeof seeds, "%s/seeds.json", title);
    snprintf(patches, sizeof patches, "%s/xbe_patches.json", title);
    snprintf(symbols, sizeof symbols, "%s/xdk_symbols.json", title);
    {
        char *argv[] = { "python3", "-X", "utf8", script, xbe,
                         "--work-dir", work, "--project", manual, "--gen-dir", gen,
                         "--seeds", seeds, "--xbe-patches", patches,
                         "--hle-symbols", symbols,
                         "--jobs", (char *)(getenv("RECOMP_ANDROID_JOBS") ? getenv("RECOMP_ANDROID_JOBS") : "1"),
                         NULL };
        int argc = (int)(sizeof argv / sizeof argv[0]) - 1;
        t0 = now_s();
        rc = bytes_main(argc, argv);
    }
    fprintf(stderr, "[BUILD] recompile.py exit %d after %.1f s\n", rc, now_s() - t0);
    return rc;
}

/* Sets zig's environment: its library, its caches, and the libc file that
 * points it at the NDK sysroot (probe_zig explains). */
static int zig_env(const char *files, char *zig, size_t n)
{
    char libs[512], zlib[600], cache[600], dir[600], libc_txt[700], text[3000];
    char sysroot[600], crt[700];

    if (!lib_dir(libs, sizeof libs))
        return 0;
    snprintf(zig, n, "%s/libzig.so", libs);
    snprintf(zlib, sizeof zlib, "%s/zig/lib", files);
    snprintf(cache, sizeof cache, "%s/zig-cache", SDL_GetAndroidInternalStoragePath());
    setenv("ZIG_LIB_DIR", zlib, 1);
    setenv("ZIG_GLOBAL_CACHE_DIR", cache, 1);
    setenv("ZIG_LOCAL_CACHE_DIR", cache, 1);
    snprintf(dir, sizeof dir, "%s/probe", files);
    mkdir(dir, 0770);
    snprintf(sysroot, sizeof sysroot, "%s/ndk-sysroot", files);
    snprintf(crt, sizeof crt, "%s/usr/lib/aarch64-linux-android/30", sysroot);
    snprintf(libc_txt, sizeof libc_txt, "%s/libc.txt", dir);
    snprintf(text, sizeof text,
             "include_dir=%s/usr/include\n"
             "sys_include_dir=%s/usr/include/aarch64-linux-android\n"
             "crt_dir=%s\ncc_dir=%s\n"
             "msvc_lib_dir=\nkernel32_lib_dir=\ndarwin_sdk_dir=\n",
             sysroot, sysroot, crt, crt);
    write_file(libc_txt, text);
    setenv("ZIG_LIBC", libc_txt, 1);
    return 1;
}

/* Compiles every lifted file (each .c in probe/gen) to an object, `jobs` at a time,
 * with the flags the NDK build gives lifted code. Times the whole of it. */
static int probe_compile(const char *files)
{
    char zig[600], gen[700], objdir[700];
    const char *j = getenv("RECOMP_ANDROID_JOBS");
    int jobs = j ? atoi(j) : 8, running = 0, done = 0, failed = 0, total = 0;
    DIR *d;
    struct dirent *e;
    double t0 = now_s();
    char names[256][64];
    int next = 0, i;

    if (!zig_env(files, zig, sizeof zig))
        return 1;
    snprintf(gen, sizeof gen, "%s/probe/gen", files);
    snprintf(objdir, sizeof objdir, "%s/obj", SDL_GetAndroidInternalStoragePath());
    mkdir(objdir, 0770);
    if (!(d = opendir(gen)))
        return 1;
    while ((e = readdir(d)) && total < 256) {
        size_t len = strlen(e->d_name);
        if (len > 2 && !strcmp(e->d_name + len - 2, ".c"))
            snprintf(names[total++], sizeof names[0], "%s", e->d_name);
    }
    closedir(d);
    fprintf(stderr, "[BUILD] compiling %d lifted files, %d at a time\n", total, jobs);

    while (done < total) {
        while (running < jobs && next < total) {
            char src[800], obj[800];
            char *argv[] = { zig, "cc", "-target", "aarch64-linux-android", "-c",
                             "-O3", "-DNDEBUG", "-std=gnu11", "-fPIC", "-fwrapv", "-w",
                             "-Werror=implicit-function-declaration",
                             "-ffunction-sections", "-fdata-sections",
                             "-I", gen, "-o", obj, src, NULL };
            pid_t pid;
            snprintf(src, sizeof src, "%s/%s", gen, names[next]);
            snprintf(obj, sizeof obj, "%s/%s.o", objdir, names[next]);
            argv[15] = gen;
            if (posix_spawn(&pid, zig, NULL, NULL, argv, environ) == 0)
                running++;
            else
                failed++, done++;
            next++;
        }
        {
            int status;
            pid_t pid = wait(&status);
            if (pid < 0)
                break;
            running--;
            done++;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                failed++;
            if (done % 8 == 0 || done == total)
                fprintf(stderr, "[BUILD] %d/%d compiled, %.1f s\n", done, total, now_s() - t0);
        }
    }
    (void)i;
    fprintf(stderr, "[BUILD] compiled %d files in %.1f s, %d failed\n", total, now_s() - t0, failed);
    return failed != 0;
}

/* Python as a program (libpyexec.so): its own executable, spawned workers,
 * Capstone. */
static int probe_pyexec(const char *files)
{
    char libs[512], exe[600], home[600], from[600];
    char script[] =
        "import sys, os, multiprocessing as mp, time\n"
        "print('executable', sys.executable, sys.version.split()[0])\n"
        "t = time.time()\n"
        "try:\n"
        "    with mp.get_context('spawn').Pool(4) as p: print('pool', p.map(abs, [-1, -2, -3, -4]))\n"
        "except Exception as e: print('no pool:', type(e).__name__, e)\n"
        "import subprocess; print('subprocess', subprocess.run([sys.executable, '-c', 'print(6*7)'], capture_output=True, text=True).stdout.strip())\n"
        "print('spawn pool in %.2f s' % (time.time() - t))\n"
        "import capstone; md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)\n"
        "print('capstone', [i.mnemonic for i in md.disasm(bytes([0x55, 0x8b, 0xec, 0xc3]), 0)])\n";

    if (!lib_dir(libs, sizeof libs))
        return 1;
    snprintf(exe, sizeof exe, "%s/libpyexec.so", libs);
    snprintf(home, sizeof home, "%s/python", SDL_GetAndroidInternalStoragePath());
    snprintf(from, sizeof from, "%s/python", files);
    if (access(home, F_OK) != 0) {
        char *argv[] = { "/system/bin/cp", "-r", from, home, NULL };
        run("/system/bin/cp", argv);
    }
    setenv("PYTHONHOME", home, 1);
    setenv("LD_LIBRARY_PATH", libs, 1);
    setenv("LIBCAPSTONE_PATH", libs, 1);
    {
        char site[700];
        snprintf(site, sizeof site, "%s/site", files);
        setenv("PYTHONPATH", site, 1);
    }
    unsetenv("RECOMP_IN_PROCESS");
    {
        char *argv[] = { exe, "-u", "-c", script, NULL };
        return run(exe, argv);
    }
}

int android_probe(const char *files)
{
    const char *what = getenv("RECOMP_ANDROID_PROBE");

    fprintf(stderr, "[BUILD] probe: %s\n", what);
    if (!strcmp(what, "zig"))
        return probe_zig(files);
    if (!strcmp(what, "python"))
        return probe_python(files);
    if (!strcmp(what, "compile"))
        return probe_compile(files);
    if (!strcmp(what, "pyexec"))
        return probe_pyexec(files);
    fprintf(stderr, "[BUILD] unknown probe %s\n", what);
    return 1;
}

#endif /* __ANDROID__ */
