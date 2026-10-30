# Controller bindings

*September 2026.*

Until now a recompiled title got one pad, on port 1, driven by XInput pad 0
and a keyboard map written into `src/hle/input_host.c`. Nothing could be
rebound, a second player could not be plugged in, and the only way to change a
key was to edit C and rebuild every title.

There are now three pieces:

| | |
|---|---|
| `src/input/input_bindings.c` | reads the config, samples the four ports |
| `tools/input_ui/` | the UI that writes the config (`py -3 -m tools.input_ui`) |
| `tools/input_ui/bindings.py` | the schema both sides speak, and its tests |

Every title built from this toolkit picks it up: the binding layer is part of
`xbox_input`, which `xbox_hle` and `xbox_usb` both link, so a title only has
to be rebuilt, not changed.

## The file

JSON, one entry per controller:

```json
{
  "version": 1,
  "pad_api": "sdl",
  "controllers": [
    {
      "port": 1,
      "device": "gamepad:0",
      "deadzone": 7849,
      "bindings": {
        "a": ["pad:a", "key:Z"],
        "start": "key:RETURN",
        "lstick_left": ["pad:lx-", "key:A"],
        "black": "none"
      }
    }
  ]
}
```

Ports are **1-4**, as they are written on the console. A controller the file
does not mention keeps its defaults, and so does a control the file does not
mention -- a file with one binding in it is a config with one binding changed.
That is deliberate: it makes the file hand-editable and a diff readable.

**`device`** is `keyboard`, `gamepad:0` .. `gamepad:3`, or `none`. It says where
`pad:` sources are read from. `key:` sources are always read, whatever the
device is, which is why the default port 1 answers to both a pad and the
keyboard at once. `xinput:0` .. `xinput:3` is what files written before SDL3
say, and names the same slot.

**`pad_api`** (Sep 2026) is which API reads every pad: `"sdl"`, the default, or
`"xinput"`. SDL3 reads Xbox pads and everything else -- DualSense and
DualShock 4, Switch Pro, 8BitDo, the Steam Deck, generic pads through SDL's
mapping database -- with hotplug and rumble; a pad takes the first free slot
when it is connected and keeps it until unplugged. XInput reads Xbox
controllers only, by XInput's user index, and is kept for a pad or machine
where SDL3 misbehaves. `RECOMP_PAD_API=sdl|xinput` overrides the file, and
the launcher's Input tab has it as "Controllers". Both go through
`src/input/recomp_pad.h`, which returns XInput's layout whichever API read
the pad; `recomp_pad_probe [seconds] [xinput]` (built on request, target
`recomp_pad_probe`) prints what it sees, live.

**A binding** is one source string or a list of up to four. Any of them
pressing the control presses it, and the strongest wins for anything analog,
so a trigger bound to both a key and a pad trigger behaves sensibly. `"none"`
or `[]` unbinds.

**Sources**

