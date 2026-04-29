/*
 * filter.c — Chamberlin state-variable filter implementation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The hot path is small enough to live in this .c file rather than
 * inlining; we mark it LFE_HOT so it ends up in ITCM on the NDS build.
 */

#include "filter.h"
#include "platform.h"

#include <math.h>

/*
 * Convert a cutoff in Hz to the SVF f coefficient in Q15.
 *
 * Mathematically: f = 2 * sin(pi * cutoff / sample_rate)
 *
 * For stability, the Chamberlin SVF requires f < sqrt(2). We cap at
 * sample_rate / 4 (where f ≈ 2 * sin(pi/4) ≈ 1.41) to stay safe.
 *
 * This runs at set-cutoff time, not in the hot loop, so libm sin()
 * is fine. Generators that change cutoff per-sample (filter sweeps)
 * should call set_cutoff infrequently or precompute a table.
 */
static int32_t cutoff_to_f_q15(uint32_t cutoff_hz, uint32_t sample_rate)
{
    if (sample_rate == 0) return 0;

    /* Stability cap. */
    uint32_t max_cutoff = sample_rate / 4u;
    if (cutoff_hz > max_cutoff) cutoff_hz = max_cutoff;

    double f = 2.0 * sin(3.14159265358979323846 *
                         (double)cutoff_hz / (double)sample_rate);

    /* Scale to Q15 (32768 = 1.0). The cap above ensures f < ~1.41,
     * which is well within int32_t range when stored as Q15. */
    int32_t q15 = (int32_t)(f * 32768.0);
    if (q15 > 32767) q15 = 32767;
    if (q15 < 0)     q15 = 0;
    return q15;
}

void lfe_filter_init(lfe_filter_state *flt, lfe_filter_mode mode)
{
    if (!flt) return;
    flt->mode = mode;
    flt->lp   = 0;
    flt->bp   = 0;

    /* Defaults: cutoff at sample_rate/4 isn't useful without knowing
     * the actual sample rate, so the user is expected to call
     * set_cutoff before processing audio. We set safe defaults anyway. */
    flt->f = (int32_t)(0.5 * 32768.0);  /* arbitrary mid-band */
    flt->q = (int32_t)(0.7 * 32768.0);  /* slightly damped */
}

void lfe_filter_set_cutoff(lfe_filter_state *flt,
                           uint32_t cutoff_hz,
                           uint32_t sample_rate)
{
    if (!flt) return;
    flt->f = cutoff_to_f_q15(cutoff_hz, sample_rate);
}

void lfe_filter_set_q(lfe_filter_state *flt, q15_t damping)
{
    if (!flt) return;
    /* Lower clamp keeps the filter out of self-oscillation — below ~0x0400
     * the resonant peak parks the output at the saturator rails regardless
     * of input. Upper clamp is Q=1 (mildly colored, analog-synth-style
     * "resonance off"), not Butterworth; Butterworth needs damping ≈ √2
     * which doesn't fit in Q15. */
    if (damping < 0x0400) damping = 0x0400;
    if (damping > Q15_ONE) damping = Q15_ONE;
    flt->q = damping;
}

void lfe_filter_reset(lfe_filter_state *flt)
{
    if (!flt) return;
    flt->lp = 0;
    flt->bp = 0;
}

LFE_HOT
q15_t lfe_filter_step(lfe_filter_state *flt, q15_t input)
{
    /* Music DSP / standard Hal Chamberlin SVF ordering. State variables
     * `lp` and `bp` are kept in Q31 semantics but stored in int64_t for
     * overshoot headroom (see filter.h for the rationale). Coefficients
     * `f` and `q` remain Q15. Input is promoted from Q15 to Q31 once.
     *
     * The order of updates is critical:
     *
     *   1. lp = lp + f * bp        (uses prior bp)
     *   2. hp = input - lp - q*bp  (uses NEW lp, prior bp)
     *   3. bp = bp + f * hp        (uses prior bp, NEW hp)
     */
    int64_t in_q31 = (int64_t)((int32_t)input << 16);
    int64_t f      = flt->f;
    int64_t q      = flt->q;

    /* 1. lp += f * bp (prior bp) */
    flt->lp += (f * flt->bp) >> 15;

    /* 2. hp = input - lp - q * bp (new lp, prior bp) */
    int64_t q_bp = (q * flt->bp) >> 15;
    int64_t hp   = in_q31 - flt->lp - q_bp;

    /* 3. bp += f * hp (prior bp, new hp) */
    flt->bp += (f * hp) >> 15;

    /* Choose the output. */
    int64_t out_q31 = 0;
    switch (flt->mode) {
    case LFE_FILTER_LP:    out_q31 = flt->lp;      break;
    case LFE_FILTER_HP:    out_q31 = hp;           break;
    case LFE_FILTER_BP:    out_q31 = flt->bp;      break;
    case LFE_FILTER_NOTCH: out_q31 = hp + flt->lp; break;
    }

    /* Q31 → Q15 with saturation. Resonant peaks can push the state
     * past Q15 range during the transient; the saturation keeps the
     * audible output well-defined while the int64 internal state
     * preserves the filter math. */
    int32_t out_q15 = (int32_t)(out_q31 >> 16);
    return lfe_sat_q15(out_q15);
}
