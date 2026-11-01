/*
 * MCPX APU DSP (GP/EP) - Stub implementation
 *
 * The DSP Global Processor (GP) and Encode Processor (EP) handle effects
 * processing (reverb, chorus, etc.) and final output encoding. The full
 * DSP is ~3000 lines of DSP56300 emulation code.
 *
 * For initial audio, we bypass the DSP entirely:
 * - VP mixbins are passed directly to the EP output
 * - GP effects processing is skipped
 * - The EP just copies mixbin 0/1 (front L/R) to the monitor buffer
 *
 * This gives us basic voice playback without effects. The DSP can be
 * connected later for reverb, EQ, and other processing.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu_state.h"
#include "fpconv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── DSP command doorbell acknowledgement ────────────────────────────────
 *
 * DirectSound does not stop at creating the device. It hands the audio DSP a
 * command block in guest RAM, writes a command word, and spins until the DSP
 * writes zero back. On real hardware the GP runs a DSP56300 program that does
 * that. Here the DSP is a passthrough stub, so the word never changes and the
 * title hangs inside DirectSound initialisation -- which on Wreckless gates the
 * entire engine, not just audio.
 *
 * RECOMP_APU_DSP_ACK=<addr>[,<addr>...] clears those guest dwords once per APU
 * frame, which is what "the command completed" looks like to the title.
 *
 * ponytail: this is a handshake acknowledgement, not a DSP. It says every
 * command succeeded instantly and computes nothing, so anything whose *result*
 * the title reads back will still be wrong. The real fix is DSP56300 emulation
 * in the GP/EP; this exists so audio init stops blocking everything behind it.
 *
 * The address is not derivable from the APU registers: GPSADDR/GPFADDR/
 * EPSADDR/EPFADDR point at the DSP's own scratch and frame memory, while the
 * command block is a DirectSound heap allocation. On Wreckless the registers
 * read 0x01504000 / 0x014EC000 / 0x0151C000 / 0x014F0000 and the doorbell is at
 * 0x014F8810 -- inside none of them. So it has to be observed: run with
 * RECOMP_WATCHDOG_SECS and the spin shows up as ebx plus the poll offset.
 */
#define APU_DSP_ACK_MAX 8
static uint32_t s_dsp_ack[APU_DSP_ACK_MAX];
static int s_dsp_ack_count = -1;

static void dsp_ack_init(void)
{
    const char *spec = getenv("RECOMP_APU_DSP_ACK");
    char buf[256], *p, *end;

    s_dsp_ack_count = 0;
    if (!spec || !*spec)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (p = buf; *p && s_dsp_ack_count < APU_DSP_ACK_MAX; ) {
        unsigned long v = strtoul(p, &end, 0);
        if (end == p)
            break;
        if (v)
            s_dsp_ack[s_dsp_ack_count++] = (uint32_t)v;
        p = (*end == ',') ? end + 1 : end;
    }
    if (s_dsp_ack_count)
        fprintf(stderr, "[APU] DSP doorbell ack: %d address(es), first 0x%08X\n",
                s_dsp_ack_count, s_dsp_ack[0]);
}

/* SUM EVERY MIXBIN THE GUEST ROUTED TO, NOT JUST THE FIRST TWO.
 *
 * Default ON. RECOMP_APU_MIXDOWN_ALL=0 restores the previous two-bin read,
 * because this changes audible output for every title and an escape hatch
 * costs one branch. */
static int mcpx_apu_mixdown_all(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_MIXDOWN_ALL");
        on = (e && *e) ? (atoi(e) != 0) : 1;
    }
    return on;
}

/* THE MIXDOWN BY DIRECTSOUND'S SPEAKER LAYOUT.
 *
 * Summing every bin, even ones left and odd ones right, treated each bin as
 * a speaker. They are not: DirectSound's mixbins (DSMIXBIN_*) are
 *
 *     0 front left    1 front right    2 centre    3 LFE
 *     4 back left     5 back right
 *     6 3D front left 7 3D front right 8 3D back left 9 3D back right
 *    10 I3DL2 reverb send               11-30 effect sends
 *
 * and on hardware the GP's effects image turns the sends into reverb and
 * effects, mixed in far below the voices they copy. JSRF's sound effects are
 * 3D voices in bins 6-9 and, at the same level, in bin 10; its music is 2D,
 * in bins 0 and 1. The even/odd sum added the reverb send dry on top of the
 * left side, put the centre on the left and the LFE on the right, and the
 * effects drowned the music.
 *
 * Now: front and 3D front pairs to their sides; centre to both at -3 dB;
 * back pairs to their sides at -3 dB (a stereo downmix's surrounds); LFE to
 * both at -6 dB; every send to both at -12 dB (RECOMP_APU_SEND_DB), a stand-
 * in for the reverb the stub cannot make, never dry on top. The GP stays a
 * stub: no reverb, no effects.
 *
 * RECOMP_APU_MIXDOWN=pairs  the even/odd sum, as before
 * RECOMP_APU_MIXDOWN_ALL=0  bins 0 and 1 only, as before that
 * RECOMP_APU_2D_DB=<dB>     bins 0-5 louder (+) or quieter (-): JSRF's music
 * RECOMP_APU_3D_DB=<dB>     bins 6-9: JSRF's sound effects
 * RECOMP_APU_SEND_DB=<dB>   bins 10-30 (default -12)
 */
