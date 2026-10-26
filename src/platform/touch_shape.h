/*
 * touch_shape.h - one on-screen control as the renderer draws it.
 *
 * Shared by src/input's touch controls, which say where the controls are and
 * what is held (touch_controls.h), and src/d3d, which draws them over the
 * finished frame (d3d8_touch.h); neither library links the other.
 */
#ifndef XBOXRECOMP_TOUCH_SHAPE_H
#define XBOXRECOMP_TOUCH_SHAPE_H

enum {
    TOUCH_SHAPE_STICK = 1,      /* ring of radius r, knob at (x + kx, y + ky) */
    TOUCH_SHAPE_BUTTON,         /* disc of radius r */
    TOUCH_SHAPE_PILL,           /* rounded rectangle w x h, corner radius r */
    TOUCH_SHAPE_DPAD,           /* a cross inside radius r; `pressed` holds the directions */
};

/* In pixels of the screen it was made for. */
typedef struct {
    int      kind;
    float    x, y;              /* centre */
    float    r;
    float    w, h;              /* pill only */
    float    kx, ky;            /* stick only */
    unsigned rgb;               /* 0xRRGGBB */
    unsigned pressed;           /* non-zero when held; the d-pad's XBOX_GAMEPAD_DPAD_* bits
                                 * (up 1, down 2, left 4, right 8) */
} TouchShape;

#endif
