/*
 * biquad.h — Direct-form-I biquad (internal).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Added for the OTT multiband-compressor FX: the Chamberlin SVF in
 * filter.h can't reach Butterworth damping (its Q15 damping is clamped
 * to ≤ 1.0 while Butterworth needs √2 ≈ 1.414), and a proper 4th-order
 * Linkwitz-Riley crossover is built from two cascaded 2nd-order
 * Butterworth biquads.
 *
 * Storage:
 *   - Coefficients in Q29 (int32, range ±4).
 *   - Input and output state in Q29 as well (NOT Q15 like most DSP
 *     libraries). The feedback path of a narrow-band biquad is extremely
 *     sensitive to state precision: with Q15 state the sub-integer y
 *     values get truncated before feeding back through a1*y, and the
 *     lost contribution compounds over each recursion cycle, producing
 *     large systematic magnitude errors at stopband frequencies. Q29
 *     state preserves ~14 extra bits of precision, which is enough for
 *     LR4 at 88 Hz / 32 kHz (pole magnitude ~0.988).
 *
 * Internal header — not exposed via lfe.h.
 */

#ifndef LFE_UTIL_BIQUAD_H
#define LFE_UTIL_BIQUAD_H

#include <stdint.h>

#include "fixed.h"

typedef struct {
    /* Transfer function coefficients, Q29. a0 normalized to 1.
     *   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
     */
    int32_t b0, b1, b2;
    int32_t a1, a2;

    /* Direct Form I state, all in Q29. High-precision state is what
     * makes the filter work at low cutoffs. */
    int32_t x1, x2;
    int32_t y1, y2;
} lfe_biquad_state;

/*
 * Initialize a 2nd-order Butterworth (Q = 1/√2) lowpass biquad at the
 * requested cutoff. Clamps cutoff to (0, sample_rate/2). Used for the
 * LR4 crossover where fixed Butterworth Q is the whole point.
 */
void lfe_biquad_init_lp_butter(lfe_biquad_state *bq,
                               uint32_t cutoff_hz,
                               uint32_t sample_rate);

/*
 * Same, highpass.
 */
void lfe_biquad_init_hp_butter(lfe_biquad_state *bq,
                               uint32_t cutoff_hz,
                               uint32_t sample_rate);

/*
 * Variable-Q initializers (RBJ Audio EQ Cookbook). `q` is the filter's
 * quality factor as a double — 0.5 = critically damped, 0.707 =
 * Butterworth, 1 = mildly peaked, 10+ = obviously resonant, 40+ = near
 * self-oscillation. Not hot-path; init uses doubles.
 *
 *   lfe_biquad_init_lp     — lowpass, DC gain 1, Nyquist gain 0
 *   lfe_biquad_init_hp     — highpass, DC gain 0, Nyquist gain 1
 *   lfe_biquad_init_bp     — bandpass, peak gain 1 at cutoff (constant-0-dB-peak variant)
 *   lfe_biquad_init_notch  — notch, passes DC and Nyquist, nulls at cutoff
 */
void lfe_biquad_init_lp(lfe_biquad_state *bq,
                        uint32_t cutoff_hz, uint32_t sample_rate, double q);
void lfe_biquad_init_hp(lfe_biquad_state *bq,
                        uint32_t cutoff_hz, uint32_t sample_rate, double q);
void lfe_biquad_init_bp(lfe_biquad_state *bq,
                        uint32_t cutoff_hz, uint32_t sample_rate, double q);
void lfe_biquad_init_notch(lfe_biquad_state *bq,
                           uint32_t cutoff_hz, uint32_t sample_rate, double q);

/* Zero the state without touching coefficients. */
void lfe_biquad_reset(lfe_biquad_state *bq);

/* Process one sample. Returned value is saturated Q15. State is
 * updated from the full-precision internal result. */
q15_t lfe_biquad_step(lfe_biquad_state *bq, q15_t in);

#endif /* LFE_UTIL_BIQUAD_H */
