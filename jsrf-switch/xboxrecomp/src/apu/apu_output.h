/*
 * Host audio output for the APU model: an SDL2 audio queue.
 *
 * The APU frame thread renders one block of 256 stereo frames (48 kHz, s16)
 * every 8 EP frames and pushes it here. It watches audio_out_queued() to
 * decide when to render the next one, so the device's clock, draining the
 * queue, is what keeps the APU in time.
 */
#ifndef APU_OUTPUT_H
#define APU_OUTPUT_H

#include <stdint.h>

/* Open the device. 0 if output is turned off (RECOMP_AUDIO=0) or failed. */
int  audio_out_open(void);
void audio_out_close(void);
int  audio_out_active(void);

/* Queue `frames` interleaved stereo frames, after the volume and limiter.
 * 0 if the block was not queued. */
int  audio_out_push(const int16_t *pcm, int frames);

/* Blocks waiting to be played, and how many the frame thread keeps there. */
int  audio_out_queued(void);
int  audio_out_target(void);

/* Volume 0..1 (0.5 to start), followed by a soft limiter that keeps every
 * sample under -6 dBFS: whatever the mixers produce, a headset never gets
 * full scale. audio_out_limit applies both, for another output path; in and
 * out may be the same buffer. */
void audio_out_set_volume(float volume);
void audio_out_limit(const int16_t *in, int16_t *out, int samples);

/* Counters since the start. ran_dry: a push that found the queue empty (a
 * gap was heard). dropped: a block thrown away because the queue was full.
 * loudest: the largest |sample| pushed since the previous call. */
typedef struct {
    uint64_t pushed, dropped, ran_dry;
    int      loudest;
} AudioOutStats;
void audio_out_stats(AudioOutStats *stats);

#endif
