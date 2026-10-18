/*
 * audio_output_sdl.c -- SDL3 output for replaced DirectSound buffers, the
 * host side of audio_output.h on every platform but Windows.
 *
 * The same contract and the same switches as audio_output_xaudio2.cpp, which
 * is the reference; where it says something below, this does the same:
 *
 *   - RECOMP_AUDIO_GAIN (0..1, absent = 1): the host master gain. 0 opens no
 *     device at all, so nothing reports a play position and the streams fall
 *     back to their own clocks. An invalid value also opens none.
 *   - RECOMP_MUTE=1: the device opens and plays at gain 0, so the play
 *     position is the device's own, exactly as an audible run's would be.
 *   - Each slot (buffers, streams, then the movie: audio_output.h) is its own
 *     voice, here an SDL_AudioStream bound to one device; SDL mixes them.
 *   - At most kQueueSize submissions in flight per voice. A full queue
 *     refuses (returns 0) and the stream offers the chunk again, never steps
 *     over it (CLAUDE.md, "Audio de-sync was dropped sound").
 *   - recomp_audio_output_position is the device's clock: bytes the device
 *     has pulled from the voice since it was created, and the submissions
 *     not yet wholly pulled.
 *
 * SDL's stream does its own conversion and resampling to the device format,
 * so a voice is just a source format, a gain, and a FIFO of the submission
 * sizes (the stream holds the bytes).
 */
#include "audio_output.h"
#include "audio_speakers.h"
#include "audio_health.h"
#include "audio_reverb.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VOICE_COUNT     RECOMP_AUDIO_VOICE_COUNT   /* buffers, streams, movie: audio_output.h */
#define QUEUE_SIZE      64u          /* audio_output_xaudio2.cpp kQueueSize */
#define MAX_BUFFER      160000u
#define MIN_RATE        1000u
#define MAX_RATE        200000u
#define RESAMPLER_SLACK_FRAMES 8u

typedef struct {
    SDL_AudioStream *stream;
    /* The voice's reverb send, for the native path's room (the bus below):
     * the same submissions at the same rate and ratio as the voice, so SDL
     * resamples the two alike and the send reaches the room in step with
     * the dry sound. NULL until the voice has a send and there is a room. */
    SDL_AudioStream *send;
    uint32_t sample_rate, channels, bits_per_sample;
    uint64_t submitted;              /* bytes put into the stream, in the caller's format */
    uint64_t chunk_end[QUEUE_SIZE];  /* FIFO: cumulative end of each submission */
    uint32_t front, queued;
    /* The stream takes 5.1 float frames made from the caller's PCM with the
     * slot's mix applied (recomp_audio_output_set_mix), so the bytes SDL
     * holds are MIX_FRAME_BYTES per caller frame. */
    int      mixed;
    uint32_t mix_applied;
    /* RECOMP_AUDIO_METER: submissions that found the voice already empty
     * (it ran dry: a gap in that voice), and the last two samples sent, to
     * see whether the next submission joins on smoothly. */
    uint32_t dry, joins, bad_joins, gain_steps, big_gain_steps, remade, level_steps;
    float    last_levels[2][RECOMP_SPEAKERS];
    float    last_gain;
    int16_t  tail[2];
    int      have_tail;
} voice;

/* recomp_audio_output_set_mix / _route, per slot (see the XAudio2 file),
 * and the slot's filter state. */
typedef struct {
    int      set;
    RecompAudioMix mix;
    uint32_t generation;
    uint32_t route;                  /* mix-in submix key, 0 = speakers */
    float    svf_low[2], svf_band[2];
} slot_mix;

/* A mix-in buffer's submix. Mixing is linear, so a voice routed into one is
 * the voice's channels taken through the submix's levels and gain: composed
 * per voice here, with no second mixing stage. */
typedef struct {
    uint32_t key, channels;
    RecompAudioMix mix;
    float gain;
} submix;
#define SUBMIXES 32u

/* Every voice is handed to SDL as float frames in the device's own channel
 * layout, with its mix and the fold-down (audio_speakers.h) already applied:
 * SDL's 5.1-to-stereo conversion would play a mixed voice 10 dB under an
 * unmixed one, and a voice whose format changed with its mix had to be
 * remade, losing what it had queued. Set when the device opens, before any
 * voice exists. */
static uint32_t g_out_channels = 2u;
static float    g_fold[RECOMP_SPEAKERS][RECOMP_FOLD_MAX_CHANNELS];
#define MIX_FRAME_BYTES (g_out_channels * 4u)
#define MAX_RATIO       8.0f

static pthread_mutex_t   g_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioDeviceID g_device;
static voice             g_voices[VOICE_COUNT];
/* Voices let go of by release_voice, playing out what they hold. */
#define DRAINING_MAX 64u
static SDL_AudioStream  *g_draining[DRAINING_MAX];
static slot_mix          g_mixes[VOICE_COUNT];
static submix            g_submixes[SUBMIXES];
static int               g_reported_nonzero[VOICE_COUNT];
static int               g_attempted, g_summary_printed;
static unsigned long long g_submitted_buffers, g_submitted_bytes, g_nonzero_buffers;
static unsigned long long g_dropped_buffers;

static void drop_buffer(uint32_t slot, const char *reason)
{
    ++g_dropped_buffers;
    recomp_audio_health_refused();
    if (g_dropped_buffers <= 4 || (g_dropped_buffers & (g_dropped_buffers - 1)) == 0)
        fprintf(stderr, "[audio-output] dropped=%llu slot=%u reason=%s\n",
                g_dropped_buffers, slot, reason);
}

static void destroy_voice(voice *v)
{
    if (v->stream)
        SDL_DestroyAudioStream(v->stream);     /* unbinds it from the device */
    if (v->send)
        SDL_DestroyAudioStream(v->send);
    memset(v, 0, sizeof *v);
}

/* Bytes the device has taken from the voice. The stream reports what is
 * still waiting in the source format, so the rest has been pulled. */
static uint64_t voice_played(voice *v)
{
    int queued_bytes = v->stream ? SDL_GetAudioStreamQueued(v->stream) : 0;
    uint32_t frame = v->channels * (v->bits_per_sample / 8);
    uint64_t waiting;

    if (queued_bytes < 0)
        queued_bytes = 0;
    /* In the caller's bytes: a mixed stream holds 5.1 float frames. */
    waiting = v->mixed ? (uint64_t)queued_bytes / MIX_FRAME_BYTES * frame
                       : (uint64_t)queued_bytes;
    /* SDL's resampler keeps its last few input frames (at most 7,
     * RESAMPLER_MAX_PADDING_FRAMES) until more arrive, so a voice that has
     * run dry never reaches zero and its last submission would never
     * complete. XAudio2 plays those out; count them as played. */
    if (waiting <= RESAMPLER_SLACK_FRAMES * frame)
        waiting = 0;
    return waiting >= v->submitted ? 0 : v->submitted - waiting;
}