enum { MIX_SPEAKERS, MIX_PAIRS, MIX_FRONT };
static int   s_mix_mode = -1;
static float s_gain_l[NUM_MIXBINS], s_gain_r[NUM_MIXBINS];

static float env_db(const char *name, float dflt)
{
    const char *e = getenv(name);
    float db = (e && *e) ? (float)atof(e) : dflt;
    return powf(10.0f, db / 20.0f);
}

static void mix_setup(void)
{
    const char *m = getenv("RECOMP_APU_MIXDOWN");
    float g2d = env_db("RECOMP_APU_2D_DB", 0.0f), g3d = env_db("RECOMP_APU_3D_DB", 0.0f);
    float gs = env_db("RECOMP_APU_SEND_DB", -12.0f);
    const float h = 0.70710678f;
    int b;

    if (!mcpx_apu_mixdown_all())
        s_mix_mode = MIX_FRONT;
    else if (m && !strcmp(m, "pairs"))
        s_mix_mode = MIX_PAIRS;
    else
        s_mix_mode = MIX_SPEAKERS;
    memset(s_gain_l, 0, sizeof s_gain_l);
    memset(s_gain_r, 0, sizeof s_gain_r);
    if (s_mix_mode == MIX_FRONT) {
        s_gain_l[0] = s_gain_r[1] = 1.0f;
    } else if (s_mix_mode == MIX_PAIRS) {
        for (b = 0; b < NUM_MIXBINS; b++) {
            if (b & 1) s_gain_r[b] = 1.0f;
            else       s_gain_l[b] = 1.0f;
        }
    } else {
        s_gain_l[0] = g2d;            s_gain_r[1] = g2d;
        s_gain_l[2] = s_gain_r[2] = h * g2d;
        s_gain_l[3] = s_gain_r[3] = 0.5f * g2d;
        s_gain_l[4] = h * g2d;        s_gain_r[5] = h * g2d;
        s_gain_l[6] = g3d;            s_gain_r[7] = g3d;
        s_gain_l[8] = h * g3d;        s_gain_r[9] = h * g3d;
        for (b = 10; b < NUM_MIXBINS; b++)
            s_gain_l[b] = s_gain_r[b] = gs;
    }
    fprintf(stderr, "[APU] mixdown: %s (2D x%.2f, 3D x%.2f, sends x%.2f)\n",
            s_mix_mode == MIX_FRONT ? "front bins only" : s_mix_mode == MIX_PAIRS
            ? "even bins left, odd right" : "by speaker layout", g2d, g3d, gs);
}

/* Levels over 10 s of audio, per group of bins and at the output, with the
 * headroom the title set: what the mix is made of, to set its balance. */
static struct {
    double   sq[4];                 /* 2D (0-5), 3D (6-9), sends, output */
    float    peak[4];
    uint32_t n, over;               /* samples, output samples past full scale */
} s_lv;

static float lv_db(double v) { return v > 1e-10 ? (float)(10.0 * log10(v)) : -99.0f; }

static void level_report(const MCPXAPUState *d)
{
    static const char *name[4] = { "2D", "3D", "sends", "output" };
    char line[512];
    int at = 0, g, b;
    double n = s_lv.n ? (double)s_lv.n : 1.0;
    at += snprintf(line + at, sizeof line - at, "[APU] levels over 10 s, RMS/peak dBFS:");
    for (g = 0; g < 4; g++)
        at += snprintf(line + at, sizeof line - at, " %s %.0f/%.0f%s", name[g],
                       lv_db(s_lv.sq[g] / n), lv_db((double)s_lv.peak[g] * s_lv.peak[g]),
                       g < 3 ? "," : "");
    at += snprintf(line + at, sizeof line - at, "; %.1f%% of output past full scale;"
                   " headroom bins 0-10:", 100.0 * s_lv.over / (n * 2.0));
    for (b = 0; b <= 10 && at < (int)sizeof line - 8; b++)
        at += snprintf(line + at, sizeof line - at, " %u", d->vp.submix_headroom[b]);
    snprintf(line + at, sizeof line - at, ", 3D %u (bins %u %u %u %u)", d->vp.hrtf_headroom,
             d->vp.hrtf_submix[0], d->vp.hrtf_submix[1], d->vp.hrtf_submix[2],
             d->vp.hrtf_submix[3]);
    fprintf(stderr, "%s\n", line);
    memset(&s_lv, 0, sizeof s_lv);
}

