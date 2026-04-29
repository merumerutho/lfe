/*
 * crossover.h — 4th-order Linkwitz-Riley crossover (internal).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Splits one sample stream into low and high bands with 24 dB/oct
 * slopes, built from two cascaded Butterworth biquads per band (LR4 =
 * cascaded Butterworth). The low+high magnitude sum is approximately
 * flat across the crossover frequency — good enough for multiband
 * dynamics processing where the bands will be re-summed after
 * modification.
 *
 * Used by fx_ott for the 88 Hz L/M and ~2500 Hz M/H crossovers. The
 * chain is serial: input → LR4_low → (LOW, above_low) ; above_low →
 * LR4_high → (MID, HIGH). This is the simpler "serial" topology used
 * by Xfer OTT and similar plugins; perfect reconstruction at bypass
 * has a small phase wobble around the two crossover points, which is
 * not audible in a compression context.
 *
 * Internal header — not exposed via lfe.h.
 */

#ifndef LFE_UTIL_CROSSOVER_H
#define LFE_UTIL_CROSSOVER_H

#include <stdint.h>

#include "biquad.h"
#include "fixed.h"

typedef struct {
    lfe_biquad_state lp1, lp2;   /* cascaded 2nd-order LPF → 4th-order LR4 LP */
    lfe_biquad_state hp1, hp2;   /* cascaded 2nd-order HPF → 4th-order LR4 HP */
} lfe_lr4_state;

/* Initialize both sections at the given crossover frequency. */
void lfe_lr4_init(lfe_lr4_state *x,
                  uint32_t cutoff_hz,
                  uint32_t sample_rate);

/* Reset all four biquad histories to zero. Coefficients unchanged. */
void lfe_lr4_reset(lfe_lr4_state *x);

/* Process one sample, producing the low and high band outputs. */
void lfe_lr4_step(lfe_lr4_state *x,
                  q15_t in,
                  q15_t *out_low,
                  q15_t *out_high);

#endif /* LFE_UTIL_CROSSOVER_H */