static const submix *find_submix(uint32_t key)
{
    uint32_t i;
    for (i = 0; key && i < SUBMIXES; i++)
        if (g_submixes[i].key == key)
            return &g_submixes[i];
    return NULL;
}

/* Whether the slot plays through a mix, and which: its own, or its own pitch
 * and filter with the levels of the submix it is routed to. */
static int effective_mix(uint32_t slot, uint32_t channels, RecompAudioMix *out)
{
    const slot_mix *m = &g_mixes[slot];
    const submix *sub = find_submix(m->route);
    uint32_t c, o, s;

    if (!m->set && !sub)
        return 0;
    if (m->set) {
        *out = m->mix;
    } else {
        memset(out, 0, sizeof *out);
        out->ratio = 1.0f;
    }
    if (sub) {
        memset(out->levels, 0, sizeof out->levels);
        memset(out->reverb, 0, sizeof out->reverb);
        for (c = 0; c < channels && c < 2u; c++)
            for (o = 0; o < sub->channels && o < 2u; o++) {
                float into;
                if (!(channels == 1u || sub->channels == 1u || o == c))
                    continue;
                into = (sub->channels == 1u && channels == 2u) ? 0.5f : 1.0f;
                for (s = 0; s < RECOMP_SPEAKERS; s++)
                    out->levels[c][s] += into * sub->mix.levels[o][s] * sub->gain;
            }
    }
    return 1;
}

/* ── The native path's room ──────────────────────────────────────
 *
 * SDL mixes each voice as a stream of its own, so there is no one place
 * where the voices' reverb sends add up -- and a send summed when it is
 * submitted would reach the room before the dry sound, by however much the
 * voice has queued. So the sends are streams too (voice.send), bound to a
 * second logical device on the same output: SDL mixes them in device time,
 * as it mixes the voices, and that device's post-mix runs the room over the
 * summed send and replaces it with the wet sound. Nothing of the dry send is
 * ever heard. The bus is mixed after the main device's post-mix, so the
 * limiter does not see the wet sound. SDL applies a logical device's gain to
 * its streams before its post-mix, so the bus keeps the device's gain and a
 * muted run (RECOMP_MUTE) silences the wet sound in the post-mix instead,
 * after the room has run -- as the main device's post-mix does.
 *
 * The send streams carry the send in channel 0 of the device's layout, so
 * SDL does no channel conversion on the way to the bus. */
static SDL_AudioDeviceID g_bus;
static int               g_post_muted;          /* defined with the post-mix below */
static RecompReverb     *g_bus_reverb;
static pthread_mutex_t   g_bus_lock = PTHREAD_MUTEX_INITIALIZER;   /* g_bus_reverb only */
static int               g_bus_room;                              /* a room to hear */
/* What reached the bus and what left it (tests/audio_output): frames with a
 * send in them, frames with wet sound, and when each was first seen. */
static uint64_t g_bus_send_frames, g_bus_wet_frames, g_bus_first_send_ns, g_bus_first_wet_ns;

void recomp_audio_output_reverb_stats(uint64_t *send_frames, uint64_t *wet_frames,
                                      uint64_t *first_send_ns, uint64_t *first_wet_ns)
{
    pthread_mutex_lock(&g_bus_lock);
    if (send_frames) *send_frames = g_bus_send_frames;
    if (wet_frames) *wet_frames = g_bus_wet_frames;
    if (first_send_ns) *first_send_ns = g_bus_first_send_ns;
    if (first_wet_ns) *first_wet_ns = g_bus_first_wet_ns;
    pthread_mutex_unlock(&g_bus_lock);
}

static void SDLCALL bus_postmix(void *userdata, const SDL_AudioSpec *spec, float *buffer, int buflen)
{
    static float in[512], wet[2 * 512];
    uint32_t n = spec->channels > 0 ? (uint32_t)spec->channels : 2u;
    uint32_t frames = (uint32_t)buflen / (n * (uint32_t)sizeof(float)), done = 0, f;

    (void)userdata;
    pthread_mutex_lock(&g_bus_lock);
    while (done < frames) {
        uint32_t chunk = frames - done < 512u ? frames - done : 512u;
        float *b = buffer + (size_t)done * n;
        for (f = 0; f < chunk; f++)
            in[f] = b[(size_t)f * n];
        memset(wet, 0, sizeof(float) * 2u * chunk);
        if (recomp_reverb_active(g_bus_reverb))
            recomp_reverb_process(g_bus_reverb, in, 1, wet, wet + 1, 2, chunk);
        for (f = 0; f < chunk; f++) {
            if (fabsf(in[f]) > 1e-6f) {
                g_bus_send_frames++;
                if (!g_bus_first_send_ns) g_bus_first_send_ns = SDL_GetTicksNS();
            }
            if (fabsf(wet[2 * f]) + fabsf(wet[2 * f + 1]) > 1e-6f) {
                g_bus_wet_frames++;
                if (!g_bus_first_wet_ns) g_bus_first_wet_ns = SDL_GetTicksNS();
            }
        }
        if (g_post_muted)
            memset(wet, 0, sizeof(float) * 2u * chunk);
        memset(b, 0, sizeof(float) * n * chunk);
        for (f = 0; f < chunk; f++) {
            if (n >= 2) {
                b[(size_t)f * n] = wet[2 * f];
                b[(size_t)f * n + 1] = wet[2 * f + 1];
            } else {
                b[f] = 0.70710678f * (wet[2 * f] + wet[2 * f + 1]);
            }
        }
        done += chunk;
    }
    pthread_mutex_unlock(&g_bus_lock);
}

/* Opens the bus beside the main device; 0 leaves the native path dry. */
static SDL_AudioDeviceID bus_open(float gain)
{
    SDL_AudioDeviceID bus = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
    if (!bus)
        return 0;
    if (!SDL_SetAudioPostmixCallback(bus, bus_postmix, NULL)) {
        SDL_CloseAudioDevice(bus);
        return 0;        /* the dry sends would be heard: no bus at all */
    }
    SDL_SetAudioDeviceGain(bus, gain);
    return bus;
}

