/*
 * fx_delay.c — Mono single-tap delay with feedback, on a selection.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Classic circular-buffer delay line. The caller owns the scratch
 * buffer (the library never allocates — NDS has no heap) and must
 * supply at least `delay_ms * sr / 1000` samples of storage. The
 * library zeroes the scratch on entry so the caller can reuse the
 * same buffer across multiple passes.
 *
 * Signal flow for each sample within the selection:
 *
 *   tap        = line[read_pos]
 *   wet        = tap
 *   line[wpos] = in + (tap * feedback)  (saturating)
 *   out        = dry * (1-mix) + wet * mix
 *
 * Delay tail: because we operate strictly on [range.start, range.end),
 * any echoes that would land past range.end are *not* written back
 * into the buffer. This is the "truncate at selection end" policy
 * documented in the public header. If you want bleed-through tails,
 * extend the selection to include the tail region before calling.
 */

#include "fx_common.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>   /* for memset */

LFE_HOT
lfe_status lfe_fx_delay(lfe_buffer *buf,
                        const lfe_fx_range *range,
                        const lfe_fx_delay_params *p)
{
    uint32_t count;
    lfe_status rc = lfe_fx_validate_range(buf, range, &count);
    if (rc != LFE_OK) return rc;
    if (!p) return LFE_ERR_NULL;
    if (count == 0) return LFE_OK;

    const uint32_t sr = (uint32_t)buf->rate;
    if (sr == 0) return LFE_ERR_BAD_PARAM;

    /* Compute required delay line size in samples. Zero-delay is
     * illegal (would pass the signal through with no effect, with
     * the added overhead of a useless memcpy through scratch). */
    const uint64_t delay_samples_64 = ((uint64_t)p->delay_ms * sr) / 1000u;
    if (delay_samples_64 == 0)          return LFE_ERR_BAD_PARAM;
    if (delay_samples_64 > UINT32_MAX)  return LFE_ERR_BAD_PARAM;
    const uint32_t delay_samples = (uint32_t)delay_samples_64;

    if (!p->scratch)                          return LFE_ERR_NULL;
    if (p->scratch_length < delay_samples)    return LFE_ERR_BUF_TOO_SMALL;

    /* Zero the delay line so we start with silence in the taps. */
    memset(p->scratch, 0, delay_samples * sizeof(lfe_sample_t));

    lfe_sample_t *data = buf->data + range->start;
    lfe_sample_t *line = p->scratch;
    const uint32_t line_len = delay_samples;
    const int32_t  feedback = (int32_t)p->feedback;
    const uint16_t mix      = p->mix;

    /* Write position walks forward; read position is always
     * write_pos (modulo line_len) because we read the "oldest" slot
     * just before overwriting it. That gives us exactly `line_len`
     * samples of delay in a minimal circular buffer. */
    uint32_t wpos = 0;
    bool clipped = false;

    for (uint32_t i = 0; i < count; i++) {
        q15_t dry = data[i];

        /* Read tap (the oldest sample in the line — what we recorded
         * `delay_samples` samples ago). */
        q15_t tap = line[wpos];

        /* Write new value into the line: in + (tap * feedback). Use
         * Q15 multiply and saturate so the feedback loop can't blow
         * up regardless of feedback strength. */
        int32_t new_val = (int32_t)dry + (((int32_t)tap * feedback) >> 15);
        if (new_val >  Q15_ONE)     { new_val =  Q15_ONE;     clipped = true; }
        if (new_val < -Q15_ONE - 1) { new_val = -Q15_ONE - 1; clipped = true; }
        line[wpos] = (lfe_sample_t)new_val;

        wpos++;
        if (wpos >= line_len) wpos = 0;

        /* Output: dry + wet blend. Wet is the tap we read above. */
        int32_t out = lfe_fx_mix_drywet(dry, tap, mix);
        lfe_fx_write_sat(&data[i], out, &clipped);
    }

    return clipped ? LFE_WARN_CLIPPED : LFE_OK;
}
