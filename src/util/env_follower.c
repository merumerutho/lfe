/*
 * env_follower.c — One-pole peak envelope follower implementation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "env_follower.h"
#include "platform.h"

#include <math.h>

static int32_t time_to_alpha_q15(uint32_t time_ms, uint32_t sample_rate)
{
    if (time_ms == 0 || sample_rate == 0) return Q15_ONE;
    double tau_samples = (double)time_ms * 1e-3 * (double)sample_rate;
    if (tau_samples < 1.0) return Q15_ONE;
    double alpha = 1.0 - exp(-1.0 / tau_samples);
    int32_t q = (int32_t)(alpha * (double)Q15_ONE + 0.5);
    if (q < 1)        q = 1;
    if (q > Q15_ONE)  q = Q15_ONE;
    return q;
}

void lfe_env_follower_init(lfe_env_follower_state *f,
                           uint32_t attack_ms,
                           uint32_t release_ms,
                           uint32_t sample_rate)
{
    if (!f) return;
    f->alpha_atk = time_to_alpha_q15(attack_ms,  sample_rate);
    f->alpha_rel = time_to_alpha_q15(release_ms, sample_rate);
    f->env       = 0;
}

void lfe_env_follower_reset(lfe_env_follower_state *f)
{
    if (!f) return;
    f->env = 0;
}

LFE_HOT
q15_t lfe_env_follower_step(lfe_env_follower_state *f, q15_t in)
{
    int32_t mag = in >= 0 ? (int32_t)in : -(int32_t)in;
    int32_t alpha = (mag >= f->env) ? f->alpha_atk : f->alpha_rel;
    int32_t delta = mag - f->env;
    f->env += (delta * alpha) >> 15;
    if (f->env < 0)       f->env = 0;
    if (f->env > Q15_ONE) f->env = Q15_ONE;
    return (q15_t)f->env;
}
