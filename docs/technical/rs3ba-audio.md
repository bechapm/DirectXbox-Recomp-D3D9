# Rainbow Six 3: Black Arrow -- the sounds that are never requested

**State, 12 Oct 2026: open, parked.** Black Arrow (title 0x55530037, XDK 5849,
`titles/rs3ba`) draws correctly and most of its sound plays, but three groups of
sounds are missing on macOS:

- the guns' own reports. What is heard is a short clip: the shared layer from
  `CommonPistols.SB2` / `CommonAssaultRiffles.SB2`, or the silenced MK23's own
  `Pistol_Mk23_Silenced.SB2`;
- the reloads;
- the night-vision switch.

Rainbow Six 3 runs the same engine on XDK 5659 and plays its weapon-specific
sounds (`Sub_MP5A4.SB2`, `Sub_P90.SB2`, `Gadgets_NightVision.SB2`) on the same
build.

## What is known

**Black Arrow never asks for these sounds.** Every buffer `Play` and every stream
packet is logged (`RECOMP_DSOUND_PLAY_LOG=1`, `RECOMP_DSOUND_STREAM_LOG=1`).
Tracing each played sound's data back to its `.SB2` bank shows no `Play` from
`Assault_G36K.SB2`, `Assault_L85A1.SB2` or the reload and night-vision banks, in
a session that fired, reloaded and switched vision. The sounds that follow a
vision press come from `int/HD_Stream.LS2`, the localised speech file, so they
are voice lines, not the effect.

**Ruled out, with the measurement:**

| Suspect | Why it is not that |
|---|---|
| Our output loses sound | A capture of the player's own session (`RECOMP_AUDIO_CAPTURE`) holds exactly what they heard. Each submitted voice is whole: the MK23 shot is a 30 ms crack and a 250 ms tail, as its bank has it. |
| Stream table / voices | Fixed separately in f00c870: the title makes about 850 streams, so streams now pool their voices. The missing sounds did not come back with it. |
| Banks not loaded | `RECOMP_READ_NAMES=1`: `Assault_G36K.SB2` and the others are read in full, from a `Z:` cache that is byte-identical to the disc. |
| Hardware voice budget | The XDK hands voices out in software (192 2D at 0x004B506C, 64 3D at 0x004B5070, read from its own code). Every stream is made with `0x40000` (LOCDEFER), and the counters stay at 187/64 all run. |
| Creation failing | `CreateSoundBuffer` and `CreateSoundStream` failures are now logged (`[DSOUND] ... failed`). None appear. |
| A DirectSound call failing | The engine's own result check, `sub_00036610`, sees no failure (`RECOMP_RS3_SND_LOG=1`, `titles/rs3ba/src/recomp_manual.c`). |
| "Resource not loaded" | The engine's refusal messages, formatted through `sprintf` and dropped, never fire (same switch). |
| `PlayEx` / `StopEx` unreplaced | Black Arrow does not link `PlayEx`. Every caller of `CMcpxBuffer_Play` is either our replaced `Play` or the XDK's own hardware layer. |
| Speaker configuration | Reported as stereo (`XC_AUDIO` = 0); the channel check only prints. |

xemu was compared on its APU, and Cxbx-Reloaded on its list of DirectSound
patches. Neither gap fits: the title cannot read the voice budget, and the
functions Cxbx patches that we do not are either absent from this XBE or not on
these sounds' path.

## Where to look next

The decision is made above DirectSound: in the game code that asks the SND
engine for a sound, or in the engine's choice of what to play. One plausible
mechanism (not yet evidence) is audibility culling. The engine could compute
the weapon's 3D sound as inaudible, from its own distance and volume maths, and
skip it, while 2D layers play. That would put a fault in lifted maths (x87
precision first, per CLAUDE.md) on exactly these sounds. Starting points:

- The engine's `Play` call site is at 0x0003C28D, and its buffer wrapper is
  `sub_0003B5C0`. Sound resources are set up in `sub_00037600`, and
  `sub_0002FF70` is the "not loaded" check.
- Most direct: run Black Arrow in xemu with its GDB stub, break on the SND
  request path when firing the G36K, and diff the decision against a lifted run
  (CLAUDE.md, "Reference oracle").
