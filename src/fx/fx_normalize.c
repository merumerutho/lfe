/*
 * fx_normalize.c — DC-remove and peak-normalize the selection.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Two-pass operation:
 *
 *   Pass 1 (accumulate + subtract): sum every sample into an int64
 *          accumulator to get the DC mean. Subtract the mean from
 *          each sample, write back in place, track the peak absolute
 *          magnitude of the DC-removed output.
 *
 *   Pass 2 (scale):                 if peak > 0, compute gain =
 *          target_peak * Q15_ONE / peak (as int64 to avoid overflow
 *          when peak is small) and multiply every sample by gain,
 *          then saturate.
 *
 * If the selection is silent or pure DC (peak_after == 0) we stop
 * after pass 1 — the samples are already 0 and applying any gain is
 * undefined.
 *
 * Edge cases:
 *   - range.start == range.end: LFE_OK, no work
 *   - target_peak == 0:          treated as Q15_ONE (full-scale) — a
 *                                 user-friendly default; explicit mute
 *                                 belongs in a different effect.
 *   - peak_after == 0:           skip scaling, return LFE_OK
 *   - rounding-induced saturation during scaling: return LFE_WARN_CLIPPED
 */

#include "fx_common.h"

#include <stdbool.h>
#include <stdint.h>

lfe_status lfe_fx_normalize(lfe_buffer *buf,
                            const lfe_fx_range *range,
                            const lfe_fx_normalize_params *p)
{
    if (!p) return LFE_ERR_NULL;

    uint32_t count;
    lfe_status rc = lfe_fx_validate_range(buf, range, &count);
    if (rc != LFE_OK) return rc;
    if (count == 0)   return LFE_OK;

    uint16_t target = p->target_peak == 0 ? (uint16_t)Q15_ONE : p->target_peak;

    lfe_sample_t *data = buf->data + range->start;

    /* ---- Pass 1: DC mean ----
     * int64 accumulator covers any plausible selection length at s16
     * range (count * 32768 fits easily unless count > 2^48 — not a
     * real scenario on NDS or host). */
    int64_t sum = 0;
    for (uint32_t i = 0; i < count; i++)
        sum += data[i];

    /* Rounded average: add half-count with sign-correct bias so the
     * divide truncates toward nearest rather than toward zero. */
    int32_t dc;
    if (sum >= 0) dc = (int32_t)((sum + (int64_t)(count / 2)) / count);
    else          dc = (int32_t)((sum - (int64_t)(count / 2)) / count);

    /* ---- Pass 1b: subtract DC + find peak ---- */
    int32_t peak = 0;
    for (uint32_t i = 0; i < count; i++) {
        int32_t v = (int32_t)data[i] - dc;
        /* DC subtraction can push a Q15_ONE sample past ±Q15 range by
         * at most 1 LSB in practice (dc is bounded by the same range),
         * so a hard saturation keeps the stored value well-formed. */
        if (v >  Q15_ONE)     v =  Q15_ONE;
        if (v < -Q15_ONE - 1) v = -Q15_ONE - 1;
        data[i] = (lfe_sample_t)v;

        int32_t mag = v < 0 ? -v : v;
        if (mag > peak) peak = mag;
    }

    /* Silent or pure-DC selection — nothing left to scale. */
    if (peak == 0) return LFE_OK;

    /* ---- Pass 2: apply gain ----
     *
     * gain (Q15) = target_peak / peak  ← logically in Q0 ratio-land
     *            → multiply each sample s by gain, interpret as Q15 so
     *              `(s * gain) >> 15` lands in the right range.
     *
     * Do the divide in int64 so we can carry full precision when
     * peak is small (e.g. a quiet selection → large gain).
     *
     * Cap gain to avoid runaway multiplication when peak is
     * pathologically tiny. The cap is arbitrary but conservative:
     * 64 × full-scale is +36 dB of boost, well beyond any useful
     * normalization.
     */
    int64_t gain_q15 = ((int64_t)target * (int64_t)Q15_ONE) / (int64_t)peak;
    if (gain_q15 > (int64_t)Q15_ONE * 64) gain_q15 = (int64_t)Q15_ONE * 64;

    bool any_clipped = false;
    for (uint32_t i = 0; i < count; i++) {
        int64_t scaled = (int64_t)data[i] * gain_q15;
        int32_t out    = (int32_t)(scaled >> 15);
        lfe_fx_write_sat(&data[i], out, &any_clipped);
    }

    return any_clipped ? LFE_WARN_CLIPPED : LFE_OK;
}
