/*
 * envelope.c — ADSR envelope generator implementation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Linear ramps in Q31 internal precision. The init function converts
 * user-friendly millisecond durations into per-sample Q31 increments;
 * the step function adds the increment, checks for phase transitions,
 * and clamps to the target level on overshoot.
 *
 * The Q31 internal representation matters because per-sample increments
 * for slow envelopes (e.g. 1-second attack at 32 kHz = 32000 samples)
 * are tiny — Q15 increments would round to 0. Using Q31 gives 16 extra
 * fractional bits of resolution, which is enough for envelopes lasting
 * up to a few minutes without precision loss.
 */

#include "envelope.h"

#include <stdbool.h>

/*
 * Convert a Q15 level to a Q31 level. Q15 has 15 fractional bits,
 * Q31 has 31; the conversion shifts left by 16. The result fits in
 * int32_t for any valid Q15 input.
 */
static inline int32_t q15_to_q31(q15_t v)
{
    return ((int32_t)v) << 16;
}

/*
 * Compute the Q31 increment per sample to traverse `delta_q31` over
 * `duration_ms` milliseconds at `sample_rate` Hz. Returns delta_q31
 * unchanged if duration_ms is zero, so a "0 ms" phase completes in
 * exactly one step. Sign of the result follows the sign of delta.
 */
static int32_t compute_inc(int32_t delta_q31,
                           uint32_t duration_ms,
                           uint32_t sample_rate)
{
    if (duration_ms == 0)
        return delta_q31;

    /* Number of samples for this phase. At least 1 to avoid div-by-0
     * in the unlikely case of (duration_ms * sample_rate < 1000). */
    uint32_t samples = (duration_ms * sample_rate) / 1000u;
    if (samples == 0) samples = 1;

    return delta_q31 / (int32_t)samples;
}

void lfe_env_init(lfe_env_state *env,
                  const lfe_env_params *p,
                  uint32_t sample_rate)
{
    if (!env || !p || sample_rate == 0) return;

    /* Clamp peak and sustain to valid Q15 range. */
    q15_t peak    = p->peak_level;
    q15_t sustain = p->sustain_level;
    if (peak    > Q15_ONE) peak    = Q15_ONE;
    if (peak    < 0)       peak    = 0;
    if (sustain > peak)    sustain = peak;
    if (sustain < 0)       sustain = 0;

    env->peak_q31    = q15_to_q31(peak);
    env->sustain_q31 = q15_to_q31(sustain);

    /* Attack: 0 → peak. Increment is positive. */
    env->attack_inc = compute_inc(env->peak_q31,
                                  p->attack_ms, sample_rate);

    /* Decay: peak → sustain. Delta is negative when sustain < peak. */
    env->decay_inc = compute_inc(env->sustain_q31 - env->peak_q31,
                                 p->decay_ms, sample_rate);

    /* Release: peak → 0 at a fixed slope. The actual time to reach 0
     * from a release-time level may be shorter than release_ms (see
     * the header comment). */
    env->release_inc = compute_inc(-env->peak_q31,
                                   p->release_ms, sample_rate);

    env->phase     = LFE_ENV_IDLE;
    env->level_q31 = 0;
}

void lfe_env_trigger(lfe_env_state *env)
{
    if (!env) return;
    env->phase     = LFE_ENV_ATTACK;
    env->level_q31 = 0;
}

void lfe_env_release(lfe_env_state *env)
{
    if (!env) return;
    if (env->phase == LFE_ENV_IDLE) return;
    env->phase = LFE_ENV_RELEASE;
}

q15_t lfe_env_step(lfe_env_state *env)
{
    if (!env) return 0;

    /* Use int64 for the level addition to avoid int32 overflow when
     * the envelope is just below peak/0 and the increment would push
     * it past INT32_MAX/MIN. The narrow attack at 10 ms / 32 kHz hits
     * this case: peak_q31 = 0x7FFF0000 ≈ INT32_MAX, and one step past
     * the last "below peak" sample would wrap. Promoting to int64 for
     * the comparison avoids the wrap entirely. */
    int64_t new_level;

    switch (env->phase) {
    case LFE_ENV_IDLE:
        /* No motion. */
        break;

    case LFE_ENV_ATTACK:
        new_level = (int64_t)env->level_q31 + env->attack_inc;
        if (new_level >= env->peak_q31) {
            env->level_q31 = env->peak_q31;
            env->phase     = LFE_ENV_DECAY;
        } else {
            env->level_q31 = (int32_t)new_level;
        }
        break;

    case LFE_ENV_DECAY:
        new_level = (int64_t)env->level_q31 + env->decay_inc;  /* decay_inc <= 0 */
        if (new_level <= env->sustain_q31) {
            env->level_q31 = env->sustain_q31;
            env->phase     = LFE_ENV_SUSTAIN;
        } else {
            env->level_q31 = (int32_t)new_level;
        }
        break;

    case LFE_ENV_SUSTAIN:
        /* Hold at sustain until released. */
        break;

    case LFE_ENV_RELEASE:
        new_level = (int64_t)env->level_q31 + env->release_inc;  /* release_inc <= 0 */
        if (new_level <= 0) {
            env->level_q31 = 0;
            env->phase     = LFE_ENV_IDLE;
        } else {
            env->level_q31 = (int32_t)new_level;
        }
        break;
    }

    /* Q31 → Q15 by truncating the bottom 16 bits. */
    return (q15_t)(env->level_q31 >> 16);
}

bool lfe_env_is_idle(const lfe_env_state *env)
{
    return env && env->phase == LFE_ENV_IDLE;
}
