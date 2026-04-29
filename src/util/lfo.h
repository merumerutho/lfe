/*
 * lfo.h — Low-frequency oscillator (internal stepper).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Stateful unipolar-phase LFO producing a signed Q15 sample stream in
 * [-Q15_ONE, +Q15_ONE]. Shapes: sine, triangle, square, saw-up,
 * saw-down. Rate is Q24.8 Hz (typically 0.1..20 Hz for musical
 * modulation, higher for dubstep wobble).
 *
 * The shape enum lives in the public lfe.h (`lfe_lfo_shape`) so engine
 * params and UI can reference it uniformly. This header only owns the
 * internal state struct + the step/init entry points; generators embed
 * `lfe_lfo_state` inline and call `lfe_lfo_step()` per sample.
 */

#ifndef LFE_UTIL_LFO_H
#define LFE_UTIL_LFO_H

#include <stdint.h>

#include "lfe.h"     /* for lfe_lfo_shape */
#include "fixed.h"

typedef struct {
    lfe_phase_t   phase;
    lfe_phase_t   phase_inc;
    lfe_lfo_shape shape;
} lfe_lfo_state;

/*
 * Initialize an LFO. Resets phase to 0, computes phase increment from
 * the rate and sample rate. Safe to call multiple times.
 */
void lfe_lfo_init(lfe_lfo_state *lfo,
                  lfe_lfo_shape shape,
                  q24_8_t rate_hz_q8,
                  uint32_t sample_rate);

/*
 * Advance one sample and return the LFO value as signed Q15 in
 * [-Q15_ONE, +Q15_ONE]. Caller multiplies by its depth and adds
 * the result to whatever destination it is modulating.
 */
q15_t lfe_lfo_step(lfe_lfo_state *lfo);

#endif /* LFE_UTIL_LFO_H */
