/*
 * launcher_win32.c -- the launcher's window on Windows (launcher.h).
 *
 * A plain Win32 window: the back buffer launcher_theme_gdi.c draws into is
 * put on screen at WM_PAINT, a 16 ms timer runs the pad and the glow, and
 * the keyboard and mouse messages become launcher.c's calls.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define COBJMACROS
#include <windows.h>
#include <shobjidl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "launcher.h"
#include "launcher_theme.h"

static HWND g_hwnd;

/* ------------------------------------------------------------ the host */

uint64_t lhost_ticks(void)
{
    return GetTickCount64();
}

static int vk_of(int key)
{
    switch (key) {
    case LKEY_UP:     return VK_UP;
    case LKEY_DOWN:   return VK_DOWN;
    case LKEY_LEFT:   return VK_LEFT;
    case LKEY_RIGHT:  return VK_RIGHT;
    case LKEY_RETURN: return VK_RETURN;
    case LKEY_ESCAPE: return VK_ESCAPE;
    case LKEY_TAB:    return VK_TAB;
    case LKEY_BACK:   return VK_BACK;
    case LKEY_SPACE:  return VK_SPACE;
    case LKEY_LSHIFT: return VK_LSHIFT;
    case LKEY_RSHIFT: return VK_RSHIFT;
    case LKEY_LCTRL:  return VK_LCONTROL;
    case LKEY_RCTRL:  return VK_RCONTROL;
    default:          return key;       /* 'A'..'Z', '0'..'9' are their own */
    }
}

int lhost_key_held(int key)
{
    return (GetAsyncKeyState(vk_of(key)) & 0x8000) != 0;
}

int lhost_paste(char *out, size_t n)
{
    HANDLE h;
    const char *s;
    int got = 0;

    if (!n || !OpenClipboard(g_hwnd))
        return 0;
    h = GetClipboardData(CF_TEXT);
    s = h ? (const char *)GlobalLock(h) : NULL;
    if (s) {
        snprintf(out, n, "%s", s);
        GlobalUnlock(h);
        got = 1;
    }
    CloseClipboard();
    return got;
}

int lhost_copy(const char *text)
{
    size_t len = strlen(text) + 1;
    HGLOBAL h;
    int done = 0;

    if (!OpenClipboard(g_hwnd))
        return 0;
    EmptyClipboard();
    h = GlobalAlloc(GMEM_MOVEABLE, len);
    if (h) {
        memcpy(GlobalLock(h), text, len);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_TEXT, h))
            GlobalFree(h);
        else
            done = 1;
    }
    CloseClipboard();
    return done;
}

