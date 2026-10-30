/*
 * recomp_mods.h - the toolkit's side of mods (recomp_mod.h is the mods').
 */
#ifndef RECOMP_MODS_H
#define RECOMP_MODS_H

/* Loads the mods beside the game, once; later calls do nothing. The HLE
 * calls it as the title makes its Direct3D device. */
void recomp_mods_load(void);

/* Once a frame, after it is shown: the mods' on_frame calls. */
void recomp_mods_frame(void);

#endif