/* The caller's PCM as float frames in the device's layout with the slot's
 * mix applied: the voice's low-pass (a state-variable filter, as the
 * console's and XAudio2's are) on each channel, then its speaker levels,
 * folded onto the device's channels. */
static float *mix_pcm(slot_mix *m, const RecompAudioMix *mix, const uint8_t *pcm,
                      uint32_t bytes, uint32_t channels, uint32_t bits, uint32_t *out_bytes,
                      float **send_out)
{
    static float *buf, *sendbuf;
    static uint32_t cap, sendcap;
    uint32_t frame = channels * (bits / 8), frames = bytes / frame, f, c, s;
    uint32_t need = frames * MIX_FRAME_BYTES;

    if (need > cap) {
        float *grown = (float *)realloc(buf, need);
        if (!grown)
            return NULL;
        buf = grown;
        cap = need;
    }
    float into[2][RECOMP_FOLD_MAX_CHANNELS];
    uint32_t n = g_out_channels, k;
    float *sendp = NULL;

    /* The reverb send, filtered as the dry path is, in channel 0. */
    if (send_out) {
        if (need > sendcap) {
            float *grown = (float *)realloc(sendbuf, need);
            if (!grown)
                return NULL;
            sendbuf = grown;
            sendcap = need;
        }
        memset(sendbuf, 0, need);
        sendp = sendbuf;
        *send_out = sendbuf;
    }

    for (c = 0; c < 2u; c++)
        for (k = 0; k < n; k++) {
            into[c][k] = 0.0f;
            for (s = 0; s < RECOMP_SPEAKERS; s++)
                into[c][k] += mix->levels[c][s] * g_fold[s][k];
        }
    for (f = 0; f < frames; f++) {
        float *o = buf + (size_t)f * n;
        for (k = 0; k < n; k++)
            o[k] = 0.0f;
        for (c = 0; c < channels && c < 2u; c++) {
            float x = bits == 8u ? ((float)pcm[f * frame + c] - 128.0f) / 128.0f
                                 : (float)(int16_t)(pcm[f * frame + c * 2u] |
                                                    pcm[f * frame + c * 2u + 1u] << 8) / 32768.0f;
            if (mix->filter.low_pass) {
                float high = x - m->svf_low[c] - mix->filter.one_over_q * m->svf_band[c];
                m->svf_band[c] += mix->filter.frequency * high;
                m->svf_low[c] += mix->filter.frequency * m->svf_band[c];
                x = m->svf_low[c];
            }
            for (k = 0; k < n; k++)
                o[k] += x * into[c][k];
            if (sendp)
                sendp[(size_t)f * n] += x * mix->reverb[c];
        }
    }
    *out_bytes = need;
    return buf;
}

static void retire_played(voice *v)
{
    uint64_t played = voice_played(v);

    while (v->queued && v->chunk_end[v->front] <= played) {
        v->front = (v->front + 1) % QUEUE_SIZE;
        v->queued--;
    }
}

/* Opens the device, on a host thread of its own (open_device_thread).
 *
 * Not on the caller's: the first submission comes from a guest thread inside
 * an HLE call, which holds the guest lock, and CoreAudio can take seconds to
 * open a device (15 s, then failure, has been seen) -- every other guest
 * thread and the vblank ISR would wait that long. Until the device is up,
 * submissions are refused (a stream offers them again; audio_output.h) and
 * there is no play position, so the streams run on their own clock, exactly
 * as before the first voice exists. */
static float g_open_gain;

/* The mix, after SDL has summed every voice and before the device: one place
 * for what the XAudio2 backend does on its mastering voice, and for the
 * diagnostics that need the finished sound.
 *
 *   - A limiter, on by default (RECOMP_AUDIO_LIMITER=0 for none), as the
 *     XAudio2 file's FXMasteringLimiter: unity below full scale, and above it
 *     the gain drops at once to the frame's peak and recovers over about
 *     6 ms (FXMASTERINGLIMITER_DEFAULT_RELEASE), instead of the hard clip SDL
 *     does when it converts the float mix. Rainbow Six 3's firefights summed
 *     past full scale and crackled.
 *   - RECOMP_AUDIO_METER=1: every five seconds, the peak, the frames past full
 *     scale before the limiter, and the frames that were silent.
 *   - RECOMP_AUDIO_CAPTURE=<file.wav>: the mix as heard (after the limiter),
 *     32-bit float WAV, for listening to or measuring a run that was muted.
 *   - RECOMP_MUTE: done here, by zeroing the mix after the capture and the
 *     meter, so a muted run's mix is still there to measure. The device still
 *     pulls every voice at its own rate, so play positions are unchanged.
 * Runs on SDL's audio thread. */
static int      g_post_muted, g_post_limit = 1, g_post_meter;
static float    g_limit_gain = 1.0f, g_limit_release = 1.0f;
static FILE    *g_capture;
static uint64_t g_capture_bytes;
static uint32_t g_capture_rate, g_capture_channels;
static uint64_t g_meter_frames, g_meter_over, g_meter_silent;
static uint32_t g_remade_rate, g_remade_mixed, g_remade_queued;
static double   g_cut_ms, g_drained_ms;      /* sound thrown away / played out at a stop */
static uint32_t g_cuts, g_drains;
static float    g_meter_peak;
static Uint64   g_meter_last;

static void capture_header(FILE *f, uint32_t rate, uint32_t channels, uint64_t bytes)
{
    uint32_t data = bytes > 0xFFFFFFF0u ? 0xFFFFFFF0u : (uint32_t)bytes;
    uint32_t riff = data + 36u, byte_rate = rate * channels * 4u, fmt_len = 16u;
    uint16_t fmt_tag = 3u, ch = (uint16_t)channels, align = (uint16_t)(channels * 4u), bits = 32u;

    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_len, 4, 1, f); fwrite(&fmt_tag, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fseek(f, 0, SEEK_END);
}