static void dsp_ack_frame(MCPXAPUState *d)
{
    int i;

    if (s_dsp_ack_count < 0)
        dsp_ack_init();
    if (!d->ram_ptr)
        return;
    for (i = 0; i < s_dsp_ack_count; i++) {
        uint32_t *slot = (uint32_t *)(d->ram_ptr + s_dsp_ack[i]);
        if (*slot) {
            static int shown[APU_DSP_ACK_MAX];
            if (shown[i]++ < 3)
                fprintf(stderr, "[APU] DSP doorbell 0x%08X: command 0x%08X"
                                " acknowledged\n", s_dsp_ack[i], *slot);
            *slot = 0;
        }
    }
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    /* Allocate minimal DSP state for GP and EP.
     * We need these to exist so reset doesn't crash,
     * but they won't actually run DSP programs. */
    d->gp.dsp = (DSPState *)calloc(1, sizeof(DSPState));
    d->ep.dsp = (DSPState *)calloc(1, sizeof(DSPState));

    if (d->gp.dsp) d->gp.dsp->is_gp = true;
    if (d->ep.dsp) d->ep.dsp->is_gp = false;

    d->gp.realtime = false;
    d->ep.realtime = false;

    fprintf(stderr, "[APU] DSP GP/EP initialized (STUBBED - passthrough mode)\n");
}

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    /* In the real xemu, this reads settings to decide whether
     * GP/EP should run in realtime or cached mode. We ignore it. */
    (void)d;
}

void mcpx_apu_dsp_frame(MCPXAPUState *d,
                         float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    /* Bypass DSP: mix the VP's mixbins down to stereo (mix_setup: by
     * DirectSound's speaker layout) and write them directly to the monitor
     * frame buffer as the final EP output. */

    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;

    dsp_ack_frame(d);
    if (s_mix_mode < 0)
        mix_setup();
    if (s_lv.n >= 48000u * 10u)
        level_report(d);

    if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            /* Bins 2..31 used to be computed and then dropped on the floor.
             * On hardware the GP and EP mix the submixes down; here they are
             * stubs, so thirty of thirty-two bins were discarded every frame
             * with no counter anywhere to say so.
             *
             * Measured on Jet Set Radio Future, one 200 s gameplay run, with
             * a positive control moving beside it:
             *
             *     [APU-BIN] 2D heard=557466 lost=0
             *               3D heard=0      lost=377768
             *               lost by bin: 6,7,8,9,10
             *
             * 557,466 music voice-frames heard and none lost; 377,768 effect
             * voice-frames produced correctly and thrown away. The title's 3D
             * positional voices -- its sound effects -- are routed to bins 6
             * to 10 by the guest's own V0BIN..V3BIN, and music on 2D voices
             * lands in bins 0 and 1, which is why the music was always
             * audible and no effect ever was. Every instrument upstream of
             * this line read healthy.
             *
             * Gating the HRTF submix override was tried first and did not fix
             * it, so the defect is the width of this mixdown and nothing else.
             *
             * Every bin is now mixed in, by DirectSound's speaker layout
             * (mix_setup): the even/odd sum that first replaced the two-bin
             * read put the reverb send dry on the left, on top of the
             * effects it copies. Not what a real EP does either: no reverb,
             * no effects, a fixed stereo downmix. */
            float left = 0.0f, right = 0.0f;
            for (int b = 0; b < NUM_MIXBINS; ++b) {
                float x = mixbins[b][i];
                left += s_gain_l[b] * x;
                right += s_gain_r[b] * x;
                if (x != 0.0f) {
                    int g = b < 6 ? 0 : b < 10 ? 1 : 2;
                    float a = fabsf(x);
                    s_lv.sq[g] += (double)x * x;
                    if (a > s_lv.peak[g]) s_lv.peak[g] = a;
                }
            }
            s_lv.sq[3] += 0.5 * ((double)left * left + (double)right * right);
            if (fabsf(left) > s_lv.peak[3]) s_lv.peak[3] = fabsf(left);
            if (fabsf(right) > s_lv.peak[3]) s_lv.peak[3] = fabsf(right);
            s_lv.over += (fabsf(left) > 1.0f) + (fabsf(right) > 1.0f);
            s_lv.n++;
            /* Summing every bin goes past full scale (1.33 in the intro
             * movies), and a hard clamp there is audible distortion. tanh is
             * near-linear below ~0.5 and bends smoothly into [-1, 1]. */
            left = tanhf(left);
            right = tanhf(right);

            /* Convert to 16-bit and write (not accumulate) into frame buffer.
             * Each of the 8 sub-frames writes its own 32-sample slice. */
            d->monitor.frame_buf[off + i][0] = (int16_t)(left * 32767.0f);
            d->monitor.frame_buf[off + i][1] = (int16_t)(right * 32767.0f);
        }
    }

    g_dbg.gp.cycles = 0;
    g_dbg.ep.cycles = 0;
}
