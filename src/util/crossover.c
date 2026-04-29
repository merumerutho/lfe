/*
 * crossover.c — LR4 crossover implementation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "crossover.h"
#include "platform.h"

void lfe_lr4_init(lfe_lr4_state *x,
                  uint32_t cutoff_hz,
                  uint32_t sample_rate)
{
    if (!x) return;
    lfe_biquad_init_lp_butter(&x->lp1, cutoff_hz, sample_rate);
    lfe_biquad_init_lp_butter(&x->lp2, cutoff_hz, sample_rate);
    lfe_biquad_init_hp_butter(&x->hp1, cutoff_hz, sample_rate);
    lfe_biquad_init_hp_butter(&x->hp2, cutoff_hz, sample_rate);
}

void lfe_lr4_reset(lfe_lr4_state *x)
{
    if (!x) return;
    lfe_biquad_reset(&x->lp1);
    lfe_biquad_reset(&x->lp2);
    lfe_biquad_reset(&x->hp1);
    lfe_biquad_reset(&x->hp2);
}

LFE_HOT
void lfe_lr4_step(lfe_lr4_state *x,
                  q15_t in,
                  q15_t *out_low,
                  q15_t *out_high)
{
    q15_t lo = lfe_biquad_step(&x->lp1, in);
    lo       = lfe_biquad_step(&x->lp2, lo);

    q15_t hi = lfe_biquad_step(&x->hp1, in);
    hi       = lfe_biquad_step(&x->hp2, hi);

    *out_low  = lo;
    *out_high = hi;
}
