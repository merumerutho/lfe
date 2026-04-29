/*
 * fx_bitcrush.c — Bit depth reduction + sample rate reduction + dither.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Signal chain per sample:
 *   in → hold (zero-order hold at reduced rate) → dither → quantize → mix → out
 *
 * The sample rate reduction is virtual — the output stays at the
 * buffer's native rate, but samples are held for `rate_div` consecutive
 * positions (staircase / zero-order hold). This creates the classic
 * "crunchy aliasing" of vintage samplers.
 *
 * Dithering uses TPDF (triangular probability density function):
 * sum of two uniform random values, centered at zero, with amplitude
 * of ±1 quantization step. This eliminates quantization distortion
 * harmonics at the cost of a flat noise floor — the standard mastering
 * technique adapted for lo-fi use.
 */

#include "fx_common.h"

#include "util/noise.h"

LFE_HOT
lfe_status lfe_fx_bitcrush(lfe_buffer *buf,
                           const lfe_fx_range *range,
                           const lfe_fx_bitcrush_params *p)
{
    uint32_t count;
    lfe_status rc = lfe_fx_validate_range(buf, range, &count);
    if (rc != LFE_OK) return rc;
    if (!p) return LFE_ERR_NULL;
    if (count == 0) return LFE_OK;

    uint8_t bits = p->bit_depth;
    if (bits < 1)  bits = 1;
    if (bits > 15) bits = 15;

    uint32_t rdiv = p->rate_div;
    if (rdiv < 1)  rdiv = 1;
    if (rdiv > 64) rdiv = 64;

    uint16_t mix = p->mix;
    if (mix > Q15_ONE) mix = Q15_ONE;

    int shift = 15 - (int)bits;
    int32_t half_step = shift > 0 ? (1 << (shift - 1)) : 0;

    lfe_noise_state rng;
    lfe_noise_init(&rng, 0xBEEF1234u);

    lfe_sample_t *data = buf->data + range->start;
    int32_t held = (int32_t)data[0];
    bool clipped = false;

    for (uint32_t i = 0; i < count; i++) {
        int32_t dry = (int32_t)data[i];

        if ((i % rdiv) == 0)
            held = dry;

        int32_t wet = held;

        if (p->dither && shift > 0) {
            /* TPDF: two uniform samples in [0, step), sum - step.
             * Result is triangular in [-step, +step). */
            uint32_t r1 = (uint32_t)lfe_noise_step(&rng) & 0x7FFFu;
            uint32_t r2 = (uint32_t)lfe_noise_step(&rng) & 0x7FFFu;
            int32_t step = 1 << shift;
            int32_t d1 = (int32_t)((r1 * (uint32_t)step) >> 15);
            int32_t d2 = (int32_t)((r2 * (uint32_t)step) >> 15);
            wet += d1 + d2 - step;
        }

        /* Quantize: round to nearest, then truncate low bits. */
        wet += half_step;
        wet >>= shift;
        wet <<= shift;

        if (wet >  Q15_ONE)     wet =  Q15_ONE;
        if (wet < -Q15_ONE - 1) wet = -Q15_ONE - 1;

        int32_t out = lfe_fx_mix_drywet((q15_t)dry, (q15_t)wet, mix);
        lfe_fx_write_sat(&data[i], out, &clipped);
    }

    return clipped ? LFE_WARN_CLIPPED : LFE_OK;
}
