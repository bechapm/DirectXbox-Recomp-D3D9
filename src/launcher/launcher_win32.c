/*
 * launcher_win32.c -- the launcher's window on Windows (launcher.h).
 *
 * A plain Win32 window: the back buffer launcher_theme_gdi.c draws into is
 * put on screen at WM_PAINT, a 16 ms timer runs the pad and the glow, and
 * the keyboard and mouse messages become launcher.c's calls.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
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

/* A self-building release's builder and its file dialog are not on Windows
 * yet: the release is Linux first. Saying so with 0 leaves the launcher's
 * first-run screen to ask for a typed path, and its Build to explain. */
int lhost_job_start(const char *const *argv)
{
    (void)argv;
    return 0;
}

int lhost_job_line(char *out, size_t n)
{
    (void)out; (void)n;
    return 0;
}

int lhost_job_status(void)
{
    return 0;
}

void lhost_job_cancel(void)
{
}

int lhost_pick(int folder)
{
    (void)folder;
    return 0;
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
    case WM_DESTROY:
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
