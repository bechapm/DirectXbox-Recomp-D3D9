/* xaudio2_mingw.h -- what audio_output_xaudio2.cpp uses from the Windows SDK's
 * xaudio2fx.h and xapofx.h that MinGW's versions of them leave out.
 *
 * A self-building release compiles for Windows with zig's MinGW headers, not
 * the Windows SDK (tools/release/fetch_toolchain.py). MinGW's xaudio2fx.h and
 * xapofx.h (from Wine) declare CreateAudioReverb, CreateAudioVolumeMeter and
 * the effects' CLSIDs, but not the parameter blocks, the SDK's inline
 * creators or ReverbConvertI3DL2ToNative; XAudio2_9.dll exports CreateFX,
 * CreateAudioReverb and CreateAudioVolumeMeter all the same.
 *
 * The parameter blocks are XAudio2 2.9's, packed to one byte as the SDK packs
 * them: XAudio2 checks the size it is given against its own.
 *
 * ReverbConvertI3DL2ToNative is adapted from FAudio's FAudioFX_reverb.c
 * (https://github.com/FNA-XNA/FAudio), under its zlib licence:
 *
 *   Copyright (c) 2011-2024 Ethan Lee, Luigi Auriemma, and the MonoGame Team
 *
 *   This software is provided 'as-is', without any express or implied
 *   warranty. In no event will the authors be held liable for any damages
 *   arising from the use of this software.
 *
 *   Permission is granted to anyone to use this software for any purpose,
 *   including commercial applications, and to alter it and redistribute it
 *   freely, subject to the following restrictions:
 *
 *   1. The origin of this software must not be misrepresented; you must not
 *   claim that you wrote the original software. If you use this software in a
 *   product, an acknowledgment in the product documentation would be
 *   appreciated but is not required.
 *
 *   2. Altered source versions must be plainly marked as such, and must not be
 *   misrepresented as being the original software.
 *
 *   3. This notice may not be removed or altered from any source distribution.
 */
#ifndef XBOXRECOMP_XAUDIO2_MINGW_H
#define XBOXRECOMP_XAUDIO2_MINGW_H

#if defined(__MINGW32__)
#include <math.h>
#include <windows.h>
#include <xaudio2.h>

#pragma pack(push, 1)

typedef struct XAUDIO2FX_VOLUMEMETER_LEVELS {
    float *pPeakLevels;
    float *pRMSLevels;
    UINT32 ChannelCount;
} XAUDIO2FX_VOLUMEMETER_LEVELS;

typedef struct XAUDIO2FX_REVERB_PARAMETERS {
    float WetDryMix;
    UINT32 ReflectionsDelay;
    BYTE ReverbDelay;
    BYTE RearDelay;
    BYTE SideDelay;
    BYTE PositionLeft;
    BYTE PositionRight;
    BYTE PositionMatrixLeft;
    BYTE PositionMatrixRight;
    BYTE EarlyDiffusion;
    BYTE LateDiffusion;
    BYTE LowEQGain;
    BYTE LowEQCutoff;
    BYTE HighEQGain;
    BYTE HighEQCutoff;
    float RoomFilterFreq;
    float RoomFilterMain;
    float RoomFilterHF;
    float ReflectionsGain;
    float ReverbGain;
    float DecayTime;
    float Density;
    float RoomSize;
    BOOL DisableLateField;
} XAUDIO2FX_REVERB_PARAMETERS;

typedef struct XAUDIO2FX_REVERB_I3DL2_PARAMETERS {
    float WetDryMix;
    INT32 Room;
    INT32 RoomHF;
    float RoomRolloffFactor;
    float DecayTime;
    float DecayHFRatio;
    INT32 Reflections;
    float ReflectionsDelay;
    INT32 Reverb;
    float ReverbDelay;
    float Diffusion;
    float Density;
    float HFReference;
} XAUDIO2FX_REVERB_I3DL2_PARAMETERS;

typedef struct FXMASTERINGLIMITER_PARAMETERS {
    UINT32 Release;
    UINT32 Loudness;
} FXMASTERINGLIMITER_PARAMETERS;

#pragma pack(pop)

#define FXMASTERINGLIMITER_DEFAULT_RELEASE  6
#define FXMASTERINGLIMITER_DEFAULT_LOUDNESS 1000

#define XAUDIO2FX_REVERB_MAX_REFLECTIONS_DELAY      300
#define XAUDIO2FX_REVERB_MAX_REVERB_DELAY           85
#define XAUDIO2FX_REVERB_DEFAULT_REAR_DELAY         5
#define XAUDIO2FX_REVERB_DEFAULT_7POINT1_SIDE_DELAY 5
#define XAUDIO2FX_REVERB_DEFAULT_7POINT1_REAR_DELAY 20
#define XAUDIO2FX_REVERB_DEFAULT_POSITION           6
#define XAUDIO2FX_REVERB_DEFAULT_POSITION_MATRIX    27
#define XAUDIO2FX_REVERB_DEFAULT_ROOM_SIZE          100.0f