| form | meaning |
|---|---|
| `key:Z`, `key:7` | a letter or digit |
| `key:RETURN`, `key:LSHIFT`, `key:F8`, `key:SLASH` | a named key (the list is `KEYS[]` in `input_bindings.c`, mirrored in `bindings.py`) |
| `key:#191` | a raw Windows virtual-key code, for anything with no name |
| `pad:a` `pad:b` `pad:x` `pad:y` | face buttons on the host pad |
| `pad:lshoulder` `pad:rshoulder` | shoulder buttons (the Xbox's Black and White by default) |
| `pad:start` `pad:back` `pad:lthumb` `pad:rthumb` | the rest of the digital buttons |
| `pad:dpad_up` .. `pad:dpad_right` | the host D-pad |
| `pad:lt` `pad:rt` | analog triggers, 0-255 |
| `pad:lx+` `pad:lx-` `pad:ly+` .. `pad:ry-` | one half of one stick axis |
| `mouse:left` `mouse:right` `mouse:middle` | a mouse button, heard by the game's window (Oct 2026) |
| `mouse:x1` `mouse:x2` | the mouse's side buttons, back and forward |
| `mouse:wheel_up` `mouse:wheel_down` | a notch of the wheel: a 50 ms press, then a 40 ms release (Oct 2026) |

**Controls** are the 24 inputs an Xbox pad has: `a b x y black white start
back ltrigger rtrigger`, `dpad_up/down/left/right`, `lthumb`, `rthumb`, and
each stick as four halves -- `lstick_up/down/left/right` and `rstick_*`.

A stick is four halves rather than two axes because a key has no middle. It
also means every source, whatever it is, reads as one 0-255 magnitude:
a digital control fires above a quarter press, an analog button takes the
magnitude, and a stick axis is the positive half minus the negative one,
scaled to the signed 16 bits the guest reads. Binding a trigger to a key
gives a full pull; binding a stick half to a key gives a full deflection.

### Where it lives

Both sides look in the same places, in this order:

1. `RECOMP_INPUT_CONFIG`, if set -- an explicit path, and a run says so if it
   cannot be read rather than silently falling back;
2. `%APPDATA%\xboxrecomp\input_bindings.json` (on POSIX,
   `$XDG_CONFIG_HOME/xboxrecomp/input_bindings.json`) -- the per-user file the
   UI writes by default;
3. `input_bindings.json` in the working directory, which for a title started
   by its `run.bat` is the directory the executable sits in, so a title can
   ship a config of its own.

Bindings are per-user rather than per-title on purpose: they belong to the
person and their hardware, and a toolkit that builds several titles should not
make them rebind for each one. A title that genuinely needs its own gets one
through (3), and a test run gets one through (1).

### A title's own layout

*October 2026.* Which key should fire depends on the game, so a title can
ship its own layout: `recomp_title_input(DEFAULTS config/input_defaults.json)`
in its CMakeLists (src/config/CMakeLists.txt) compiles a file in this format
into `xbox_config`, which the game and its launcher both link. It is read
over the built-in layout below, and becomes what an unbound control reads and
what the launcher's reset gives back. A control the file names replaces the
built-in one whole, so name its pad source too.

Such a title keeps its player's bindings to itself, in
`titles/<TITLEID>.input.json` in the per-user folder, beside its settings
file, instead of the `input_bindings.json` every other game shares (its keys
mean something only in it), and it does not read a file in the working
directory. `RECOMP_INPUT_CONFIG` still beats both. The search order lives in
one place, `recomp_input_config_path` in `src/config/recomp_config.c`.
`MOUSE_LOOK` on the same line makes mouse look on by default for the title.

### Defaults, when there is no file

For a title without a layout of its own, exactly what the runtime did before
this existed, so nothing changes for a run that has not been configured:

- controller 1: XInput pad 0 **and** the keyboard -- arrows = D-pad,
  Enter = START, Backspace = BACK, Z = A, X = B, A = X, S = Y, Q = White,
  W = Black, E = left trigger, R = right trigger;
- controllers 2-4: XInput pad 1, 2, 3, pad only.

The sticks have no keyboard default. Every comfortable key around WASD is
already a button in that map, and guessing would have silently changed what
existing runs do.

## The mouse

*October 2026.* Two things, and they live in different files on purpose.

**Mouse buttons are sources.** `mouse:left` and the rest bind like any key,
on any port, and count as a key for whether a port is present. Only the game
window hears them, so `src/hle/hle_d3d8.c` passes them on: from its window
messages on Windows and from the SDL shell's events elsewhere
(`host_window_callbacks.on_mouse_button`). Unlike the keyboard on Windows,
which `GetAsyncKeyState` reads whatever is in front, a click in another
window is not a press. A window that loses the focus lets every button go.

**The wheel is two sources** (Oct 2026), `mouse:wheel_up` and
`mouse:wheel_down`. A wheel has no "held", so each notch is a press 50 ms
long followed by a 40 ms release: a title polling once a frame sees both,
and one counting presses counts one per notch. Notches faster than that wait
their turn, at most four of them, and a turn the other way drops what is
still waiting. The window passes `WM_MOUSEWHEEL` (focus only) on Windows and
SDL's wheel event elsewhere (`host_window_callbacks.on_mouse_wheel`) to
`recomp_mouse_wheel`, which adds a fine wheel's fractions into notches.

**Mouse look is a setting of the title, not a binding.** `mouse_look = 1` in
the title's settings file (`RECOMP_MOUSE_LOOK`; the launcher's Input tab has
it as a checkbox) moves **controller 1's right stick** with the mouse. It is
added after the bindings have been sampled, so whatever is bound to
`rstick_*` -- the pad's own stick by default -- still works, and a still
mouse adds nothing. Per title because a camera on the mouse is right for a
shooter and a surprise in a racing game; bindings are the person's, shared
by every game.

A stick says how fast to turn, so the push is proportional to the mouse's
speed: the counts since the last look over the time since, worked out again
at most every 12 ms whatever the title's polling rate, averaged with the
period before (a 125 Hz mouse lands one report in some periods and two in
others), and kept on the stick. The push is proportional from the centre:
there is no dead zone of our own and no starting push to get past the
title's, because any offset made a slow movement jump and broke the
mouse's feel (a `mouse_deadzone` setting, 5% by default, was removed in
Oct 2026; a settings file that still has the line is read without it). One
setting shapes it, a row on the launcher's Input tab, a line in the title's
settings file and an environment variable:

| setting | range | default | what it does |
|---|---|---|---|
| `mouse_sensitivity` (`RECOMP_MOUSE_SENSITIVITY`) | 1-100 | 80 | a full push at 100,000 / sensitivity counts a second, so 80 is 1,250 -- an unhurried turn on an 800 dpi mouse; each step is 1.25% of the default |

**Tuning it.** The default and range are `RECOMP_MOUSE_SENSITIVITY_*` in
`src/config/recomp_config.h` and nowhere else: the launcher's row, the
settings file's comments and the runtime all read them, so a better default
found by playing is a one-line change there. A setting already written to a
title's file keeps its value, so a new default reaches a player who has
never moved the row, and one who has keeps theirs. In the launcher, A or
Enter on the row puts it back to the default. To try values without the
launcher, set the environment variable, which beats the file; the
`[INPUT] mouse look:` line at start-up says which value a run got.

With it on, the window takes the pointer while it is in front: hidden and
kept inside it. On Windows the motion is raw input (`WM_INPUT`), which keeps
coming at the edge of the screen and has no pointer acceleration in it, and
the cursor is clipped to the client area; elsewhere it is SDL's relative
mouse mode. Switching away gives the pointer back; switching to the window,
or a click in it, takes it again; on Windows that click is not passed to the
title.
A `RECOMP_WINDOW_BACKGROUND=1` run never takes it. The `[INPUT] mouse look:`
line at start-up says it is on and with what; the first movement is named
like any first press.

**Direct mouse aim.** A stick is a speed, so the right stick's mouse look
always feels like a stick: the title's dead zone, its top turn speed and its
acceleration all still apply. A title that knows where its view angles are
calls `recomp_mouse_set_direct(1)`: the window still takes the pointer, the
right stick gets nothing, and the title takes the motion itself with
`recomp_mouse_take` (counts, x right, y down) and turns by it, with
`recomp_mouse_sensitivity` for the player's setting. TimeSplitters 2 does
this at the function that turns the player (its `src/ts2_mouse.c`).

**A title's last word.** `recomp_bindings_set_filter` runs after everything
else in a sample, with the pad as it will read and the part of it the
keyboard and mouse alone press. It is for what one key must mean differently
by what is on screen: TimeSplitters 2's Escape is bound to Start, which
pauses in a level, and the filter makes it B in the menus, where Start would
accept.

`tests/input_mouse` drives both through `recomp_bindings_sample` with no
window and no pad.

**A pointer for a title's menus** (Oct 2026). A console title's menus are
made for a pad, but a title that knows where its menu items are can let the
mouse point at them: while it calls `recomp_pointer_want(1)` the game window
gives the pointer back from mouse look and shows the system's arrow (in
fullscreen too), and the mouse's buttons and wheel go to the title through
`recomp_pointer_read` instead of to the bindings, so a click on a menu item
is not also a press of whatever the left button is bound to. Everything held
is let go at each switch. The pointer is given as 0..1 across the picture as
it is shown, bars left out (`xbox_D3D8PictureShape` says the picture's shape;
the window fits it to the client area as the renderer does), with the
picture's aspect, so a title with 4:3 menus in a 16:9 picture can measure
from the middle. On Windows the window's thread follows the pointer from
`WM_MOUSEMOVE` and its 30 ms key timer (so a picture that changes shape
under a still pointer is measured again); on the SDL shell
`host_window_pointer` keeps the free pointer's last position and the
renderer's thread reads it each frame. A background run's window never
reports a pointer; its title may move one itself with `recomp_pointer_set`
(TimeSplitters 2 does, for scripted tests).

## The UI

```
py -3 -m tools.input_ui                  # the per-user config
py -3 -m tools.input_ui path\to.json     # somewhere else
py -3 -m tools.input_ui --print          # what the runtime would load
py -3 -m tools.input_ui --write-defaults # a default file, no display needed
```

Four controllers down the left as dashboard blades; the selected one's device
across the top -- the keyboard, each XInput slot with whether a pad is
answering right now, or nothing; every control below it. Clicking a binding
(or `Bind`) opens a capture: press a key or a button and it is taken, `+` adds
a second source to the same control, Esc cancels, and the dialog has a button
to clear. `Reset this controller`, `Reset all four`, `Save`.

It is tkinter with every colour set by hand -- black field, `#0D0D0D` panels,
`#107C10` for structure and `#9BE04B` for anything with focus or a value. ttk
on Windows will not let a widget be black, and `tk.Scrollbar` keeps the
system's white trough whatever it is told, so the scrollbar is drawn here too.

Device detection is XInput through `ctypes` (`tools/input_ui/devices.py`): it
tries `xinput1_4`, `xinput1_3`, then `xinput9_1_0`, and asks each of the four
slots. DirectInput and raw HID are not read -- an Xbox-layout pad is what
XInput reports, and anything else would need a translation layer the toolkit
does not have. A pad that is not detected can still be bound and configured;
the port simply reports empty until it is plugged in.