int lhost_launch(const char *exe, const char *dir)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);

    if (!CreateProcessA(exe, NULL, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        char msg[MAX_PATH + 160];

        snprintf(msg, sizeof msg,
                 "The game could not be started.\n\n%s\n\n"
                 "The launcher expects it beside itself.", exe);
        MessageBoxA(NULL, msg, "Cannot start the game", MB_OK | MB_ICONERROR);
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

void lhost_close(void)
{
    PostMessage(g_hwnd, WM_CLOSE, 0, 0);
}

static DWORD WINAPI thread_main(LPVOID fn)
{
    ((void (*)(void))fn)();
    return 0;
}

int lhost_thread(void (*fn)(void))
{
    HANDLE t = CreateThread(NULL, 0, thread_main, (LPVOID)fn, 0, NULL);

    if (!t)
        return 0;
    CloseHandle(t);
    return 1;
}

void lhost_publish(volatile long *flag, long value)
{
    InterlockedExchange((volatile LONG *)flag, value);
}

long lhost_read(volatile long *flag)
{
    return InterlockedCompareExchange((volatile LONG *)flag, 0, 0);
}

void lhost_full_path(const char *path, char *out, size_t n)
{
    if (!GetFullPathNameA(path, (DWORD)n, out, NULL))
        snprintf(out, n, "%s", path);
}

/* ------------------------------------------------------------------ job */

/* A self-building release's builder: one child at a time, its standard
 * output read a line at a time without waiting (launcher.h). It runs in a
 * job object that kills everything in it when closed, so Cancel -- or the
 * launcher going away -- stops the compilers it started too, as the POSIX
 * front end's process group does. No console window: the builder and every
 * tool under it are console programs. */
static HANDLE g_job = NULL, g_job_proc = NULL, g_job_out = NULL;
static int    g_job_exit = -1;
static char   g_job_buf[16384];
static size_t g_job_len;

/* One argument for CreateProcess's command line, quoted the way the C
 * runtime splits it again. */
static void append_arg(wchar_t *out, size_t cap, size_t *at, const wchar_t *arg)
{
    size_t i, slashes = 0;
    int quote = !*arg || wcspbrk(arg, L" \t\"") != NULL;

#define PUT(c) do { if (*at + 1 < cap) out[(*at)++] = (c); } while (0)
    if (*at)
        PUT(L' ');
    if (quote)
        PUT(L'"');
    for (i = 0; arg[i]; i++) {
        if (arg[i] == L'\\') {
            slashes++;
            continue;
        }
        if (arg[i] == L'"') {
            slashes = slashes * 2 + 1;           /* each backslash, then \" */
        }
        while (slashes) {
            PUT(L'\\');
            slashes--;
        }
        PUT(arg[i]);
    }
    if (quote)
        slashes *= 2;                            /* before the closing quote */
    while (slashes) {
        PUT(L'\\');
        slashes--;
    }
    if (quote)
        PUT(L'"');
    out[*at] = 0;
#undef PUT
}

int lhost_job_start(const char *const *argv)
{
    static wchar_t line[32768];
    wchar_t arg[4096], exe[MAX_PATH * 2];
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
    HANDLE rd = NULL, wr = NULL, nul;
    size_t at = 0;
    int i;

    if (g_job_proc || g_job_out)
        return 0;
    line[0] = 0;
    for (i = 0; argv[i]; i++) {
        if (!MultiByteToWideChar(CP_ACP, 0, argv[i], -1, arg, (int)(sizeof arg / sizeof arg[0])))
            return 0;
        append_arg(line, sizeof line / sizeof line[0], &at, arg);
    }
    if (!MultiByteToWideChar(CP_ACP, 0, argv[0], -1, exe, (int)(sizeof exe / sizeof exe[0])))
        return 0;

    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      &sa, OPEN_EXISTING, 0, NULL);

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = nul;                 /* the builder logs to build.log */
    if (!CreateProcessW(exe, line, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        if (nul != INVALID_HANDLE_VALUE)
            CloseHandle(nul);
        return 0;
    }
    CloseHandle(wr);                    /* the child's now: its exit ends the pipe */
    if (nul != INVALID_HANDLE_VALUE)
        CloseHandle(nul);

    g_job = CreateJobObjectW(NULL, NULL);
    if (g_job) {
        memset(&lim, 0, sizeof lim);
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &lim, sizeof lim);
        AssignProcessToJobObject(g_job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_job_proc = pi.hProcess;
    g_job_out = rd;
    g_job_len = 0;
    g_job_exit = -1;
    return 1;
}

int lhost_job_line(char *out, size_t n)
{
    char *nl;
    DWORD avail = 0, got = 0;

    if (g_job_out && g_job_len < sizeof g_job_buf - 1) {
        if (!PeekNamedPipe(g_job_out, NULL, 0, NULL, &avail, NULL)) {
            CloseHandle(g_job_out);      /* it has closed its end: no more lines */
            g_job_out = NULL;
        } else if (avail) {
            DWORD room = (DWORD)(sizeof g_job_buf - 1 - g_job_len);
            if (ReadFile(g_job_out, g_job_buf + g_job_len, avail < room ? avail : room, &got, NULL))
                g_job_len += got;
        }
    }
    nl = memchr(g_job_buf, '\n', g_job_len);
    if (!nl && !g_job_out && g_job_len) {
        nl = g_job_buf + g_job_len;          /* a last line with no newline */
        g_job_len++;
    }
    if (!nl) {
        if (g_job_len == sizeof g_job_buf - 1)
            g_job_len = 0;                   /* a line longer than the buffer */
        return 0;
    }
    *nl = 0;
    if (nl > g_job_buf && nl[-1] == '\r')
        nl[-1] = 0;
    snprintf(out, n, "%s", g_job_buf);
    g_job_len -= (size_t)(nl - g_job_buf) + 1;
    memmove(g_job_buf, nl + 1, g_job_len);
    return 1;
}

int lhost_job_status(void)
{
    DWORD code;

    if (g_job_proc && WaitForSingleObject(g_job_proc, 0) == WAIT_OBJECT_0) {
        g_job_exit = GetExitCodeProcess(g_job_proc, &code) ? (int)code : 1;
        CloseHandle(g_job_proc);
        g_job_proc = NULL;
        if (g_job) {
            CloseHandle(g_job);
            g_job = NULL;
        }
    }
    if (g_job_proc || g_job_out || g_job_len)
        return -1;
    return g_job_exit;
}

void lhost_job_cancel(void)
{
    if (g_job)
        TerminateJobObject(g_job, 1);       /* the builder and its tools */
    else if (g_job_proc)
        TerminateProcess(g_job_proc, 1);
}

/* --------------------------------------------------------------- picker */

/* The shell's own open dialog (IFileOpenDialog), for an image or a folder.
 * Modal, so it runs here; its answer is posted to the window, so the
 * launcher hears it from the message loop as launcher.h asks. The GUIDs are
 * spelled out: not every SDK's uuid library carries the dialog's. */
#define WM_PICKED (WM_APP + 1)

static const CLSID k_clsid_file_open = {0xdc1c5a9c, 0xe88a, 0x4dde,
                                        {0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7}};
static const IID k_iid_file_open = {0xd57c7288, 0xd4ad, 0x4768,
                                    {0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60}};

int lhost_pick(int folder)
{
    static const COMDLG_FILTERSPEC k_images[] = {
        { L"Xbox disc images (*.iso, *.xiso)", L"*.iso;*.xiso" },
        { L"All files", L"*.*" },
    };
    IFileOpenDialog *dlg = NULL;
    IShellItem *item = NULL;
    wchar_t *wpath = NULL;
    char *path = NULL;
    DWORD opts = 0;
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        return 0;
    if (FAILED(CoCreateInstance(&k_clsid_file_open, NULL, CLSCTX_INPROC_SERVER,
                                &k_iid_file_open, (void **)&dlg)))
        return 0;
    IFileOpenDialog_GetOptions(dlg, &opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (folder) {
        opts |= FOS_PICKFOLDERS;
        IFileOpenDialog_SetTitle(dlg, L"The folder the game's files are in");
    } else {
        opts |= FOS_FILEMUSTEXIST;
        IFileOpenDialog_SetFileTypes(dlg, 2, k_images);
        IFileOpenDialog_SetTitle(dlg, L"The game's disc image");
    }
    IFileOpenDialog_SetOptions(dlg, opts);
    if (SUCCEEDED(IFileOpenDialog_Show(dlg, g_hwnd)) &&
        SUCCEEDED(IFileOpenDialog_GetResult(dlg, &item)) &&
        SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &wpath))) {
        int n = WideCharToMultiByte(CP_ACP, 0, wpath, -1, NULL, 0, NULL, NULL);
        path = n > 0 ? (char *)malloc((size_t)n) : NULL;
        if (path)
            WideCharToMultiByte(CP_ACP, 0, wpath, -1, path, n, NULL, NULL);
        CoTaskMemFree(wpath);
    }
    if (item)
        IShellItem_Release(item);
    IFileOpenDialog_Release(dlg);
    /* NULL is a cancel, which the launcher hears too. */
    PostMessageA(g_hwnd, WM_PICKED, 0, (LPARAM)path);
    return 1;
}

/* ---------------------------------------------------------------- window */

static int key_of(WPARAM vk)
{
    switch (vk) {
    case VK_UP:     return LKEY_UP;
    case VK_DOWN:   return LKEY_DOWN;
    case VK_LEFT:   return LKEY_LEFT;
    case VK_RIGHT:  return LKEY_RIGHT;
    case VK_RETURN: return LKEY_RETURN;
    case VK_ESCAPE: return LKEY_ESCAPE;
    case VK_TAB:    return LKEY_TAB;
    default:        return 0;
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);

        launcher_draw();
        theme_present(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;                       /* everything is painted */
    case WM_KEYDOWN:
        launcher_key(key_of(wp), (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_CHAR:
        launcher_char((unsigned)wp);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        if (!launcher_mouse_button(LMOUSE_LEFT))
            launcher_click(LOWORD(lp), HIWORD(lp));
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        /* Only ever a binding being captured: the launcher has no other use
         * for them. */
        launcher_mouse_button(msg == WM_RBUTTONDOWN ? LMOUSE_RIGHT
                              : msg == WM_MBUTTONDOWN ? LMOUSE_MIDDLE
                              : GET_XBUTTON_WPARAM(wp) == XBUTTON2 ? LMOUSE_X2 : LMOUSE_X1);
        InvalidateRect(h, NULL, FALSE);
        return msg == WM_XBUTTONDOWN ? TRUE : 0;
    case WM_MOUSEMOVE:
        launcher_hover(LOWORD(lp), HIWORD(lp));
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_MOUSEWHEEL: {
        /* WHEEL_DELTA (120) a notch, positive away from the user, which is
         * up the list. A touchpad sends fractions of a notch, kept until
         * they add up to one. The position is in screen coordinates,
         * signed (a second monitor to the left is negative). */
        static int part;
        POINT pt;
        int rows;

        part += GET_WHEEL_DELTA_WPARAM(wp);
        rows = part / WHEEL_DELTA;
        part -= rows * WHEEL_DELTA;
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        ScreenToClient(h, &pt);
        if (rows) {
            launcher_wheel(-rows, pt.x, pt.y);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_TIMER:
        launcher_tick();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_SIZE:
        launcher_resize(LOWORD(lp), HIWORD(lp));
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_PICKED:
        launcher_picked((const char *)lp);
        free((void *)lp);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_DESTROY:
        /* Closing the window cancels a build, as on the other front ends. */
        lhost_job_cancel();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    MSG msg;
    RECT r;

    (void)prev; (void)cmd;

    /* Before any window exists: otherwise Windows draws us smaller and
     * scales the result up, which on a look made of soft edges and
     * glows shows as a blur over all of it. */
    SetProcessDPIAware();

    launcher_init();

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hIcon         = LoadIconA(inst, MAKEINTRESOURCEA(1));   /* cmake/title_icon.cmake */
    wc.lpszClassName = "xboxrecomp_launcher";
    if (!RegisterClassA(&wc))
        return 1;

    r.left = r.top = 0;
    r.right = LAUNCHER_WINDOW_W;
    r.bottom = LAUNCHER_WINDOW_H;
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    {
        char caption[160];

        snprintf(caption, sizeof caption, "%s", launcher_title());
        g_hwnd = CreateWindowA(wc.lpszClassName, caption,
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               r.right - r.left, r.bottom - r.top,
                               NULL, NULL, inst, NULL);
    }
    if (!g_hwnd)
        return 1;
    if (!theme_begin(LAUNCHER_WINDOW_W, LAUNCHER_WINDOW_H))
        return 1;

    ShowWindow(g_hwnd, show);
    SetTimer(g_hwnd, 1, 16, NULL);          /* the pad, and the glow */

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    theme_shutdown();
    return 0;
}
