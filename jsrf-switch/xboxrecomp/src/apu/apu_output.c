/*
 * Host audio output for the APU model (see apu_output.h).
 *
 * SDL_QueueAudio rather than a callback: the frame thread already produces
 * blocks at the device's rate, and SDL copies each one into its own queue,
 * so no second ring or lock is needed and guest memory is never handed to
 * the sound service.
 *
 * Settings (environment):
 *   RECOMP_AUDIO=0          no host device; the APU paces by the wall clock.
 *   RECOMP_AUDIO_BLOCKS=n   blocks kept queued, 2..64 (12 = 64 ms on the
 *                           Switch, whose busy cores time-slice in 10 ms
 *                           quanta; 8 elsewhere).
 *   RECOMP_AUDIO_VOLUME=n   0..100.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>

#include "apu_output.h"

enum {
    RATE          = 48000,
    CHANNELS      = 2,
    BLOCK_FRAMES  = 256,
    BLOCK_BYTES   = BLOCK_FRAMES * CHANNELS * (int)sizeof(int16_t),
    MAX_PUSH      = 1024,           /* frames in one push */
#ifdef __SWITCH__
    DEFAULT_QUEUE = 12,
#else
    DEFAULT_QUEUE = 8,
#endif
};

/* Output ceiling: -6 dBFS. */
#define LIMIT_CEILING 16384.0f

static struct {
    SDL_AudioDeviceID device;
    int               queue_target;
    float             volume;
    AudioOutStats     stats;
    int               curve_ready;
} g_out = { 0, DEFAULT_QUEUE, 0.5f };

/* Volume then limiter, for every possible input sample: indexed by the
 * sample + 32768. Rebuilt when the volume changes. (Zeroed storage, apart
 * from g_out, so it does not take 128 KB of the NRO.) */
static int16_t g_curve[65536];

static void build_curve(void)
{
    int s;

    /* ceiling * tanh(x / ceiling): straight for quiet signals, bending
     * smoothly towards the ceiling for loud ones, so a spike is capped
     * without the hard edge of clipping. */
    for (s = -32768; s <= 32767; s++)
        g_curve[s + 32768] =
            (int16_t)(LIMIT_CEILING * tanhf((float)s * g_out.volume / LIMIT_CEILING));
    g_out.curve_ready = 1;
}

void audio_out_set_volume(float volume)
{
    if (volume < 0.0f)
        volume = 0.0f;
    if (volume > 1.0f)
        volume = 1.0f;
    g_out.volume = volume;
    build_curve();
}

void audio_out_limit(const int16_t *in, int16_t *out, int samples)
{
    int i;

    if (!g_out.curve_ready)
        build_curve();
    for (i = 0; i < samples; i++)
        out[i] = g_curve[in[i] + 32768];
}

static int env_number(const char *name, int *value)
{
    const char *text = getenv(name);

    if (!text || !*text)
        return 0;
    *value = atoi(text);
    return 1;
}

int audio_out_open(void)
{
    SDL_AudioSpec asked, got;
    const char *driver;
    int n;

    if (g_out.device)
        return 1;
    if (env_number("RECOMP_AUDIO", &n) && n == 0) {
        fprintf(stderr, "[AUDIO] RECOMP_AUDIO=0: no host output, the APU keeps time by the wall clock\n");
        return 0;
    }
    if (env_number("RECOMP_AUDIO_BLOCKS", &n) && n >= 2 && n <= 64)
        g_out.queue_target = n;
    if (env_number("RECOMP_AUDIO_VOLUME", &n))
        audio_out_set_volume((float)n / 100.0f);
    if (!g_out.curve_ready)
        build_curve();

    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[AUDIO] SDL audio did not start: %s\n", SDL_GetError());
        return 0;
    }

    memset(&asked, 0, sizeof asked);
    asked.freq     = RATE;
    asked.format   = AUDIO_S16SYS;
    asked.channels = CHANNELS;
    asked.samples  = 1024;          /* the device's own buffer */
    asked.callback = NULL;          /* queued, not pulled */

    /* No changes allowed: SDL converts if the device wants something else. */
    g_out.device = SDL_OpenAudioDevice(NULL, 0, &asked, &got, 0);
    if (!g_out.device) {
        fprintf(stderr, "[AUDIO] no audio device: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return 0;
    }
    SDL_PauseAudioDevice(g_out.device, 0);

    driver = SDL_GetCurrentAudioDriver();
    fprintf(stderr, "[AUDIO] SDL %s output: %d Hz, %d ch, %d-frame device buffer,"
                    " %d blocks of %d frames queued, volume %d%%\n",
            driver ? driver : "?", got.freq, got.channels, got.samples,
            g_out.queue_target, BLOCK_FRAMES, (int)(g_out.volume * 100.0f + 0.5f));
    return 1;
}

void audio_out_close(void)
{
    if (!g_out.device)
        return;
    SDL_CloseAudioDevice(g_out.device);
    g_out.device = 0;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    fprintf(stderr, "[AUDIO] closed after %llu blocks (%llu ran dry, %llu dropped)\n",
            (unsigned long long)g_out.stats.pushed, (unsigned long long)g_out.stats.ran_dry,
            (unsigned long long)g_out.stats.dropped);
}

int audio_out_active(void)
{
    return g_out.device != 0;
}

int audio_out_queued(void)
{
    return g_out.device ? (int)(SDL_GetQueuedAudioSize(g_out.device) / BLOCK_BYTES) : 0;
}

int audio_out_target(void)
{
    return g_out.queue_target;
}

int audio_out_push(const int16_t *pcm, int frames)
{
    int16_t block[MAX_PUSH * CHANNELS];
    int samples, queued, i;

    if (!g_out.device)
        return 0;
    if (frames > MAX_PUSH)
        frames = MAX_PUSH;

    /* A device that stopped draining (the applet suspended) must not grow
     * the queue without bound. The frame thread stops waiting on it too. */
    queued = audio_out_queued();
    if (queued >= 4 * g_out.queue_target) {
        g_out.stats.dropped++;
        return 0;
    }
    if (queued == 0 && g_out.stats.pushed)
        g_out.stats.ran_dry++;

    samples = frames * CHANNELS;
    for (i = 0; i < samples; i++) {
        int magnitude = abs(pcm[i]);
        if (magnitude > g_out.stats.loudest)
            g_out.stats.loudest = magnitude;
    }
    audio_out_limit(pcm, block, samples);

    if (SDL_QueueAudio(g_out.device, block, (Uint32)(samples * sizeof(int16_t))) != 0)
        return 0;
    g_out.stats.pushed++;
    return 1;
}

void audio_out_stats(AudioOutStats *stats)
{
    /* Read without a lock: the counters only grow, and a torn "loudest" is
     * one report's worth of wrong. */
    *stats = g_out.stats;
    g_out.stats.loudest = 0;
}