static void SDLCALL postmix(void *userdata, const SDL_AudioSpec *spec, float *buffer, int buflen)
{
    int channels = spec->channels > 0 ? spec->channels : 2;
    int frames = buflen / (int)sizeof(float) / channels, i, c;

    (void)userdata;
    for (i = 0; i < frames; i++) {
        float *f = buffer + (size_t)i * channels, peak = 0.0f;

        for (c = 0; c < channels; c++) {
            float a = fabsf(f[c]);
            if (a > peak) peak = a;
        }
        if (peak > g_meter_peak) g_meter_peak = peak;
        if (peak > 1.0f) g_meter_over++;
        if (peak < 1e-6f) g_meter_silent++;
        if (g_post_limit) {
            float want = peak > 1.0f ? 1.0f / peak : 1.0f;
            if (want < g_limit_gain)
                g_limit_gain = want;                         /* at once */
            else
                g_limit_gain += (1.0f - g_limit_gain) * g_limit_release;
            if (g_limit_gain < 1.0f)
                for (c = 0; c < channels; c++)
                    f[c] *= g_limit_gain;
        }
    }
    g_meter_frames += (uint64_t)frames;
    if (g_capture) {
        if (!g_capture_rate) {
            g_capture_rate = (uint32_t)spec->freq;
            g_capture_channels = (uint32_t)channels;
            capture_header(g_capture, g_capture_rate, g_capture_channels, 0);
        }
        fwrite(buffer, 1, (size_t)buflen, g_capture);
        g_capture_bytes += (uint64_t)buflen;
    }
    if (g_post_meter) {
        Uint64 now = SDL_GetTicks();
        if (!g_meter_last) g_meter_last = now;
        if (now - g_meter_last >= 5000u) {
            fprintf(stderr, "[audio-output] meter: peak %.1f dBFS, %llu of %llu frames past full "
                    "scale before the limiter, %llu silent\n",
                    g_meter_peak > 0.0f ? 20.0 * log10((double)g_meter_peak) : -144.0,
                    (unsigned long long)g_meter_over, (unsigned long long)g_meter_frames,
                    (unsigned long long)g_meter_silent);
            g_meter_peak = 0.0f;
            g_meter_over = g_meter_frames = g_meter_silent = 0;
            g_meter_last = now;
            {
                /* Per voice, from submit(): buffers are slots 0-255, streams
                 * and the movie after them (audio_output.h). Read without the
                 * lock: counters for a report, not state. */
                uint32_t i, dry_b = 0, dry_s = 0, bad = 0, joins = 0, worst = 0, worst_slot = 0;
                uint32_t steps = 0, big = 0, levels = 0;
                for (i = 0; i < VOICE_COUNT; i++) {
                    voice *v = &g_voices[i];
                    if (i < 256u) dry_b += v->dry; else dry_s += v->dry;
                    bad += v->bad_joins;
                    joins += v->joins;
                    if (v->dry + v->bad_joins > worst) {
                        worst = v->dry + v->bad_joins;
                        worst_slot = i;
                    }
                    levels += v->level_steps;
                    v->level_steps = 0;
                    steps += v->gain_steps;
                    big += v->big_gain_steps;
                    v->dry = v->joins = v->bad_joins = 0;
                    v->gain_steps = v->big_gain_steps = 0;
                }
                fprintf(stderr, "[audio-output] gain changes between submissions: %u, %u of them "
                        "over half a dB, each applied at once; 3D level changes %u; voices "
                        "remade with sound still queued %u (%u for a new rate, %u mix on/off)\n",
                        steps, big, levels, g_remade_queued, g_remade_rate, g_remade_mixed);
                g_remade_queued = g_remade_rate = g_remade_mixed = 0;
                fprintf(stderr, "[audio-output] voices reset with sound queued: %u, %.0f ms cut; "
                        "stopped and played out: %u, %.0f ms\n", g_cuts, g_cut_ms, g_drains,
                        g_drained_ms);
                g_cuts = g_drains = 0;
                g_cut_ms = g_drained_ms = 0.0;
                fprintf(stderr, "[audio-output] voices: ran dry %u (buffers) %u (streams); "
                        "%u of %u joins between submissions jump; worst slot %u (%u)\n",
                        dry_b, dry_s, bad, joins, worst_slot, worst);
            }
        }
    }
    if (g_post_muted)
        memset(buffer, 0, (size_t)buflen);
}

static int postmix_install(SDL_AudioDeviceID dev, int muted)
{
    const char *limit = getenv("RECOMP_AUDIO_LIMITER");
    const char *meter = getenv("RECOMP_AUDIO_METER");
    const char *capture = getenv("RECOMP_AUDIO_CAPTURE");
    SDL_AudioSpec spec;
    int frames = 0;

    SDL_zero(spec);
    SDL_GetAudioDeviceFormat(dev, &spec, &frames);
    g_post_muted = muted;
    g_post_limit = !(limit && strcmp(limit, "0") == 0);
    g_post_meter = meter && *meter && strcmp(meter, "0") != 0;
    /* Recover over about 6 ms: e^(-1 / (6 ms * rate)) per frame. */
    g_limit_release = 1.0f - expf(-1.0f / (0.006f * (float)(spec.freq > 0 ? spec.freq : 48000)));
    if (capture && *capture) {
        g_capture = fopen(capture, "wb");
        fprintf(stderr, "[audio-output] capturing the mix to %s%s\n", capture,
                g_capture ? "" : " -- could not open it");
    }
    if (!SDL_SetAudioPostmixCallback(dev, postmix, NULL)) {
        fprintf(stderr, "[audio-output] no post-mix stage (%s): no limiter%s\n",
                SDL_GetError(), muted ? "; muting by the device gain" : "");
        return 0;
    }
    if (g_post_limit)
        fprintf(stderr, "[audio-output] limiter on the mix (RECOMP_AUDIO_LIMITER=0 for none)\n");
    return 1;
}