Capture compares against the pad state taken when the dialog opened, so a
stick resting off centre or a trigger resting high does not bind itself.
A click on the capture box binds that mouse button (left, right or middle;
Tk cannot see the side buttons everywhere), and a click on its own buttons
does what they say.

The launcher's Input tab binds them too: its second column is "keyboard /
mouse", and while a binding there waits for its answer any mouse button,
the side ones included, and a turn of the wheel are taken. Escape can be
bound there: a tap binds it, holding it for most of a second gives up. In
the pad column Escape gives up at once, and so does a click. The column
holds up to two sources a control (shown "Mouse wheel up, Left arrow"),
the shape a title's layout sometimes wants; binding one replaces both.

## What the runtime does with it

`recomp_bindings_init()` runs once, from whichever input path is reached
first, and logs what it read:

```
[INPUT] bindings from C:\Users\...\AppData\Roaming\xboxrecomp\input_bindings.json
[INPUT]   controller 1: keyboard
[INPUT]   controller 2: nothing
[INPUT] XAPI input replaced by name: 1 pad(s); gamepad type 0x0028C054
[INPUT]   port 1 <- keyboard
[INPUT] XInputOpen port 1 (keyboard) -> handle 0x58490001
[INPUT] port 1 first press: start (keyboard)
```

That last line is named once per port, the first time anything is pressed on
it. A wrong binding is otherwise invisible -- the title just does not respond,
and there is no way to tell a mis-bound key from a title that is not reading
its pads at all.

`src/hle/hle_input.c` now reports **up to four ports**. A port is present when
its device is the keyboard, or when its XInput pad answers, or when it has
keyboard bindings as well (which is why the default port 1 is always there).
`XGetDevices` and `XGetDeviceChanges` re-check, so a pad plugged in while the
title runs arrives as an insertion the way the console reported one, and
`XInputGetState` samples the device bound to that handle's own port.

Sources are resolved to key codes and pad indices once, at load, so a poll
does no string work; a poll reads each distinct key once and its pad once. An
empty XInput slot is re-checked only once a second, because `XInputGetState`
on one costs about a millisecond and a title polls several times a frame --
that was true before this change too, and silently cost more than the game on
a machine with no pad.

`RECOMP_FAKE_INPUT` and `RECOMP_INPUT_SEQ` are unchanged and apply to
controller 1 after its bindings. They stand in for a person at the keyboard,
so what that person bound should not change them.

`src/usb/usb_gamepad.c` reports port 1 through the same bindings, so a title
that finds its pad through the emulated USB stack rather than the XAPI
replacement behaves the same.

## Tests

```
py -3 -m unittest discover -s tools -p "test_*.py" -t .
```

`tools/input_ui/test_bindings.py` covers the half that needs no device:
defaults, what a partial or broken file means, which sources are valid, and
that a saved config reloads identical.

`tools/input_ui/test_runtime_vocabulary.py` reads `CONTROLS`, `PAD_BUTTONS`,
`KEYS` and `DEFAULTS` out of `src/input/input_bindings.c` and fails if the
Python copies have drifted. Drift is the dangerous failure here: the UI would
write a name the runtime does not recognise, the file would save, and the
control would simply never fire.

## Verified

Burnout 2, built from this branch:

- with no config file: boots as before, one pad on port 1, the log naming the
  built-in defaults;
- with a config naming `keyboard` on port 1, `none` on port 2 and START bound
  to `key:F8`: the log named the file and the devices, and pressing F8 came
  through as `port 1 first press: start`.

## Not done

- **Hot reload.** The config is read once, at start-up. Re-reading it when the
  file changes would be a few lines, but a binding that changes mid-frame
  wants thinking about.
- **DirectInput and raw HID**, so a PlayStation or generic pad appears without
  a third-party translation layer.
- **Rumble** goes to the model and no further: nothing drives the host pad's
  motors yet, on any port.
- **Per-title overrides** beyond dropping a file next to the executable -- no
  profiles, no per-title directory in the per-user config.
- **The keyboard off Windows, live.** Pads go through SDL3 everywhere. Since
  Oct 2026 the keyboard does too off Windows: `input_bindings.c` reads
  `SDL_GetKeyboardState`, translating the config's virtual-key names to SDL
  scancodes (generic SHIFT/CTRL/ALT answer for either side, as on Windows).
  Unlike `GetAsyncKeyState` it only hears keys while the game's window has
  the focus, so a `RECOMP_WINDOW_BACKGROUND=1` run never does. The table has
  not been exercised with a person at the keyboard on macOS yet.
- **Memory units and other XPP devices.** Only gamepads are bound; the model
  still answers "nothing connected" for everything else.
