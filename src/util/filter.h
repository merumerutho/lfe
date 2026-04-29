/*
 * filter.h — Chamberlin state-variable filter (internal).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Two-pole state-variable filter with simultaneous lowpass, highpass,
 * bandpass, and notch outputs. Cutoff and resonance are settable at
 * runtime. Stable across the audible frequency range up to about
 * sample_rate/4 (~8 kHz at 32 kHz sampling), which covers everything
 * the percussion and subtractive generators will throw at it.
 *
 * The Chamberlin SVF is well-known and well-documented. The recurrence
 * is:
 *
 *   hp[n] = input[n] - lp[n-1] - q * bp[n-1]
 *   bp[n] = bp[n-1] + f * hp[n]
 *   lp[n] = lp[n-1] + f * bp[n]
 *
 * where:
 *   f ≈ 2 * sin(pi * cutoff / sample_rate)   [stability bound: f < sqrt(2)]
 *   q = 1/Q                                  [damping factor, lower = more resonant]
 *
 * For very low cutoffs and high resonance, you may want a more stable
 * filter topology (e.g. trapezoidal-integrator SVF), but Chamberlin is
 * a good fit for percussion bodies and synth filter sweeps.
 *
 * Internal header — not exposed via lfe.h.
 */

#ifndef LFE_UTIL_FILTER_H
#define LFE_UTIL_FILTER_H

#include <stdint.h>

#include "fixed.h"

typedef enum {
    LFE_FILTER_LP = 0,   /* Lowpass */
    LFE_FILTER_HP,       /* Highpass */
    LFE_FILTER_BP,       /* Bandpass */
    LFE_FILTER_NOTCH,    /* Notch (band-reject) */
} lfe_filter_mode;

typedef struct {
    /* Coefficients in Q15. */
    int32_t f;   /* Frequency, ≈ 2*sin(pi*cutoff/sample_rate) */
    int32_t q;   /* Damping = 1/Q, lower means more resonant */

    /* Filter state nominally in Q31, but stored as int64_t. The
     * extra bits are pure headroom: with Q-factor near 1 the LP
     * overshoots ~2x its steady-state value during the transient
     * response, which would wrap a Q31 stored as int32_t and corrupt
     * the filter state forever. int64_t holds the overshoot trivially
     * and the per-step performance cost on ARM9 is small (a couple
     * extra instructions, only matters for offline rendering anyway). */
    int64_t lp;
    int64_t bp;

    lfe_filter_mode mode;
} lfe_filter_state;

/*
 * Initialize a filter to the requested mode with default cutoff
 * (sample_rate / 4) and Q ~= 0.7 (no resonance, slightly damped).
 * State is reset to zero.
 */
void lfe_filter_init(lfe_filter_state *flt, lfe_filter_mode mode);

/*
 * Set the cutoff frequency. Computes f = 2*sin(pi*cutoff/sample_rate)
 * and stores it as Q15. Cutoffs above sample_rate/4 are clamped down
 * because the Chamberlin SVF becomes unstable above that.
 */
void lfe_filter_set_cutoff(lfe_filter_state *flt,
                           uint32_t cutoff_hz,
                           uint32_t sample_rate);

/*
 * Set the resonance. Lower q means more resonance. Standard values:
 *   q = 0x7FFF (1.0 in Q15) — Q=1, mildly colored (~0 dB bump at cutoff);
 *                             this is "resonance off" in analog-synth terms,
 *                             NOT Butterworth (which would need damping ≈ √2,
 *                             unreachable in Q15)
 *   q = 0x4000 (0.5 in Q15) — Q=2, moderate resonance
 *   q = 0x1000 (0.125)      — Q=8, sharp resonant peak
 *   q = 0x0400 (~0.031)     — Q≈32, near-screaming (lower internal clamp)
 *
 * Input is clamped to [0x0400, Q15_ONE]. Self-oscillation (q→0) is
 * excluded on purpose — it parks the output at the saturator rails and
 * is rarely what a patch or modulator actually wants.
 */
void lfe_filter_set_q(lfe_filter_state *flt, q15_t damping);

/*
 * Reset the filter state to zero. Use between sounds or when you
 * want a clean start without going through init again.
 */
void lfe_filter_reset(lfe_filter_state *flt);

/*
 * Process one sample. Returns the output corresponding to the filter's
 * current mode (LP/HP/BP/Notch).
 */
q15_t lfe_filter_step(lfe_filter_state *flt, q15_t input);

#endif /* LFE_UTIL_FILTER_H */
