/*
 * gen_braids.c — Public Braids generator.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Wraps the internal MACRO oscillator and decimates from Braids'
 * native 96 kHz down to whatever `out->rate` requested. Decimation
 * is integer box-average; non-integer ratios round down and are
 * accepted (the resulting alias artifacts are negligible at any of
 * the lfe_rate values we support — 8/16/32 kHz all divide 96000
 * cleanly: ÷12, ÷6, ÷3 respectively).
 */

#include "lfe.h"
#include "util/fixed.h"
#include "util/platform.h"

#include "braids/lfe_braids_macro.h"
#include "braids/lfe_braids_random.h"

#include <math.h>
#include <string.h>

#define BRAIDS_INTERNAL_RATE   96000u
#define BRAIDS_RENDER_BLOCK    BRAIDS_MACRO_BLOCK   /* 64 internal samples */

extern bool lfe_is_initialized(void);

/* Convert Q24.8 Hz → Braids "midi_pitch" units (semitones × 128 above
 * an internal reference). MIDI 69 (A4 = 440 Hz) → 69 × 128 = 8832.
 *
 * Uses log2f. This runs once per lfe_gen_braids() call (not per
 * sample), so the float cost is acceptable even on ARM946E-S.
 */
static int16_t hz_q8_to_midi_pitch(uint32_t hz_q8)
{
    if (hz_q8 == 0) return 0;
    float hz   = (float)hz_q8 * (1.0f / 256.0f);
    float midi = 69.0f + 12.0f * log2f(hz / 440.0f);
    int   p    = (int)(midi * 128.0f + 0.5f);
    if (p < 0)         return 0;
    if (p > 16383)     return 16383;
    return (int16_t)p;
}

lfe_status lfe_gen_braids(lfe_buffer *out, const lfe_braids_params *p)
{
    if (!lfe_is_initialized())                  return LFE_ERR_NOT_INIT;
    if (!out || !out->data || !p)               return LFE_ERR_NULL;
    if (out->rate == 0 || out->rate > BRAIDS_INTERNAL_RATE)
                                                return LFE_ERR_BAD_PARAM;
    if (p->shape >= LFE_BRAIDS_SHAPE_COUNT)     return LFE_ERR_BAD_PARAM;
    if (out->length == 0)                       return LFE_OK;

    /* Decimation factor: integer floor of (internal / target). For any
     * rate that doesn't divide 96000 cleanly we round down and accept
     * the slight pitch error — the preview UI will be at one of the
     * supported lfe_rate values where the division is exact. */
    uint32_t decim = BRAIDS_INTERNAL_RATE / out->rate;
    if (decim == 0) decim = 1;

    /* Optional deterministic seed for stochastic shapes. */
    if (p->seed != 0)
        braids_random_seed(p->seed);

    /* Pitch is computed once per render call. */
    int16_t midi_pitch = hz_q8_to_midi_pitch(p->pitch_hz_q8);

    /* Spin up the macro oscillator on the stack. ~16 KB, fits the
     * default thread stack on host (≥1 MB); on NDS the editor task
     * has ample stack since it's the user-input thread. */
    braids_macro_osc_t osc;
    braids_macro_osc_init(&osc);
    braids_macro_osc_set_shape(&osc, (braids_macro_shape)p->shape);
    braids_macro_osc_set_pitch(&osc, midi_pitch);
    braids_macro_osc_set_parameters(&osc, (int16_t)p->timbre, (int16_t)p->color);
    braids_macro_osc_strike(&osc);

    /* Internal scratch — up to BRAIDS_RENDER_BLOCK samples per pass.
     * Allocated on the stack; 128 B for s16. */
    int16_t internal[BRAIDS_RENDER_BLOCK];
    uint32_t out_written = 0;
    bool clipped = false;

    while (out_written < out->length) {
        /* Choose how many output samples to produce this pass. The
         * macro renderer's internal block is BRAIDS_RENDER_BLOCK; we
         * need `n_internal = n_out * decim` samples to feed it, and
         * BRAIDS_RENDER_BLOCK is the cap on `n_internal`. */
        uint32_t n_out      = (BRAIDS_RENDER_BLOCK / decim);
        if (n_out == 0) n_out = 1;
        if (n_out > out->length - out_written)
            n_out = out->length - out_written;
        uint32_t n_internal = n_out * decim;
        /* MACRO requires even sizes (some shapes are 2× downsampled). */
        if (n_internal & 1) {
            n_internal++;
            /* keep n_out consistent so the trailing internal samples
             * still go into a defined slot — they'll fold into the
             * last output sample's average. */
        }
        if (n_internal > BRAIDS_RENDER_BLOCK)
            n_internal = BRAIDS_RENDER_BLOCK & ~1u;

        braids_macro_osc_render(&osc, internal, n_internal);

        /* Box-average decimation. Each output sample is the mean of
         * `decim` consecutive internal samples. */
        uint32_t i = 0;
        for (uint32_t k = 0; k < n_out; k++) {
            int32_t acc = 0;
            uint32_t taken = 0;
            for (uint32_t d = 0; d < decim && i < n_internal; d++, i++) {
                acc += internal[i];
                taken++;
            }
            int32_t avg = taken ? acc / (int32_t)taken : 0;
            if (avg >  32767)     { avg =  32767; clipped = true; }
            if (avg < -32768)     { avg = -32768; clipped = true; }
            out->data[out_written + k] = (lfe_sample_t)avg;
        }
        out_written += n_out;
    }

    return clipped ? LFE_WARN_CLIPPED : LFE_OK;
}
