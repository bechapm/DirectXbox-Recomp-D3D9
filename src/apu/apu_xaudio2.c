/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>   /* getenv: RECOMP_MUTE */
#include "apu_xaudio2.h"

/* XAudio2 on Windows; SDL3 audio with the same limits everywhere else. */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024   /* ~21ms per submission */
#define XA2_NUM_BUFS      3

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][2];
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;

int xa2_init(void)
{
    HRESULT hr;
    int com_initialized;
    WAVEFORMATEX wfx = { 0 };

    if (g_xa2_initialized) return 1;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }
    {
        /* RECOMP_MUTE: silent, with the voice still running at its own pace
         * (see src/hle/audio_output_xaudio2.cpp). */
        const char *mute = getenv("RECOMP_MUTE");
        if (mute && *mute && strcmp(mute, "0") != 0)
            IXAudio2MasteringVoice_SetVolume(g_xa2_master, 0.0f, XAUDIO2_COMMIT_NOW);
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] Start failed: 0x%08lX\n", hr);
        goto fail;
    }

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return 1;

fail:
    xa2_shutdown();
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

void xa2_shutdown(void)
{
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. Returns 1 if buffer was submitted. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;
    HRESULT hr;

    if (!g_xa2_initialized || !g_xa2_source) return 0;

    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if ((int)state.BuffersQueued >= XA2_NUM_BUFS) return 0;

    idx = g_xa2_next_buf;
    copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
    memcpy(g_xa2_bufs[idx], samples, copy_samples * XA2_CHANNELS * sizeof(int16_t));

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * XA2_CHANNELS * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
    if (FAILED(hr)) return 0;

    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    g_xa2_frames_written++;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

#else /* !_WIN32 -- SDL3 output, the same contract */

/* The monitor mix goes to an SDL3 audio stream on the default device: 48 kHz
 * stereo 16-bit, at most XA2_NUM_BUFS submissions of XA2_BUF_SAMPLES waiting,
 * the XAudio2 voice's limits above, so the device paces the APU frame the
 * same way. RECOMP_MUTE=1 plays at device gain 0 with the device still
 * pulling at its own rate. */
#include <SDL3/SDL.h>

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024
#define XA2_NUM_BUFS      3
#define XA2_BUF_BYTES     (XA2_BUF_SAMPLES * XA2_CHANNELS * (int)sizeof(int16_t))

#include <pthread.h>

/* The device opens on a host thread of its own (open_thread): CoreAudio can
 * take seconds to open one (15 s, then failure, has been seen), and the
 * caller is the title's start-up. xa2_init answers at once, the monitor mix
 * runs as usual, and its submissions are refused until the stream is
 * published -- or for ever, if the open fails, which is said once. */
static pthread_mutex_t   g_xa2_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioDeviceID g_sdl_device;
static SDL_AudioStream  *g_sdl_stream;     /* published with release order */
static int               g_xa2_initialized;
static int               g_xa2_closed;
static int               g_xa2_frames_written;

void w32_thread_unpin(void);   /* win32_compat.h */

static void *open_thread(void *arg)
{
    SDL_AudioSpec spec;
    SDL_AudioDeviceID dev;
    SDL_AudioStream *stream;
    const char *mute = getenv("RECOMP_MUTE");

    (void)arg;
    /* Started from a guest thread: SDL's audio threads start from this one
     * and would share the guest's core (RECOMP_GUEST_ONE_CPU). */
    w32_thread_unpin();
    /* SDL's audio brings up its event subsystem; keep SIGINT/SIGTERM fatal. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "[XA2] SDL audio failed to start: %s; no audio output\n", SDL_GetError());
        return NULL;
    }
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = XA2_CHANNELS;
    spec.freq = XA2_SAMPLE_RATE;
    dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
    if (!dev) {
        fprintf(stderr, "[XA2] SDL_OpenAudioDevice failed: %s; no audio output\n", SDL_GetError());
        return NULL;
    }
    /* Muted before anything is bound, and no output at all if muting fails:
     * a run that asked for silence must never be heard. */
    if (mute && *mute && strcmp(mute, "0") != 0 && !SDL_SetAudioDeviceGain(dev, 0.0f)) {
        fprintf(stderr, "[XA2] RECOMP_MUTE: could not mute the device (%s); no audio output\n",
                SDL_GetError());
        SDL_CloseAudioDevice(dev);
        return NULL;
    }
    stream = SDL_CreateAudioStream(&spec, NULL);
    if (!stream || !SDL_BindAudioStream(dev, stream)) {
        fprintf(stderr, "[XA2] SDL audio stream failed: %s; no audio output\n", SDL_GetError());
        if (stream) SDL_DestroyAudioStream(stream);
        SDL_CloseAudioDevice(dev);
        return NULL;
    }
    pthread_mutex_lock(&g_xa2_lock);
    if (g_xa2_closed) {                        /* shut down while this was opening */
        pthread_mutex_unlock(&g_xa2_lock);
        SDL_DestroyAudioStream(stream);
        SDL_CloseAudioDevice(dev);
        return NULL;
    }
    g_sdl_device = dev;
    __atomic_store_n(&g_sdl_stream, stream, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_xa2_lock);
    fprintf(stderr, "[XA2] SDL3 %s audio initialized (%d Hz stereo 16-bit, %d x %d-sample buffers)\n",
            SDL_GetCurrentAudioDriver(), XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return NULL;
}

int xa2_init(void)
{
    pthread_t thread;
    pthread_attr_t attr;
    int ok;

    pthread_mutex_lock(&g_xa2_lock);
    if (g_xa2_initialized) {
        pthread_mutex_unlock(&g_xa2_lock);
        return 1;
    }
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    ok = pthread_create(&thread, &attr, open_thread, NULL) == 0;
    pthread_attr_destroy(&attr);
    if (ok) {
        g_xa2_initialized = 1;
        g_xa2_closed = 0;
        g_xa2_frames_written = 0;
    } else {
        fprintf(stderr, "[XA2] could not start the audio thread; no audio output\n");
    }
    pthread_mutex_unlock(&g_xa2_lock);
    return ok;
}

void xa2_shutdown(void)
{
    SDL_AudioStream *stream;

    pthread_mutex_lock(&g_xa2_lock);
    stream = __atomic_exchange_n(&g_sdl_stream, NULL, __ATOMIC_ACQ_REL);
    if (stream) SDL_DestroyAudioStream(stream);
    if (g_sdl_device) SDL_CloseAudioDevice(g_sdl_device);
    g_sdl_device = 0;
    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
    g_xa2_closed = 1;
    pthread_mutex_unlock(&g_xa2_lock);
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    int copy_samples, ok = 0;
    SDL_AudioStream *stream;

    /* Under the lock, so xa2_shutdown cannot destroy the stream while it is
     * being written (one uncontended lock per 1024-sample buffer). */
    pthread_mutex_lock(&g_xa2_lock);
    stream = g_sdl_stream;
    /* Not open (yet), or what the device has not yet pulled is three
     * buffers in flight already. */
    if (g_xa2_initialized && stream &&
        SDL_GetAudioStreamQueued(stream) < XA2_NUM_BUFS * XA2_BUF_BYTES) {
        copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
        if (SDL_PutAudioStreamData(stream, samples,
                                   copy_samples * XA2_CHANNELS * (int)sizeof(int16_t))) {
            g_xa2_frames_written++;
            ok = 1;
        }
    }
    pthread_mutex_unlock(&g_xa2_lock);
    return ok;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

#endif /* _WIN32 */