static void *open_device_thread(void *arg)
{
    SDL_AudioDeviceID dev;
    float gain = g_open_gain;
    const char *mute = getenv("RECOMP_MUTE");
    int muted = mute && *mute && strcmp(mute, "0") != 0;

    (void)arg;
    /* SDL's audio brings up its event subsystem, which would otherwise turn
     * SIGINT/SIGTERM into a quit event nobody reads (src/host/host_sdl.c). */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "[audio-output] disabled operation=SDL_InitSubSystem error=%s\n",
                SDL_GetError());
        return NULL;
    }
    dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
    if (!dev) {
        fprintf(stderr, "[audio-output] disabled operation=SDL_OpenAudioDevice error=%s\n",
                SDL_GetError());
        return NULL;
    }
    /* RECOMP_MUTE: silent, but everything else as usual -- the device runs
     * and pulls the voices at its own rate, so their play positions are the
     * ones an audible run would see (see the XAudio2 file). The gain is set
     * before the device is published, so nothing is ever bound to it unmuted,
     * and a device that cannot be muted is not used at all. */
    if (muted)
        fprintf(stderr, "[audio-output] RECOMP_MUTE: playing silently\n");
    if (!postmix_install(dev, muted) && muted)
        gain = 0.0f;                         /* no post-mix stage: mute the device */
    if (!SDL_SetAudioDeviceGain(dev, gain)) {
        fprintf(stderr, "[audio-output] disabled operation=SetAudioDeviceGain error=%s\n",
                SDL_GetError());
        SDL_CloseAudioDevice(dev);
        return NULL;
    }
    {
        SDL_AudioSpec spec;
        int frames = 0;

        SDL_zero(spec);
        SDL_GetAudioDeviceFormat(dev, &spec, &frames);
        g_out_channels = spec.channels >= 1 && spec.channels <= (int)RECOMP_FOLD_MAX_CHANNELS
                       ? (uint32_t)spec.channels : 2u;
        recomp_audio_fold_matrix(g_out_channels, g_fold);
        fprintf(stderr, "[audio-output] initialized backend=sdl3_%s master_gain=%.6f "
                "device=%d Hz %d ch, %d-frame buffer\n", SDL_GetCurrentAudioDriver(),
                (double)gain, spec.freq, spec.channels, frames);
    }
    {
        /* RECOMP_AUDIO_OPEN_DELAY_MS: publish the device that much later, so
         * a test can watch streams cross from their own clock to the
         * device's, as they do when a slow CoreAudio opens mid-run. */
        const char *delay = getenv("RECOMP_AUDIO_OPEN_DELAY_MS");
        if (delay && *delay && atoi(delay) > 0)
            SDL_Delay((Uint32)atoi(delay));
    }
    /* The native path's send bus, beside it (muted in its post-mix). */
    {
        SDL_AudioDeviceID bus = bus_open(gain);
        if (!bus)
            fprintf(stderr, "[audio-output] no reverb send bus (%s): the native path "
                    "plays dry\n", SDL_GetError());
        __atomic_store_n(&g_bus, bus, __ATOMIC_RELEASE);
    }
    pthread_mutex_lock(&g_lock);
    if (g_summary_printed) {                 /* shut down while this was opening */
        pthread_mutex_unlock(&g_lock);
        SDL_CloseAudioDevice(dev);
        return NULL;
    }
    __atomic_store_n(&g_device, dev, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

void recomp_audio_output_initialize(void)
{
    double gain = 1.0;
    const char *setting;
    pthread_t thread;
    pthread_attr_t attr;

    pthread_mutex_lock(&g_lock);
    if (g_attempted) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_attempted = 1;
    setting = getenv("RECOMP_AUDIO_GAIN");
    if (setting) {
        char *end;

        gain = strtod(setting, &end);
        if (end == setting || *end != '\0' || !isfinite(gain) || gain < 0.0 || gain > 1.0) {
            fprintf(stderr, "[audio-output] muted invalid RECOMP_AUDIO_GAIN (expected 0..1)\n");
            pthread_mutex_unlock(&g_lock);
            return;
        }
    }
    if (gain == 0.0) {
        fprintf(stderr, "[audio-output] muted RECOMP_AUDIO_GAIN=0\n");
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_open_gain = (float)gain;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, open_device_thread, NULL) != 0)
        fprintf(stderr, "[audio-output] disabled operation=pthread_create\n");
    pthread_attr_destroy(&attr);
    pthread_mutex_unlock(&g_lock);
}

void recomp_audio_output_shutdown(void)
{
    uint32_t i;

    recomp_audio_health_report();

    pthread_mutex_lock(&g_lock);
    if (g_summary_printed) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    for (i = 0; i < VOICE_COUNT; i++)
        destroy_voice(&g_voices[i]);
    for (i = 0; i < DRAINING_MAX; i++)
        if (g_draining[i]) {
            SDL_DestroyAudioStream(g_draining[i]);
            g_draining[i] = NULL;
        }
    if (g_device)
        SDL_CloseAudioDevice(g_device);
    g_device = 0;
    if (g_bus)
        SDL_CloseAudioDevice(g_bus);
    g_bus = 0;
    if (g_capture) {
        capture_header(g_capture, g_capture_rate ? g_capture_rate : 48000u,
                       g_capture_channels ? g_capture_channels : 2u, g_capture_bytes);
        fclose(g_capture);
        g_capture = NULL;
    }
    g_attempted = 1;
    g_summary_printed = 1;
    fprintf(stderr, "[audio-output] summary submitted_buffers=%llu submitted_bytes=%llu "
            "nonzero_buffers=%llu dropped_buffers=%llu\n", g_submitted_buffers,
            g_submitted_bytes, g_nonzero_buffers, g_dropped_buffers);
    pthread_mutex_unlock(&g_lock);
}

/* The draining voices (g_draining) are freed by reap_draining once empty, from the callers' threads (never from SDL's audio
 * thread, which holds the device lock while it mixes). */
/* What a voice still holds, in milliseconds of its source. */
static double queued_ms(const voice *v)
{
    int q = v->stream ? SDL_GetAudioStreamQueued(v->stream) : 0;
    uint32_t frame = v->mixed ? MIX_FRAME_BYTES : v->channels * (v->bits_per_sample / 8u);
    if (q <= 0 || !frame || !v->sample_rate)
        return 0.0;
    return (double)q / frame * 1000.0 / v->sample_rate;
}

static void reap_draining(void)
{
    uint32_t i;
    for (i = 0; i < DRAINING_MAX; i++)
        if (g_draining[i] && SDL_GetAudioStreamQueued(g_draining[i]) <= 0) {
            SDL_DestroyAudioStream(g_draining[i]);
            g_draining[i] = NULL;
        }
}

void recomp_audio_output_release_voice(uint32_t slot)
{
    static int drain = -1;
    voice *v;
    uint32_t i;

    if (slot >= VOICE_COUNT)
        return;
    if (drain < 0) {
        const char *e = getenv("RECOMP_AUDIO_DRAIN");      /* 0: cut, as before */
        drain = !(e && strcmp(e, "0") == 0);
    }
    if (!drain) {
        recomp_audio_output_reset_voice(slot);
        return;
    }
    pthread_mutex_lock(&g_lock);
    reap_draining();
    v = &g_voices[slot];
    if (v->stream && SDL_GetAudioStreamQueued(v->stream) > 0) {
        for (i = 0; i < DRAINING_MAX && g_draining[i]; i++)
            ;
        if (i == DRAINING_MAX) {
            g_cut_ms += queued_ms(v);
            g_cuts++;
        } else {
            g_drained_ms += queued_ms(v);
            g_drains++;
            /* The resampler's last frames too, then let it go. The slot's
             * counters restart, as after a reset. */
            SDL_FlushAudioStream(v->stream);
            g_draining[i] = v->stream;
            v->stream = NULL;
            /* Its send plays out with it, into the room. */
            for (i = 0; v->send && i < DRAINING_MAX && g_draining[i]; i++)
                ;
            if (v->send && i < DRAINING_MAX) {
                SDL_FlushAudioStream(v->send);
                g_draining[i] = v->send;
                v->send = NULL;
            }
        }
    }
    destroy_voice(v);
    pthread_mutex_unlock(&g_lock);
}

