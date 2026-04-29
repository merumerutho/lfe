/*
 * gen_test_tone.c — Phase 0 test tone generator.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Produces a sine wave at the requested frequency for the requested
 * buffer length. The simplest possible "real" generator — used to
 * validate the entire pipeline (library API → wavetable lookup →
 * sample buffer write) before anything more complex lands.
 *
 * Algorithm: phase accumulator + sine wavetable lookup with linear
 * interpolation. Per-sample cost: ~10 cycles on ARM9.
 */

#include "lfe.h"

#include "util/fixed.h"
#include "util/platform.h"
#include "util/wavetable.h"

/* Forward declaration of the internal init check from lfe.c. */
extern bool lfe_is_initialized(void);

LFE_HOT
lfe_status lfe_gen_test_tone(lfe_buffer *out,
                             const lfe_test_tone_params *p)
{
    if (!out || !out->data || !p)
        return LFE_ERR_NULL;
    if (!lfe_is_initialized())
        return LFE_ERR_NOT_INIT;

    const uint32_t sample_rate = (uint32_t)out->rate;
    if (sample_rate == 0)
        return LFE_ERR_BAD_PARAM;

    /* Reject above-Nyquist frequencies. The Q24.8 frequency is in Hz,
     * so the Nyquist limit is sample_rate / 2 in the same units. */
    const q24_8_t nyquist_q8 = (q24_8_t)((sample_rate / 2u) << 8);
    if (p->freq_hz_q8 >= nyquist_q8)
        return LFE_ERR_BAD_PARAM;

    /* Phase accumulator advances by `inc` per sample. Wraps naturally
     * because it's unsigned 32-bit. */
    const lfe_phase_t inc = lfe_freq_to_phase_inc(p->freq_hz_q8, sample_rate);
    lfe_phase_t       phase = 0;

    /* Apply Q15 amplitude scaling so the caller can request a quieter
     * tone if needed (e.g. for headroom in tests). Cap at Q15_ONE
     * defensively in case a caller passes 0xFFFF. */
    int32_t amp = p->amplitude_q15;
    if (amp > Q15_ONE) amp = Q15_ONE;

    lfe_sample_t *dst = out->data;
    const uint32_t n  = out->length;

    for (uint32_t i = 0; i < n; i++) {
        q15_t s = lfe_wt_sine_lookup(phase);
        /* Scale by amplitude (Q15 * Q15 → Q15). */
        dst[i] = (lfe_sample_t)(((int32_t)s * amp) >> 15);
        phase += inc;
    }

    return LFE_OK;
}
