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
    RECOMP_MOUSE_BUTTONS
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

#ifdef __cplusplus
}
#endif

#endif
