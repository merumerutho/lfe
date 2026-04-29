/*
 * env_follower.h — One-pole peak envelope follower (internal).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tracks the magnitude of a signal with independent attack and release
 * time constants:
 *
 *   env[n] = env[n-1] + alpha * (|x[n]| - env[n-1])
 *
 * where alpha = alpha_attack if |x[n]| >= env[n-1], else alpha_release.
 *
 * Used by fx_ott for per-band level detection. Attack is typically
 * fast (~1 ms) so the compressor reacts to transients, release is
 * slower (~150 ms) so the gain reduction doesn't pump.
 *
 * Internal header — not exposed via lfe.h.
 */

#ifndef LFE_UTIL_ENV_FOLLOWER_H
#define LFE_UTIL_ENV_FOLLOWER_H

#include <stdint.h>

#include "fixed.h"

typedef struct {
    int32_t alpha_atk;   /* Q15 in [0, Q15_ONE] */
    int32_t alpha_rel;   /* Q15 in [0, Q15_ONE] */
    int32_t env;         /* Q15 magnitude, int32 for headroom */
} lfe_env_follower_state;

/*
 * Initialize the follower with time constants in milliseconds.
 * Zero ms clamps to an "instantaneous" alpha (Q15_ONE). State starts
 * at zero.
 */
void lfe_env_follower_init(lfe_env_follower_state *f,
                           uint32_t attack_ms,
                           uint32_t release_ms,
                           uint32_t sample_rate);

/* Zero the envelope state. Coefficients unchanged. */
void lfe_env_follower_reset(lfe_env_follower_state *f);

/* Advance one sample; returns the tracked magnitude as Q15. */
q15_t lfe_env_follower_step(lfe_env_follower_state *f, q15_t in);

#endif /* LFE_UTIL_ENV_FOLLOWER_H */
