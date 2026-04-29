/*
 * fx_filter.c — Biquad filter effect on a sample selection.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Initializes a fresh biquad (LP/HP/BP/Notch per caller-chosen mode),
 * streams the selection through it, writes back in place with a dry/wet
 * blend. The biquad primitive lives in util/biquad.h — Q29 coefs, Q29
 * internal state, so narrow-band cutoffs and high resonance both work
 * cleanly. See that file for the precision rationale.
 *
 * State is reset at the start of every call — the selection boundary
 * therefore does NOT benefit from any "pre-roll" warmup. The first few
 * samples may click slightly if the signal isn't near zero at
 * range.start. The editor UI is expected to offer a crossfade/undo
 * workflow; baking pre-roll into the library adds complexity we don't
 * need yet.
 *
 * The `q` field of lfe_fx_filter_params is a normalized resonance knob:
 * 0 means critically damped (no peak), Q15_ONE means near self-
 * oscillation. The mapping is exponential so low-end control is usable.
 */

#include "fx_common.h"

#include "util/biquad.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Map the normalized resonance knob (Q15: 0 → flat, Q15_ONE → very
 * resonant) to a biquad Q factor. Exponential mapping gives useful
 * control across the full range — Q=0.5 at knob=0, Q=40 at knob=Q15_ONE,
 * with the familiar "audible peak starts around knob=50%" feel.
 */
static double knob_to_q(uint16_t q_q15)
{
    if (q_q15 >= Q15_ONE) q_q15 = Q15_ONE;
    double t = (double)q_q15 / (double)Q15_ONE;       /* 0..1 */
    /* 0.5 * (80^t): t=0 → 0.5, t=0.5 → 4.47, t=1 → 40. */
    return 0.5 * pow(80.0, t);
}

LFE_HOT
lfe_status lfe_fx_filter(lfe_buffer *buf,
                         const lfe_fx_range *range,
                         const lfe_fx_filter_params *p)
{
    uint32_t count;
    lfe_status rc = lfe_fx_validate_range(buf, range, &count);
    if (rc != LFE_OK) return rc;
    if (!p) return LFE_ERR_NULL;
    if (count == 0) return LFE_OK;

    const uint32_t sr = (uint32_t)buf->rate;
    if (sr == 0) return LFE_ERR_BAD_PARAM;

    double q = knob_to_q(p->q);

    lfe_biquad_state bq;
    switch (p->mode) {
    case LFE_DRUM_FILTER_LP:    lfe_biquad_init_lp   (&bq, p->cutoff_hz, sr, q); break;
    case LFE_DRUM_FILTER_HP:    lfe_biquad_init_hp   (&bq, p->cutoff_hz, sr, q); break;
    case LFE_DRUM_FILTER_BP:    lfe_biquad_init_bp   (&bq, p->cutoff_hz, sr, q); break;
    case LFE_DRUM_FILTER_NOTCH: lfe_biquad_init_notch(&bq, p->cutoff_hz, sr, q); break;
    default:                    return LFE_ERR_BAD_PARAM;
    }

    lfe_sample_t *data = buf->data + range->start;
    const uint16_t mix = p->mix;
    bool clipped = false;

    for (uint32_t i = 0; i < count; i++) {
        q15_t dry = data[i];
        q15_t wet = lfe_biquad_step(&bq, dry);
        int32_t out = lfe_fx_mix_drywet(dry, wet, mix);
        lfe_fx_write_sat(&data[i], out, &clipped);
    }

    return clipped ? LFE_WARN_CLIPPED : LFE_OK;
}
