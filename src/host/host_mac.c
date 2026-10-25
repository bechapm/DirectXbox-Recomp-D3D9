/*
 * host_mac.c -- the few AppKit calls SDL3 does not make for us, through the
 * Objective-C runtime so the library stays plain C.
 *
 * The runtime's three entry points are looked up, not linked: SDL has
 * loaded AppKit, and with it libobjc, by the time any of these is called,
 * and a self-building release links the game with zig's libSystem stubs
 * only, which have no libobjc (tools/release/fetch_toolchain.py).
 *
 * All of these run on the main thread (host_sdl.c calls them from there).
 */
#include "host_mac.h"

#ifdef __APPLE__
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>

static Class (*p_getClass)(const char *);
static SEL (*p_registerName)(const char *);
static void (*p_msgSend)(void);

static int objc_ready(void)
{
    if (!p_msgSend) {
        p_getClass = (Class (*)(const char *))dlsym(RTLD_DEFAULT, "objc_getClass");
        p_registerName = (SEL (*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");
        p_msgSend = (void (*)(void))dlsym(RTLD_DEFAULT, "objc_msgSend");
    }
    return p_getClass && p_registerName && p_msgSend;
}

#define objc_getClass(n)    (objc_ready() ? p_getClass(n) : (Class)0)
#define sel_registerName(n) p_registerName(n)
#define objc_msgSend        p_msgSend

typedef id (*msg_id)(id, SEL);
typedef id (*msg_id_ptr)(id, SEL, const char *);
typedef void (*msg_void_long)(id, SEL, long);
typedef void (*msg_void_id)(id, SEL, id);
typedef BOOL (*msg_bool)(id, SEL);
typedef id (*msg_activity)(id, SEL, unsigned long long, id);

static id app(void)
{
    Class cls = objc_getClass("NSApplication");

    return cls ? ((msg_id)objc_msgSend)((id)cls, sel_registerName("sharedApplication")) : nil;
}

void host_mac_set_accessory(void)
{
    id a = app();

    /* NSApplicationActivationPolicyAccessory (1): may have windows, is not in
     * the Dock or the app switcher, and is activated only if the user clicks
     * it. An unbundled program that SDL has not made "regular" is otherwise
     * left at Prohibited, whose windows the window server may not show. */
    if (a)
        ((msg_void_long)objc_msgSend)(a, sel_registerName("setActivationPolicy:"), 1);
}

void host_mac_keep_awake(void)
{
    static id token;
    Class pi_cls = objc_getClass("NSProcessInfo");
    Class str_cls = objc_getClass("NSString");
    id pi, reason;

    if (token || !pi_cls || !str_cls)
        return;
    pi = ((msg_id)objc_msgSend)((id)pi_cls, sel_registerName("processInfo"));
    reason = ((msg_id_ptr)objc_msgSend)((id)str_cls, sel_registerName("stringWithUTF8String:"),
                                        "a game is running");
    /* NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical:
     * no App Nap for a process whose window is often covered (a background
     * run sits below everything) or that has no window at all; a napped
     * process has its timers stretched by seconds, which changes a run's
     * pacing. It does not make a wake precise -- mach_wait_until still wakes
     * about 25% late with this held (measured by mac-platform: 8 ms -> +2 ms,
     * 1 ms -> +256 us) -- which is what src/platform's host_timer is for. */
    token = ((msg_activity)objc_msgSend)(pi, sel_registerName("beginActivityWithOptions:reason:"),
                                         0x00EFFFFFull | 0xFF00000000ull, reason);
    if (token)
        ((msg_id)objc_msgSend)(token, sel_registerName("retain"));
}

void host_mac_order_back(void *nswindow)
{
    /* Behind every other window at its level, without activating anything:
     * the macOS form of SetWindowPos(HWND_BOTTOM, SWP_NOACTIVATE). */
    if (nswindow && objc_ready())
        ((msg_void_id)objc_msgSend)((id)nswindow, sel_registerName("orderBack:"), nil);
}

int host_mac_app_is_active(void)
{
    id a = app();

    return a ? (int)((msg_bool)objc_msgSend)(a, sel_registerName("isActive")) : 0;
}

#else

void host_mac_set_accessory(void) {}
void host_mac_keep_awake(void) {}
void host_mac_order_back(void *nswindow) { (void)nswindow; }
int  host_mac_app_is_active(void) { return 0; }

#endif
