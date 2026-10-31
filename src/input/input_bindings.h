/*
 * input_bindings.h -- host devices and per-control bindings, from a config
 * file, for all four Xbox controller ports.
 *
 * One place decides what the four guest pads read: which host device stands
 * in for each port, and which key or pad control drives each of the title's
 * 24 inputs. Both input paths use it -- the XAPI replacement in src/hle and
 * the USB gamepad in src/usb -- so a binding changed once applies to a title
 * whichever way it reads its pads.
 *
 * The file is JSON, written by `py -3 -m tools.input_ui`; see
 * docs/technical/input-binding.md for the format and the search order. With
 * no file present the built-in defaults are what the runtime always did:
 * XInput pad 0 plus the keyboard on port 1, XInput pad N on port N.
 */
#ifndef XBOXRECOMP_INPUT_BINDINGS_H
#define XBOXRECOMP_INPUT_BINDINGS_H

#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load the config, once. Safe to call from anywhere and from any thread that
 * reaches it first; later calls do nothing. Logs one line naming the file it
 * read (or the built-in defaults) and the device on each port. */
void recomp_bindings_init(void);

/* The config file actually read, or NULL when the built-in defaults are in
 * use. Valid after recomp_bindings_init(). */
const char *recomp_bindings_source_path(void);

/* A human name for what drives this port: "SDL3 pad 1 + keyboard",
 * "keyboard", "XInput pad 3", "nothing". Never NULL. */
const char *recomp_bindings_device_name(unsigned port);

/* Bitmask of ports whose device is present right now, bit 0 = port 1. A
 * keyboard port is always present; a pad port is present when that pad
 * answers, or when the port also has keyboard bindings. Re-checks the pads
 * at most a few times a second, so a pad plugged in mid-game shows up. */
unsigned recomp_bindings_present_mask(void);

/* Sample one port through its bindings. Returns 0 (and a neutral pad) when
 * the port has no device. */
int recomp_bindings_sample(unsigned port, XBOX_GAMEPAD *out);

/* Rumble on the pad this port reads, 0..65535 per motor as XInputSetState
 * takes it. Nothing happens for a keyboard port or a pad without motors. */
void recomp_bindings_rumble(unsigned port, unsigned short low, unsigned short high);

/* Whether a key is held now, by Windows virtual-key code (off Windows,
 * translated to SDL's scancodes; only heard while the window has the focus
 * there). Any thread. Titles use xbox_D3D8KeyDown, which also checks the
 * game's window is in front on Windows. */
int recomp_key_down(int vk);

/* ---- the mouse ----------------------------------------------------------
 *
 * Fed by whoever owns the game window (src/hle/hle_d3d8.c), because only
 * the window hears the mouse. Its buttons are `mouse:` sources any control
 * can be bound to. Its motion is mouse look: with the title's `mouse_look`
 * setting on (RECOMP_MOUSE_LOOK), controller 1's right stick follows the
 * mouse as well as whatever is bound to it, so the pad's own stick still
 * works. Every call is safe from any thread. */
enum {
    RECOMP_MOUSE_LEFT, RECOMP_MOUSE_RIGHT, RECOMP_MOUSE_MIDDLE,
    RECOMP_MOUSE_X1, RECOMP_MOUSE_X2,
    RECOMP_MOUSE_BUTTONS,
    /* The wheel's two directions, "mouse:wheel_up" and "mouse:wheel_down":
     * sources too, though nothing holds them. Each notch is a short press
     * (recomp_mouse_wheel). */
    RECOMP_MOUSE_WHEEL_UP = RECOMP_MOUSE_BUTTONS, RECOMP_MOUSE_WHEEL_DOWN,
    RECOMP_MOUSE_SOURCES
};

/* Mouse look is on and controller 1 reads something: the window should
 * take the pointer (hide it and keep it inside) while it has the focus. */
int  recomp_mouse_look(void);
/* Relative motion in mouse counts, x right and y down. Only counted while
 * mouse look is on. */
void recomp_mouse_move(float dx, float dy);
void recomp_mouse_button(int button, int down);
/* The window lost the focus or the pointer: every button up, and motion
 * not yet read is dropped, so nothing is left held or moving. */
void recomp_mouse_release(void);
/* Wheel notches, positive away from the player (up). Each is a press of
 * the matching mouse:wheel_ source long enough for a title polling once a
 * frame to see it, then a release, then the next; a fast spin is queued
 * (up to a handful) rather than lost or merged into one long press. A
 * high-resolution wheel's fractions add up to whole notches. */
void recomp_mouse_wheel(float notches);
/* For tests: the milliseconds the wheel's presses are timed by, instead of
 * the host's clock (NULL puts it back), so a test steps time rather than
 * sleeping through it. */
void recomp_mouse_wheel_clock(unsigned long long (*clock_ms)(void));

/* ---- the mouse, for a title that aims with it itself --------------------
 *
 * A camera on the right stick turns at a speed; a mouse says how far. A
 * title that knows where its view angles are can take the mouse's motion
 * and turn by it directly, which is what mouse aim means on a PC. With
 * direct on, mouse look still takes the pointer, but adds nothing to the
 * right stick; the motion waits for recomp_mouse_take. */
void recomp_mouse_set_direct(int on);
/* The motion since the last take, in mouse counts, x right and y down;
 * 0 when there was none or mouse look is off. */
int  recomp_mouse_take(float *dx, float *dy);
/* The player's mouse sensitivity setting, RECOMP_MOUSE_SENSITIVITY_MIN..MAX
 * (recomp_config.h), for a title turning the motion into angles. */
int  recomp_mouse_sensitivity(void);

/* ---- a title's last word ------------------------------------------------
 *
 * Called at the end of every sample, after the bindings, mouse look and the
 * touch controls, with what the port reads (which it may change) and the
 * part of it the keyboard and mouse alone are pressing. For what one key
 * must mean differently by what is on screen: Escape, bound to Start,
 * backing out of a menu where Start would accept. */
typedef void (*RecompBindingsFilter)(unsigned port, XBOX_GAMEPAD *pad,
                                     const XBOX_GAMEPAD *from_keyboard);
void recomp_bindings_set_filter(RecompBindingsFilter filter);

#ifdef __cplusplus
}
#endif

#endif
