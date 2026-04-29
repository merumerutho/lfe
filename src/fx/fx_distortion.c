/*
 * fx_distortion.c — Nonlinear waveshaping effects.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Four flavors of distortion / saturation, all operating on a sample
 * selection in-place:
 *
 *   HARD     — clip at ±threshold (classic solid-state distortion sound)
 *   SOFT     — rational soft-clip, tanh-like, using a cheap Pade form
 *   FOLD     — wavefolder: reflect past threshold instead of clipping,
 *              creates bright harmonics popular in West Coast synthesis
 *   BITCRUSH — requantize to N bits by dropping the low (15-N) bits
 *
 * All four are stateless per-sample transformations — the cleanest
 * possible exercise of the "process a selection in place" API.
 */

#include "fx_common.h"

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Per-mode sample transforms                                          */
/* ------------------------------------------------------------------ */

/* Hard clip at ±threshold. Above threshold, the waveform flat-tops. */
LFE_INLINE q15_t fx_hard_clip(int32_t in, int32_t threshold)
{
    if (in >  threshold) return (q15_t)threshold;
    if (in < -threshold) return (q15_t)(-threshold);
    return (q15_t)in;
}

/*
 * Soft-clip via a rational approximation of tanh. For small x the
 * output is nearly linear; as |x| grows it asymptotes toward
 * ±Q15_ONE. Produces smoother distortion than hard-clipping with no
 * audible knee.
 *
 * Formula (Q15 throughout):
 *   y = x * (Q15_ONE + (k * x² >> 15)) / (Q15_ONE + (k * x² >> 15) * 2)
 *
 * where k tunes the aggressiveness. We pick k = Q15_HALF for a mild
 * curve that's still clearly audible as saturation.
 *
 * Simpler equivalent used here: y = x / (1 + |x|) style (a rational
 * "sigmoid" approximation) — cheaper than the Pade form and sounds
 * nearly identical at the bit depth we care about.
 */
LFE_INLINE q15_t fx_soft_clip(int32_t in)
{
    int32_t absx = in < 0 ? -in : in;
    /* Denominator in Q15: 1 + |x| / Q15_ONE, but |x| can exceed Q15_ONE
     * when drive is high. Do the divide in Q30 precision. */
    int32_t denom = Q15_ONE + absx;    /* ~1..2 in Q15 units */
    int32_t y     = ((int64_t)in * Q15_ONE) / denom;
    if (y >  Q15_ONE)     y =  Q15_ONE;
    if (y < -Q15_ONE - 1) y = -Q15_ONE - 1;
    return (q15_t)y;
}

/*
 * Wavefolder. If the sample is past ±threshold, it reflects: e.g. at
 * threshold=0x4000, an input of 0x5000 becomes 0x3000 (reflected about
 * threshold), an input of 0x8000 would be 0x0000 (reflected twice).
 *
 * Iteratively fold until inside the band — bounded by a small loop
 * count so we can't spin forever on pathological input.
 */
LFE_INLINE q15_t fx_fold(int32_t in, int32_t threshold)
{
    if (threshold <= 0) return 0;
    for (int iter = 0; iter < 8; iter++) {
        if (in >  threshold) { in =  2 * threshold - in; continue; }
        if (in < -threshold) { in = -2 * threshold - in; continue; }
        break;
    }
    /* Cap in case we bailed out of the loop without converging. */
    if (in >  Q15_ONE)     in =  Q15_ONE;
    if (in < -Q15_ONE - 1) in = -Q15_ONE - 1;
    return (q15_t)in;
}

/*
 * Bitcrush: quantize to `bits` bits by masking off the low 15-bits of
 * significand. bits=15 is a no-op; bits=1 is a square-wave caricature
 * of the input.
 */
LFE_INLINE q15_t fx_bitcrush(q15_t in, int bits)
{
    if (bits >= 15) return in;
    if (bits <= 0)  return 0;
    int shift = 15 - bits;
    int32_t mask = ~((1 << shift) - 1);
    return (q15_t)((int32_t)in & mask);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

LFE_HOT
lfe_status lfe_fx_distort(lfe_buffer *buf,
                          const lfe_fx_range *range,
                          const lfe_fx_distortion_params *p)
{
    uint32_t count;
    lfe_status rc = lfe_fx_validate_range(buf, range, &count);
    if (rc != LFE_OK) return rc;
    if (!p) return LFE_ERR_NULL;
    if (count == 0) return LFE_OK;

    /* Validate mode + bit depth up front so the hot loop doesn't
     * branch on an illegal config. */
    switch (p->mode) {
    case LFE_FX_DIST_HARD:
    case LFE_FX_DIST_SOFT:
    case LFE_FX_DIST_FOLD:
        break;
    case LFE_FX_DIST_BITCRUSH:
        if (p->bit_depth < 1 || p->bit_depth > 15) return LFE_ERR_BAD_PARAM;
        break;
    default:
        return LFE_ERR_BAD_PARAM;
    }

    const int32_t drive     = p->drive == 0 ? Q15_ONE : (int32_t)p->drive;
    const int32_t threshold = p->threshold == 0 ? Q15_ONE : (int32_t)p->threshold;
    const uint16_t mix      = p->mix;
    const int bits          = (int)p->bit_depth;

    lfe_sample_t *data = buf->data + range->start;
    bool clipped = false;

    for (uint32_t i = 0; i < count; i++) {
        q15_t dry = data[i];

        /* Apply input gain (drive). Q15 * Q15 → Q15, may exceed range. */
        int32_t boosted = ((int32_t)dry * drive) >> 15;

        q15_t wet;
        switch (p->mode) {
        case LFE_FX_DIST_HARD:
            wet = fx_hard_clip(boosted, threshold);
            if (boosted > threshold || boosted < -threshold) clipped = true;
            break;
        case LFE_FX_DIST_SOFT:
            wet = fx_soft_clip(boosted);
            break;
        case LFE_FX_DIST_FOLD:
            wet = fx_fold(boosted, threshold);
            break;
        case LFE_FX_DIST_BITCRUSH:
            /* Bitcrush uses raw sample (no drive needed typically but
             * we apply it anyway for consistency). */
            wet = fx_bitcrush((q15_t)(boosted > Q15_ONE ? Q15_ONE :
                                      (boosted < -Q15_ONE - 1 ? -Q15_ONE - 1 : boosted)),
                              bits);
            break;
        default:
            wet = dry;
            break;
        }

        int32_t out = lfe_fx_mix_drywet(dry, wet, mix);
        lfe_fx_write_sat(&data[i], out, &clipped);
    }

    return clipped ? LFE_WARN_CLIPPED : LFE_OK;
}
