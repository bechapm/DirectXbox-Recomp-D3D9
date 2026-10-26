/*
 * pyexec.c - python, as a program the app can start (libpyexec.so).
 *
 * python.org's Android build is a library to embed, with no program. The
 * on-device build wants a real one: the title's own lift script runs the
 * pipeline as `python -m tools.<stage>`, and the lifter's parallel workers
 * start the interpreter again (multiprocessing's spawn). So this is the
 * program: it loads libpython3.14.so from the folder it is in and hands it
 * its arguments, exactly as the python command does.
 *
 * It is named lib*.so and shipped as one of the APK's libraries because
 * Android starts programs only from an app's library folder. The caller sets
 * PYTHONHOME (the standard library, unpacked from the APK) and
 * LD_LIBRARY_PATH (that folder, where Python's native modules find
 * libpython3.14.so).
 */
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    char dir[PATH_MAX], lib[PATH_MAX + 32];
    ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
    char *slash;
    void *h;
    int (*bytes_main)(int, char **);

    if (n <= 0) {
        perror("pyexec: /proc/self/exe");
        return 127;
    }
    dir[n] = '\0';
    slash = strrchr(dir, '/');
    if (slash)
        *slash = '\0';
    snprintf(lib, sizeof lib, "%s/libpython3.14.so", dir);
    h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        fprintf(stderr, "pyexec: %s\n", dlerror());
        return 127;
    }
    bytes_main = (int (*)(int, char **))dlsym(h, "Py_BytesMain");
    if (!bytes_main) {
        fprintf(stderr, "pyexec: no Py_BytesMain in %s\n", lib);
        return 127;
    }
    return bytes_main(argc, argv);
}
