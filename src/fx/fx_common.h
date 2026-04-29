/*
 * fx_common.h — Shared helpers for the Phase 5 FX implementations.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This header is internal to the fx/ subtree: it is *not* part of the
 * public lfe.h API. Each of fx_distortion.c / fx_filter.c / fx_delay.c /
 * fx_env_shaper.c includes it for the common range-validation helper
 * and the dry/wet mix inline.
 *
 * Keeping these as small static inlines lets the effect files share
 * behavior without the overhead of extra function calls in the hot
 * per-sample loops.
 */

#ifndef LFE_FX_COMMON_H
#define LFE_FX_COMMON_H

#include "lfe.h"

#include "util/fixed.h"
#include "util/platform.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Validate a buffer + range pair the way every FX entry point needs.
 * Writes the clamped range length into *out_count on success.
 *
 * Rules:
 *   - buf, buf->data, range must be non-NULL → LFE_ERR_NULL otherwise
 *   - range.end > buf->length → LFE_ERR_BAD_PARAM
 *   - range.end < range.start → LFE_ERR_BAD_PARAM
 *   - range.start == range.end → LFE_OK with *out_count = 0
 *     (caller should early-return without touching anything)
 */
LFE_INLINE lfe_status lfe_fx_validate_range(const lfe_buffer *buf,
                                            const lfe_fx_range *range,
                                            uint32_t *out_count)
{
    if (!buf || !buf->data || !range) return LFE_ERR_NULL;
    if (range->end < range->start)    return LFE_ERR_BAD_PARAM;
    if (range->end > buf->length)     return LFE_ERR_BAD_PARAM;
    *out_count = range->end - range->start;
    return LFE_OK;
}

/*
 * Dry/wet mix of two Q15 samples. mix_q15 = 0 returns dry unchanged,
 * mix_q15 >= Q15_ONE returns wet unchanged, values in between blend
 * linearly. Result is int32 to give the caller room to clip/saturate.
 *
 * Important: the two endpoints are *exact passthroughs*. Naively
 * computing `(wet * Q15_ONE) >> 15` loses a bit of precision because
 * Q15_ONE is 0x7FFF, not 0x8000 — for example wet = 0x0800 would come
 * out as 0x07FF. Effects that rely on preserving low-bit patterns in
 * wet (bitcrush is the obvious one) would silently fail the invariant.
 * The early returns below make "fully wet" and "fully dry" truly
 * lossless.
 *
 * Formula for the blend: out = dry*(Q15_ONE - mix) + wet*mix, then >> 15.
 * Each product fits int32 (Q15*Q15); the sum can reach ~2*Q15_ONE*Q15_ONE
 * which is ~2.1e9, just under INT32_MAX. Safe without int64.
 */
LFE_INLINE int32_t lfe_fx_mix_drywet(q15_t dry, q15_t wet, uint16_t mix_q15)
{
    if (mix_q15 == 0)         return (int32_t)dry;
    if (mix_q15 >= Q15_ONE)   return (int32_t)wet;
    int32_t m   = (int32_t)mix_q15;
    int32_t inv = Q15_ONE - m;
    return (((int32_t)dry * inv) + ((int32_t)wet * m)) >> 15;
}

/*
 * Saturate an int32 sample back into int16 Q15 range and write it to
 * *dst. Also sets *any_clipped true if the value was clamped. The
 * `any_clipped` flag is the plumbing that lets FX entry points return
 * LFE_WARN_CLIPPED.
 */
LFE_INLINE void lfe_fx_write_sat(lfe_sample_t *dst, int32_t v, bool *any_clipped)
{
    if (v >  Q15_ONE)     { v =  Q15_ONE;     *any_clipped = true; }
    if (v < -Q15_ONE - 1) { v = -Q15_ONE - 1; *any_clipped = true; }
    *dst = (lfe_sample_t)v;
}

#endif /* LFE_FX_COMMON_H */