/* The mastering limiter's class, as the SDK's __uuidof(FXMasteringLimiter):
 * MinGW's xapofx.h declares it with DEFINE_GUID, which needs a library of
 * GUIDs MinGW does not have. */
static const GUID XR_CLSID_FXMasteringLimiter =
    {0xc4137916, 0x2be1, 0x46fd, {0x85, 0x99, 0x44, 0x15, 0x36, 0xf4, 0x98, 0x56}};

extern "C" {
HRESULT __stdcall CreateFX(REFCLSID clsid, IUnknown **pEffect, const void *pInitDat,
                           UINT32 InitDataByteSize);
HRESULT __stdcall CreateAudioReverb(IUnknown **ppApo);
HRESULT __stdcall CreateAudioVolumeMeter(IUnknown **ppApo);
}

static inline HRESULT XAudio2CreateReverb(IUnknown **ppApo, UINT32 Flags = 0)
{
    (void)Flags;
    return CreateAudioReverb(ppApo);
}

static inline HRESULT XAudio2CreateVolumeMeter(IUnknown **ppApo, UINT32 Flags = 0)
{
    (void)Flags;
    return CreateAudioVolumeMeter(ppApo);
}

/* FAudio's ReverbConvertI3DL2ToNative9, with XAudio2 2.9's default of 7.1
 * delays and the late field left on. */
static inline void ReverbConvertI3DL2ToNative(const XAUDIO2FX_REVERB_I3DL2_PARAMETERS *pI3DL2,
                                              XAUDIO2FX_REVERB_PARAMETERS *pNative,
                                              BOOL sevenDotOneReverb = TRUE)
{
    float reflectionsDelay;
    float reverbDelay;

    pNative->RearDelay = sevenDotOneReverb ? XAUDIO2FX_REVERB_DEFAULT_7POINT1_REAR_DELAY
                                           : XAUDIO2FX_REVERB_DEFAULT_REAR_DELAY;
    pNative->SideDelay = XAUDIO2FX_REVERB_DEFAULT_7POINT1_SIDE_DELAY;
    pNative->PositionLeft = XAUDIO2FX_REVERB_DEFAULT_POSITION;
    pNative->PositionRight = XAUDIO2FX_REVERB_DEFAULT_POSITION;
    pNative->PositionMatrixLeft = XAUDIO2FX_REVERB_DEFAULT_POSITION_MATRIX;
    pNative->PositionMatrixRight = XAUDIO2FX_REVERB_DEFAULT_POSITION_MATRIX;
    pNative->RoomSize = XAUDIO2FX_REVERB_DEFAULT_ROOM_SIZE;
    pNative->LowEQCutoff = 4;
    pNative->HighEQCutoff = 6;

    pNative->RoomFilterMain = (float)pI3DL2->Room / 100.0f;
    pNative->RoomFilterHF = (float)pI3DL2->RoomHF / 100.0f;

    if (pI3DL2->DecayHFRatio >= 1.0f) {
        INT32 index = (INT32)(-4.0 * log10(pI3DL2->DecayHFRatio));
        if (index < -8)
            index = -8;
        pNative->LowEQGain = (BYTE)((index < 0) ? index + 8 : 8);
        pNative->HighEQGain = 8;
        pNative->DecayTime = pI3DL2->DecayTime * pI3DL2->DecayHFRatio;
    } else {
        INT32 index = (INT32)(4.0 * log10(pI3DL2->DecayHFRatio));
        if (index < -8)
            index = -8;
        pNative->LowEQGain = 8;
        pNative->HighEQGain = (BYTE)((index < 0) ? index + 8 : 8);
        pNative->DecayTime = pI3DL2->DecayTime;
    }

    reflectionsDelay = pI3DL2->ReflectionsDelay * 1000.0f;
    if (reflectionsDelay >= XAUDIO2FX_REVERB_MAX_REFLECTIONS_DELAY)
        reflectionsDelay = (float)(XAUDIO2FX_REVERB_MAX_REFLECTIONS_DELAY - 1);
    else if (reflectionsDelay <= 1)
        reflectionsDelay = 1;
    pNative->ReflectionsDelay = (UINT32)reflectionsDelay;

    reverbDelay = pI3DL2->ReverbDelay * 1000.0f;
    if (reverbDelay >= XAUDIO2FX_REVERB_MAX_REVERB_DELAY)
        reverbDelay = (float)(XAUDIO2FX_REVERB_MAX_REVERB_DELAY - 1);
    pNative->ReverbDelay = (BYTE)reverbDelay;

    pNative->ReflectionsGain = pI3DL2->Reflections / 100.0f;
    pNative->ReverbGain = pI3DL2->Reverb / 100.0f;
    pNative->EarlyDiffusion = (BYTE)(15.0f * pI3DL2->Diffusion / 100.0f);
    pNative->LateDiffusion = pNative->EarlyDiffusion;
    pNative->Density = pI3DL2->Density;
    pNative->RoomFilterFreq = pI3DL2->HFReference;

    pNative->WetDryMix = pI3DL2->WetDryMix;
    pNative->DisableLateField = FALSE;
}
#endif

#endif
