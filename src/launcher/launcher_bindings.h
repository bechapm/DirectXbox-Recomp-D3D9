/*
 * launcher_bindings.h -- the input config, as the launcher edits it.
 *
 * The runtime reads this same file (src/input/input_bindings.c) and
 * resolves every string to a key code or a pad index at load. The
 * launcher needs the strings themselves, to show them and to change
 * them, so it keeps its own view: the file's text, not the runtime's
 * resolved form.
 *
 * The vocabulary is the runtime's and nothing here may invent any of
 * it. A control the runtime does not know is a binding that silently
 * does nothing, which is worse than refusing to write it, so the names
 * below are the runtime's table copied deliberately and checked against
 * it by tools/input_ui/test_runtime_vocabulary.py.
 *
 * Bindings belong to the person, not the title: one file, shared by
 * every game the toolkit builds, in the place the runtime looks.
 */
#ifndef XBOXRECOMP_LAUNCHER_BINDINGS_H
#define XBOXRECOMP_LAUNCHER_BINDINGS_H

#include <stddef.h>
#include <stdint.h>

#define BIND_PORTS      4
#define BIND_CONTROLS   24
#define BIND_SOURCE_LEN 32

/* The 24 Xbox inputs, in the order the launcher lists them, by the names
 * the config file uses. */
extern const char *const bind_control_names[BIND_CONTROLS];
/* The same, as a player would read them. */
extern const char *const bind_control_labels[BIND_CONTROLS];

typedef enum { BIND_DEV_NONE, BIND_DEV_PAD, BIND_DEV_KEYBOARD } BindDevice;

typedef struct BindPort {
    BindDevice device;
    int        pad;                       /* pad slot, 0..3 */
    int        deadzone;
    /* One pad source and up to two keyboard-or-mouse sources ("key:" or
     * "mouse:") per control: the shape a player thinks in, with room for
     * the second key a layout sometimes wants (the mouse wheel and an
     * arrow both changing weapon). A capture sets key_src and clears
     * key_src2, so rebinding replaces. */
    char pad_src[BIND_CONTROLS][BIND_SOURCE_LEN];
    char key_src[BIND_CONTROLS][BIND_SOURCE_LEN];
    char key_src2[BIND_CONTROLS][BIND_SOURCE_LEN];
} BindPort;

/* Which API reads the pads, for every port and every game: SDL3 reads
 * every kind of controller, XInput only Xbox ones. The file's "pad_api". */
enum { BIND_PAD_API_SDL = 0, BIND_PAD_API_XINPUT = 1 };

typedef struct BindConfig {
    BindPort port[BIND_PORTS];
    int      pad_api;                     /* BIND_PAD_API_* */
} BindConfig;

/* The mapping the runtime falls back to with no file at all: the title's
 * own layout when it has one (recomp_title_input_defaults), the toolkit's
 * built-in one otherwise. */
void bind_defaults(BindConfig *c);

/* The file both ends agree on (recomp_input_config_path): RECOMP_INPUT_CONFIG
 * if set; a title with its own layout keeps its file to itself
 * (titles/<TITLEID>.input.json in the per-user folder); every other title
 * shares input_bindings.json there. */
int  bind_config_path(uint32_t title_id, char *out, size_t n);

/* Read and write that schema. A read that finds no file leaves the
 * defaults in place and returns 0. */
int  bind_load(const char *path, BindConfig *c);
int  bind_save(const char *path, const BindConfig *c);

/* A source string as a player would read it: "pad:lx+" -> "Left stick
 * left", "key:RETURN" -> "Enter", "" -> "Not bound". */
const char *bind_source_label(const char *src);
/* A control's keyboard-and-mouse column as a player would read it: one
 * source's label, or both joined ("Mouse wheel up, Left arrow"). */
const char *bind_keys_label(const BindPort *bp, int control);

#endif /* XBOXRECOMP_LAUNCHER_BINDINGS_H */
