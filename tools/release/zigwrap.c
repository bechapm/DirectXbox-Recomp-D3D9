/*
 * zigwrap.c -- the compiler and archiver CMake is given in a Windows release:
 * zcc.exe, zc++.exe, zar.exe, zranlib.exe and zrc.exe, all this one program.
 *
 * Linux and macOS releases do this with shell scripts (fetch_toolchain.py's
 * wrappers); Windows has no shell to run one, and a .cmd file would put every
 * argument through cmd.exe's quoting. So this reads its own name, runs
 * ..\zig\zig.exe with the arguments that name stands for, then the ones it
 * was given exactly as it was given them, and returns zig's exit code.
 *
 * One argument is changed: -mwindows, which CMake passes for a windowed
 * executable (WIN32_EXECUTABLE) and zig ignores, becomes the linker's
 * subsystem flag, or the game would open with a console beside it.
 *
 * Built by fetch_toolchain.py with the release's own zig:
 *     zig cc -target x86_64-windows-gnu -O2 zigwrap.c -o zcc.exe
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

/* What each name runs, before the caller's arguments. TARGET is the
 * release's (fetch_toolchain.py's TARGETS). */
#define TARGET L"x86_64-windows-gnu"
static const struct { const wchar_t *name, *args; } k_modes[] = {
    { L"zcc",     L"cc -target " TARGET L" -fno-sanitize=all" },
    { L"zc++",    L"c++ -target " TARGET L" -fno-sanitize=all" },
    { L"zar",     L"ar" },
    { L"zranlib", L"ranlib" },
    { L"zrc",     L"rc" },
};

/* The command line after the program's own name, as CreateProcess gave it. */
static const wchar_t *after_argv0(const wchar_t *cmd)
{
    if (*cmd == L'"') {
        cmd++;
        while (*cmd && *cmd != L'"')
            cmd++;
        if (*cmd)
            cmd++;
    } else {
        while (*cmd && *cmd != L' ' && *cmd != L'\t')
            cmd++;
    }
    return cmd;
}

static int fail(const wchar_t *what)
{
    fwprintf(stderr, L"zigwrap: %ls (error %lu)\n", what, GetLastError());
    return 1;
}

int wmain(void)
{
    wchar_t self[MAX_PATH], zig[MAX_PATH + 32], *base, *dot, *slash;
    const wchar_t *mode = NULL, *rest;
    size_t i, len, cap;
    wchar_t *line, *out;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;

    if (!GetModuleFileNameW(NULL, self, MAX_PATH))
        return fail(L"cannot find itself");
    slash = wcsrchr(self, L'\\');
    base = slash ? slash + 1 : self;
    dot = wcsrchr(base, L'.');
    if (dot)
        *dot = 0;
    for (i = 0; i < sizeof k_modes / sizeof k_modes[0]; i++)
        if (_wcsicmp(base, k_modes[i].name) == 0)
            mode = k_modes[i].args;
    if (!mode) {
        fwprintf(stderr, L"zigwrap: no tool is called %ls\n", base);
        return 1;
    }
    /* toolchain\bin\<this> -> toolchain\zig\zig.exe */
    if (slash)
        *slash = 0;
    slash = wcsrchr(self, L'\\');
    if (slash)
        *slash = 0;
    swprintf(zig, sizeof zig / sizeof zig[0], L"%ls\\zig\\zig.exe", self);

    rest = after_argv0(GetCommandLineW());
    len = wcslen(rest);
    cap = wcslen(zig) + wcslen(mode) + len * 2 + 64;
    line = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, cap * sizeof(wchar_t));
    if (!line)
        return fail(L"out of memory");
    out = line + swprintf(line, cap, L"\"%ls\" %ls", zig, mode);
    /* The caller's arguments, with -mwindows (a whole argument) replaced. */
    while (*rest) {
        static const wchar_t flag[] = L"-mwindows";
        const size_t n = sizeof flag / sizeof flag[0] - 1;
        if ((rest[0] == L' ' || rest[0] == L'\t') && wcsncmp(rest + 1, flag, n) == 0 &&
            (rest[1 + n] == 0 || rest[1 + n] == L' ' || rest[1 + n] == L'\t')) {
            out += swprintf(out, cap - (size_t)(out - line), L" -Wl,--subsystem,windows");
            rest += 1 + n;
            continue;
        }
        *out++ = *rest++;
    }
    *out = 0;

    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(zig, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
        return fail(L"cannot start zig");
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
