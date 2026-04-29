/*
 * lfo.c — Low-frequency oscillator (internal).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shape renderers on a 32-bit phase accumulator. Sine reuses the
 * shared sine table; the other shapes are computed directly from
 * phase bits (no table needed — a triangle/square/saw is a few ALU
 * ops).
 */

#include "lfo.h"

#include "wavetable.h"

void lfe_lfo_init(lfe_lfo_state *lfo,
                  lfe_lfo_shape shape,
                  q24_8_t rate_hz_q8,
                  uint32_t sample_rate)
{
    if (!lfo || sample_rate == 0) return;
    lfo->phase     = 0;
    lfo->phase_inc = lfe_freq_to_phase_inc(rate_hz_q8, sample_rate);
    lfo->shape     = shape;
}

q15_t lfe_lfo_step(lfe_lfo_state *lfo)
{
    if (!lfo) return 0;

    const lfe_phase_t ph = lfo->phase;
    lfo->phase += lfo->phase_inc;

    switch (lfo->shape) {
    case LFE_LFO_SHAPE_SINE:
        return lfe_wt_sine_lookup(ph);

    case LFE_LFO_SHAPE_TRIANGLE: {
        /* Triangle: first half ramps -1 → +1, second half ramps +1 → -1.
         * For the first half (top bit of phase clear), take the next 15
         * bits of phase as a Q15 fraction in [0, Q15_ONE) and scale up
         * by 2, then offset by -Q15_ONE. For the second half the same
         * but negated offset. */
        uint32_t p = ph;
        int32_t mag;
        if (p & 0x80000000u) {
            uint32_t pp = p & 0x7FFFFFFFu;          /* [0, 2^31) */
            int32_t frac_q15 = (int32_t)(pp >> 16); /* [0, Q15_ONE) */
            mag = (int32_t)Q15_ONE - frac_q15 * 2;
        } else {
            int32_t frac_q15 = (int32_t)(p >> 16);  /* [0, Q15_ONE) */
            mag = -(int32_t)Q15_ONE + frac_q15 * 2;
        }
        if (mag >  Q15_ONE) mag =  Q15_ONE;
        if (mag < -Q15_ONE) mag = -Q15_ONE;
        return (q15_t)mag;
    }

    case LFE_LFO_SHAPE_SQUARE:
        return (ph & 0x80000000u) ? (q15_t)(-Q15_ONE) : (q15_t)Q15_ONE;

    case LFE_LFO_SHAPE_SAW_UP:
        /* Linear ramp from -1 to +1 across the cycle. */
        return (q15_t)((int32_t)(ph >> 16) - 0x8000);

    case LFE_LFO_SHAPE_SAW_DOWN:
        return (q15_t)(0x7FFF - (int32_t)(ph >> 16));

    default:
        return 0;
    }
}