void recomp_audio_output_reset_voice(uint32_t slot)
{
    if (slot >= VOICE_COUNT)
        return;
    pthread_mutex_lock(&g_lock);
    if (g_voices[slot].stream && SDL_GetAudioStreamQueued(g_voices[slot].stream) > 0) {
        g_cut_ms += queued_ms(&g_voices[slot]);
        g_cuts++;
    }
    destroy_voice(&g_voices[slot]);
    pthread_mutex_unlock(&g_lock);
}

void recomp_audio_output_set_mix(uint32_t slot, const RecompAudioMix *mix)
{
    slot_mix *m;

    if (slot >= VOICE_COUNT || !mix)
        return;
    pthread_mutex_lock(&g_lock);
    m = &g_mixes[slot];
    if (!m->set || memcmp(&m->mix, mix, sizeof m->mix) != 0) {
        m->mix = *mix;
        m->set = 1;
        if (++m->generation == 0)
            m->generation = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

void recomp_audio_output_route(uint32_t slot, uint32_t key)
{
    if (slot >= VOICE_COUNT)
        return;
    pthread_mutex_lock(&g_lock);
    if (g_mixes[slot].route != key) {
        g_mixes[slot].route = key;
        if (++g_mixes[slot].generation == 0)
            g_mixes[slot].generation = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

void recomp_audio_output_set_submix(uint32_t key, uint32_t channels, const RecompAudioMix *mix,
                                    int32_t volume_hundredth_db)
{
    submix *sub = NULL;
    uint32_t i;

    if (!key || !mix)
        return;
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < SUBMIXES && !sub; i++)
        if (g_submixes[i].key == key) sub = &g_submixes[i];
    for (i = 0; i < SUBMIXES && !sub; i++)
        if (!g_submixes[i].key) sub = &g_submixes[i];
    if (sub) {
        sub->key = key;
        sub->channels = channels == 1u ? 1u : 2u;
        sub->mix = *mix;
        sub->gain = volume_hundredth_db <= -10000 ? 0.0f
                  : powf(10.0f, (float)(volume_hundredth_db > 0 ? 0 : volume_hundredth_db) / 2000.0f);
        /* What is routed here mixes anew. */
        for (i = 0; i < VOICE_COUNT; i++)
            if (g_mixes[i].route == key && ++g_mixes[i].generation == 0)
                g_mixes[i].generation = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

/* The room (audio_reverb.c), twice: the APU path's stream runs its reverb
 * send (mix bin 10) through g_reverb, and the native path's voices' sends
 * reach g_bus_reverb through the send bus (bus_postmix). */
static RecompReverb *g_reverb;
static RecompI3dl2   g_room;
static int           g_room_set;

void recomp_audio_output_set_reverb(const RecompI3dl2 *room)
{
    static int said;
    pthread_mutex_lock(&g_lock);
    g_room_set = room != NULL;
    if (room)
        g_room = *room;
    if (g_reverb)
        recomp_reverb_set_room(g_reverb, room);
    pthread_mutex_unlock(&g_lock);
    /* The native path's room (the bus). */
    pthread_mutex_lock(&g_bus_lock);
    if (!g_bus_reverb && room && room->room > -10000) {
        SDL_AudioSpec spec;
        int frames = 0;
        SDL_zero(spec);
        if (g_bus)
            SDL_GetAudioDeviceFormat(g_bus, &spec, &frames);
        g_bus_reverb = recomp_reverb_create(spec.freq > 0 ? (uint32_t)spec.freq : 48000u);
    }
    if (g_bus_reverb)
        recomp_reverb_set_room(g_bus_reverb, room);
    __atomic_store_n(&g_bus_room, recomp_reverb_active(g_bus_reverb), __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_bus_lock);
    if (room && room->room > -10000 && !said) {
        said = 1;
        fprintf(stderr, "[audio-output] I3DL2 room %d, decay %.2f s: reverb on %s\n",
                room->room, (double)room->decay_time,
                g_bus ? "the native path's send bus and the APU path's stream"
                      : "the APU path's stream (no send bus: the native path plays dry)");
    }
}

void recomp_audio_output_clear_mix(uint32_t slot)
{
    if (slot >= VOICE_COUNT)
        return;
    pthread_mutex_lock(&g_lock);
    g_mixes[slot].set = 0;
    pthread_mutex_unlock(&g_lock);
}

int recomp_audio_output_position(uint32_t slot, uint64_t *played_bytes,
                                 uint32_t *queued_buffers)
{
    voice *v;

    if (slot >= VOICE_COUNT)
        return 0;
    pthread_mutex_lock(&g_lock);
    v = &g_voices[slot];
    if (!g_device || !v->stream) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    retire_played(v);
    if (played_bytes)
        *played_bytes = voice_played(v);
    if (queued_buffers)
        *queued_buffers = v->queued;
    pthread_mutex_unlock(&g_lock);
    return 1;
}

int recomp_audio_output_submit(uint32_t slot, const uint8_t *pcm, uint32_t bytes,
                               uint32_t sample_rate, uint32_t channels,
                               uint32_t bits_per_sample, int32_t volume_hundredth_db)
{
    uint32_t block_align, i;
    voice *v;
    float gain;
    int mixed_now = 0;
    RecompAudioMix eff;
    int nonzero = 0;
    const uint8_t silence = bits_per_sample == 8 ? 0x80 : 0;

    recomp_audio_output_initialize();
    /* Read outside the lock: g_device is published once, by the opening
     * thread, with release order; 0 until then (refused, offered again). */
    if (!__atomic_load_n(&g_device, __ATOMIC_ACQUIRE) || bytes == 0)
        return 0;
    pthread_mutex_lock(&g_lock);
    if (slot >= VOICE_COUNT || !pcm || bytes > MAX_BUFFER ||
        sample_rate < MIN_RATE || sample_rate > MAX_RATE ||
        (channels != 1 && channels != 2) ||
        (bits_per_sample != 8 && bits_per_sample != 16)) {
        drop_buffer(slot, "invalid-pcm");
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    block_align = channels * (bits_per_sample / 8);
    if (bytes % block_align != 0) {
        drop_buffer(slot, "partial-frame");
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    reap_draining();
    v = &g_voices[slot];
    {
        RecompAudioMix probe;
        if (effective_mix(slot, channels, &probe)) {
            eff = probe;
        } else {
            /* No mix: the channels to the front speakers at full level, as
             * SDL's own mono and stereo conversions put them. Every voice
             * is mixed here, so a mix being set or cleared never changes
             * the stream's format. */
            memset(&eff, 0, sizeof eff);
            eff.ratio = 1.0f;
            eff.levels[0][RECOMP_SPEAKER_FRONT_LEFT] = 1.0f;
            if (channels == 2u)
                eff.levels[1][RECOMP_SPEAKER_FRONT_RIGHT] = 1.0f;
            else
                eff.levels[0][RECOMP_SPEAKER_FRONT_RIGHT] = 1.0f;
        }
        mixed_now = 1;
    }
    if (v->stream && (v->sample_rate != sample_rate || v->channels != channels ||
                      v->bits_per_sample != bits_per_sample ||
                      v->mixed != mixed_now)) {
        if (g_post_meter && SDL_GetAudioStreamQueued(v->stream) > 0)
            g_remade_rate += v->sample_rate != sample_rate,
            g_remade_mixed += v->mixed != mixed_now,
            g_remade_queued++;
        destroy_voice(v);
    }
    if (!v->stream) {
        SDL_AudioSpec src;

        src.format = bits_per_sample == 8 ? SDL_AUDIO_U8 : SDL_AUDIO_S16LE;
        src.channels = (int)channels;
        src.freq = (int)sample_rate;
        if (mixed_now) {
            src.format = SDL_AUDIO_F32;
            src.channels = (int)g_out_channels;   /* folded: audio_speakers.h */
        }
        v->stream = SDL_CreateAudioStream(&src, NULL);
        if (!v->stream || !SDL_BindAudioStream(g_device, v->stream)) {
            fprintf(stderr, "[audio-output] voice %u: %s\n", slot, SDL_GetError());
            destroy_voice(v);
            drop_buffer(slot, "create-voice");
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
        v->sample_rate = sample_rate;
        v->channels = channels;
        v->bits_per_sample = bits_per_sample;
        v->mixed = mixed_now;
    }
    if (v->mixed && v->mix_applied != g_mixes[slot].generation) {
        float ratio = eff.ratio;
        if (!(ratio >= 1.0f / MAX_RATIO)) ratio = 1.0f / MAX_RATIO;
        if (ratio > MAX_RATIO) ratio = MAX_RATIO;
        SDL_SetAudioStreamFrequencyRatio(v->stream, ratio);
        if (v->send)
            SDL_SetAudioStreamFrequencyRatio(v->send, ratio);
        v->mix_applied = g_mixes[slot].generation;
    }

    if (volume_hundredth_db < -10000)
        volume_hundredth_db = -10000;
    if (volume_hundredth_db > 0)
        volume_hundredth_db = 0;
    gain = volume_hundredth_db == -10000 ? 0.0f : powf(10.0f, (float)volume_hundredth_db / 2000.0f);
    if (g_post_meter && v->submitted && gain != v->last_gain) {
        v->gain_steps++;
        if (fabsf(gain - v->last_gain) > 0.06f * (gain > v->last_gain ? gain : v->last_gain) + 1e-4f)
            v->big_gain_steps++;     /* more than about half a dB */
    }
    v->last_gain = gain;
    SDL_SetAudioStreamGain(v->stream, gain);

    /* A send stream once the voice sends to a room, started as far behind
     * as the voice has queued so the two line up. */
    if (!v->send && g_bus && __atomic_load_n(&g_bus_room, __ATOMIC_ACQUIRE) &&
        (eff.reverb[0] > 0.0f || eff.reverb[1] > 0.0f)) {
        SDL_AudioSpec ss;
        ss.format = SDL_AUDIO_F32;
        ss.channels = (int)g_out_channels;
        ss.freq = (int)sample_rate;
        v->send = SDL_CreateAudioStream(&ss, NULL);
        if (v->send && SDL_BindAudioStream(g_bus, v->send)) {
            int q = SDL_GetAudioStreamQueued(v->stream);
            static float zeros[4096];
            SDL_SetAudioStreamFrequencyRatio(v->send, SDL_GetAudioStreamFrequencyRatio(v->stream));
            while (q > 0) {
                int chunk = q < (int)sizeof zeros ? q : (int)sizeof zeros;
                SDL_PutAudioStreamData(v->send, zeros, chunk);
                q -= chunk;
            }
        } else if (v->send) {
            SDL_DestroyAudioStream(v->send);
            v->send = NULL;
        }
    }
    if (v->send)
        SDL_SetAudioStreamGain(v->send, gain);

    retire_played(v);
    if (g_post_meter && v->submitted && SDL_GetAudioStreamQueued(v->stream) == 0)
        v->dry++;
    if (g_post_meter && bits_per_sample == 16 && bytes >= 4u * channels) {
        const int16_t *p16 = (const int16_t *)pcm;
        if (v->have_tail) {
            /* The first sample against the line through the last two. A
             * join is bad when it misses by more than a quarter of full
             * scale and by more than four times the step before it. */
            int predicted = 2 * v->tail[1] - v->tail[0];
            int miss = abs((int)p16[0] - predicted);
            int step = abs((int)v->tail[1] - (int)v->tail[0]);
            v->joins++;
            if (miss > 8192 && miss > 4 * step + 256)
                v->bad_joins++;
        }
        v->tail[0] = p16[bytes / 2u - 2u * channels];
        v->tail[1] = p16[bytes / 2u - channels];
        v->have_tail = 1;
    }
    if (v->queued == QUEUE_SIZE) {
        drop_buffer(slot, "queue-full");
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (v->mixed && g_post_meter) {
        uint32_t c2, s2;
        int changed = 0;
        for (c2 = 0; c2 < 2u; c2++)
            for (s2 = 0; s2 < RECOMP_SPEAKERS; s2++) {
                float a = v->last_levels[c2][s2], b2 = eff.levels[c2][s2];
                if (fabsf(a - b2) > 0.06f * (a > b2 ? a : b2) + 1e-3f)
                    changed = 1;
                v->last_levels[c2][s2] = b2;
            }
        v->level_steps += (uint32_t)changed;
    }
    if (v->mixed) {
        uint32_t out_bytes = 0;
        float *send = NULL;
        const float *frames = mix_pcm(&g_mixes[slot], &eff, pcm, bytes, channels,
                                      bits_per_sample, &out_bytes, v->send ? &send : NULL);
        if (!frames || !SDL_PutAudioStreamData(v->stream, frames, (int)out_bytes)) {
            drop_buffer(slot, "submit");
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
        if (v->send && send)
            SDL_PutAudioStreamData(v->send, send, (int)out_bytes);
    } else if (!SDL_PutAudioStreamData(v->stream, pcm, (int)bytes)) {
        drop_buffer(slot, "submit");
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    v->submitted += bytes;
    v->chunk_end[(v->front + v->queued) % QUEUE_SIZE] = v->submitted;
    v->queued++;
    ++g_submitted_buffers;
    g_submitted_bytes += bytes;
    for (i = 0; i < bytes && !nonzero; ++i)
        nonzero = pcm[i] != silence;
    if (nonzero)
        ++g_nonzero_buffers;
    if (g_submitted_buffers == 1 || (nonzero && !g_reported_nonzero[slot]))
        fprintf(stderr, "[audio-output] submitted slot=%u bytes=%u rate=%u channels=%u "
                "bits=%u nonzero=%u gain=%.6f\n", slot, bytes, sample_rate, channels,
                bits_per_sample, nonzero ? 1u : 0u, (double)gain);
    if (nonzero)
        g_reported_nonzero[slot] = 1;
    pthread_mutex_unlock(&g_lock);
    recomp_audio_health_sound(slot, slot >= 256u, nonzero);
    return 1;
}

/* ── The APU path's stream (audio_output.h, recomp_audio_output_mix_*) ──
 *
 * One SDL stream in the device's layout, fed the speaker stage's frames
 * folded with g_fold, so the APU path and the native path put a sound at the
 * same level. The reverb send (the last channel) is not played yet: SDL has
 * no reverb of its own. */
static SDL_AudioStream *g_mix_stream;
static uint32_t         g_mix_rate;
static float           *g_mix_buf;
static uint32_t         g_mix_cap;

int recomp_audio_output_mix_submit(const float *frames, uint32_t count, uint32_t rate)
{
    uint32_t n, f, k, s, need;

    recomp_audio_output_initialize();
    if (!__atomic_load_n(&g_device, __ATOMIC_ACQUIRE) || !frames || !count ||
        rate < MIN_RATE || rate > MAX_RATE)
        return 0;
    pthread_mutex_lock(&g_lock);
    n = g_out_channels;
    if (!g_mix_stream || g_mix_rate != rate) {
        SDL_AudioSpec src;
        if (g_mix_stream)
            SDL_DestroyAudioStream(g_mix_stream);
        src.format = SDL_AUDIO_F32;
        src.channels = (int)n;
        src.freq = (int)rate;
        g_mix_stream = SDL_CreateAudioStream(&src, NULL);
        if (!g_mix_stream || !SDL_BindAudioStream(g_device, g_mix_stream)) {
            fprintf(stderr, "[audio-output] APU stream: %s\n", SDL_GetError());
            if (g_mix_stream)
                SDL_DestroyAudioStream(g_mix_stream);
            g_mix_stream = NULL;
            pthread_mutex_unlock(&g_lock);
            recomp_audio_health_refused();
            return 0;
        }
        g_mix_rate = rate;
        fprintf(stderr, "[audio-output] APU stream: %u Hz, folded onto %u channel(s)\n",
                rate, n);
    }
    need = count * n * (uint32_t)sizeof(float);
    if (need > g_mix_cap) {
        float *grown = (float *)realloc(g_mix_buf, need);
        if (!grown) {
            pthread_mutex_unlock(&g_lock);
            recomp_audio_health_refused();
            return 0;
        }
        g_mix_buf = grown;
        g_mix_cap = need;
    }
    for (f = 0; f < count; f++) {
        const float *in = frames + (size_t)f * RECOMP_MIX_CHANNELS;
        float *o = g_mix_buf + (size_t)f * n;
        for (k = 0; k < n; k++) {
            float v = 0.0f;
            for (s = 0; s < RECOMP_SPEAKERS; s++)
                v += in[s] * g_fold[s][k];
            o[k] = v;
        }
    }
    /* The reverb send, through the room, into the front pair (both halves
     * into a mono device). */
    if (!g_reverb && g_room_set) {
        g_reverb = recomp_reverb_create(rate);
        if (g_reverb)
            recomp_reverb_set_room(g_reverb, &g_room);
    }
    if (recomp_reverb_active(g_reverb)) {
        if (n >= 2) {
            recomp_reverb_process(g_reverb, frames + RECOMP_SPEAKERS, RECOMP_MIX_CHANNELS,
                                  g_mix_buf, g_mix_buf + 1, n, count);
        } else {
            static float wet[2 * 512];
            uint32_t done = 0;
            while (done < count) {
                uint32_t chunk = count - done < 512u ? count - done : 512u;
                memset(wet, 0, sizeof wet);
                recomp_reverb_process(g_reverb,
                                      frames + (size_t)done * RECOMP_MIX_CHANNELS + RECOMP_SPEAKERS,
                                      RECOMP_MIX_CHANNELS, wet, wet + 1, 2, chunk);
                for (f = 0; f < chunk; f++)
                    g_mix_buf[done + f] += 0.70710678f * (wet[2 * f] + wet[2 * f + 1]);
                done += chunk;
            }
        }
    }
    if (!SDL_PutAudioStreamData(g_mix_stream, g_mix_buf, (int)need)) {
        pthread_mutex_unlock(&g_lock);
        recomp_audio_health_refused();
        return 0;
    }
    pthread_mutex_unlock(&g_lock);
    return 1;
}

int recomp_audio_output_mix_queued_ms(void)
{
    int q, ms;

    if (!__atomic_load_n(&g_device, __ATOMIC_ACQUIRE))
        return -1;
    pthread_mutex_lock(&g_lock);
    if (!g_mix_stream || !g_mix_rate) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    q = SDL_GetAudioStreamQueued(g_mix_stream);
    ms = q > 0 ? (int)((int64_t)q / (int64_t)(g_out_channels * sizeof(float)) * 1000 /
                       (int64_t)g_mix_rate)
               : 0;
    pthread_mutex_unlock(&g_lock);
    return ms;
}
