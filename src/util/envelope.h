/*
 * envelope.h — ADSR envelope generator (internal).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Stateful linear ADSR. Internal Q31 level for precision over long
 * envelopes; output is Q15. The envelope progresses through phases:
 *
 *   IDLE → (trigger) → ATTACK → DECAY → SUSTAIN → (release) → RELEASE → IDLE
 *
 * Generators use this internally to shape oscillator/noise output. The
 * tests in lib/lfe/test/ also include this header directly because the
 * test build adds -Isrc to CFLAGS.
 *
 * This header is internal — it is NOT exposed via lfe.h. Outside callers
 * (maxtracker, future external users) should never include it.
 */

#ifndef LFE_UTIL_ENVELOPE_H
#define LFE_UTIL_ENVELOPE_H

#include <stdint.h>
#include <stdbool.h>

#include "fixed.h"

typedef enum {
    LFE_ENV_IDLE = 0,
    LFE_ENV_ATTACK,
    LFE_ENV_DECAY,
    LFE_ENV_SUSTAIN,
    LFE_ENV_RELEASE,
} lfe_env_phase;

/*
 * ADSR parameters in user-friendly units. The init function converts
 * these into per-sample increments stored on the state object.
 *
 *   attack_ms     — time in ms to ramp from 0 to peak_level
 *   decay_ms      — time in ms to ramp from peak_level to sustain_level
 *   sustain_level — Q15 [0, Q15_ONE], plateau level after decay
 *   release_ms    — time in ms to ramp from peak_level to 0 once released
 *   peak_level    — Q15 [0, Q15_ONE], the level reached at end of attack
 *
 * Setting any *_ms to 0 produces an instantaneous transition through
 * that phase. Setting peak_level == 0 produces a silent envelope.
 *
 * Note: release_ms is "time to fall from peak to zero", a fixed slope.
 * If you release before reaching sustain, the envelope falls from
 * wherever it currently is at that same slope, finishing earlier than
 * release_ms in wall-clock terms. This is the standard simple-ADSR
 * behavior; matches what most analog synths do.
 */
typedef struct {
    uint32_t attack_ms;
    uint32_t decay_ms;
    q15_t    sustain_level;
    uint32_t release_ms;
    q15_t    peak_level;
} lfe_env_params;

typedef struct {
    /* ---- Computed at init time, treated as read-only afterward ---- */

    /* Per-sample increments in Q31. attack/decay/release are signed:
     * attack is positive (ramping up), decay/release are negative
     * (ramping down). */
    int32_t attack_inc;
    int32_t decay_inc;
    int32_t release_inc;

    /* Target levels in Q31. */
    int32_t peak_q31;
    int32_t sustain_q31;

    /* ---- Runtime state ---- */

    lfe_env_phase phase;
    int32_t       level_q31;  /* Q31 [0, Q31_ONE] */
} lfe_env_state;

/*
 * Initialize an envelope from user params and the target sample rate.
 * Sets phase to IDLE and level to 0. Calling this is required before
 * any other envelope function.
 */
void lfe_env_init(lfe_env_state *env,
                  const lfe_env_params *p,
                  uint32_t sample_rate);

/*
 * Trigger the envelope (note on). Resets level to 0 and starts the
 * attack phase. Equivalent of "key down" on a synth.
 */
void lfe_env_trigger(lfe_env_state *env);

/*
 * Release the envelope (note off). Transitions to release phase from
 * the current level. Equivalent of "key up" on a synth.
 */
void lfe_env_release(lfe_env_state *env);

/*
 * Advance one sample and return the current level as Q15. Caller is
 * responsible for multiplying this by the audio source they're shaping.
 */
q15_t lfe_env_step(lfe_env_state *env);

/*
 * True when the envelope has finished its release and is back to IDLE.
 * Generators use this to know when a voice can be deallocated.
 */
bool lfe_env_is_idle(const lfe_env_state *env);

#endif /* LFE_UTIL_ENVELOPE_H */
