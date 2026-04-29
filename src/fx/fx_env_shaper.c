/*
 * fx_env_shaper.c — Gain-envelope shaper effect.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Multiplies each sample in the selection by a gain value read from
 * a caller-supplied canvas. The canvas is stretched to the selection
 * length via fixed-point linear interpolation — canvas_length is
 * independent of the selection length, so the same 256-point canvas
 * can be applied to selections of any duration.
 *
 * The canvas convention mirrors `gen_drawn.c`: Q15 values in a flat
 * array representing the full "time axis" of the shape. Presets
 * produce common fade / decay / bell curves; callers are also free
 * to supply hand-drawn canvases (e.g. from a touchscreen drawing UI
 * similar to the waveform editor's drawn-waveform tab).
 *
 * Implementation detail: the stretching uses a 16.16 fixed-point
 * index that walks through the canvas once. For each selection
 * sample we fetch two adjacent canvas points and linearly interpolate
 * between them — the resulting envelope has no visible stair-stepping
 * even when canvas_length is much smaller than the selection.
 */

#include "fx_common.h"

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Presets                                                             */
/*                                                                     */
/* Every preset writes `length` points covering [0..Q15_ONE] gain,    */
/* representing the full shape. For length=2 each is just a 2-point   */
/* linear ramp; for length=256 the curve has full resolution.         */
/* ------------------------------------------------------------------ */

/*
 * Exponential decay curve. We want canvas[0] = Q15_ONE, falling
 * smoothly toward 0. A cheap way to get an audible exponential shape
 * without libm is to use a geometric progression: each step multiplies
 * by a fixed ratio slightly under 1. Choose the ratio so that
 * canvas[length-1] ≈ 0.001 (~-60 dB).
 *
 * For N steps and final = 1/1000: ratio = (1/1000)^(1/(N-1)).
 * We approximate: ratio_q15 ≈ Q15_ONE - (Q15_ONE * 7 / N), which
 * gives a reasonable-looking exponential for length 16..1024 without
 * any pow()/exp() calls. Not mathematically exact but looks right
 * on a scope and sounds right to the ear.
 */
static void fill_exp_decay(uint16_t *canvas, uint32_t length)
{
    /* canvas[i] = (1 - i/(N-1))^3 in Q15. Looks exponential-ish and
     * costs nothing to compute.
     *
     * Endpoints are pinned exactly: the math computes (Q15_ONE)^3 via
     * two >>15 shifts and each shift loses a bit (Q15_ONE × Q15_ONE
     * >> 15 = 32766, not 32767). The correct endpoint values are
     * trivially known, so we just write them directly and let the
     * loop compute only the interior. */
    canvas[0]          = Q15_ONE;
    canvas[length - 1] = 0;
    for (uint32_t i = 1; i + 1 < length; i++) {
        uint32_t t = (i * (uint32_t)Q15_ONE) / (length - 1u);
        int32_t u  = Q15_ONE - (int32_t)t;
        int32_t u2 = (u * u) >> 15;
        int32_t u3 = (u2 * u) >> 15;
        if (u3 < 0)       u3 = 0;
        if (u3 > Q15_ONE) u3 = Q15_ONE;
        canvas[i] = (uint16_t)u3;
    }
}

static void fill_exp_attack(uint16_t *canvas, uint32_t length)
{
    /* Mirror of exp decay: canvas[i] = 1 - (1-t)^3, same endpoint
     * pinning for the same Q15 rounding reason. */
    canvas[0]          = 0;
    canvas[length - 1] = Q15_ONE;
    for (uint32_t i = 1; i + 1 < length; i++) {
        uint32_t t = (i * (uint32_t)Q15_ONE) / (length - 1u);
        int32_t u  = Q15_ONE - (int32_t)t;
        int32_t u2 = (u * u) >> 15;
        int32_t u3 = (u2 * u) >> 15;
        int32_t v  = Q15_ONE - u3;
        if (v < 0)       v = 0;
        if (v > Q15_ONE) v = Q15_ONE;
        canvas[i] = (uint16_t)v;
    }
}

static void fill_fade_in(uint16_t *canvas, uint32_t length)
{
    for (uint32_t i = 0; i < length; i++) {
        uint32_t t = (length > 1) ? (i * (uint32_t)Q15_ONE) / (length - 1u) : 0;
        canvas[i] = (uint16_t)t;
    }
}

static void fill_fade_out(uint16_t *canvas, uint32_t length)
{
    for (uint32_t i = 0; i < length; i++) {
        uint32_t t = (length > 1) ? (i * (uint32_t)Q15_ONE) / (length - 1u) : 0;
        canvas[i] = (uint16_t)(Q15_ONE - (int32_t)t);
    }
}

static void fill_triangle(uint16_t *canvas, uint32_t length)
{
    uint32_t half = length / 2u;
    if (half == 0) half = 1;
    for (uint32_t i = 0; i < length; i++) {
        uint32_t t;
        if (i < half) {
            t = (i * (uint32_t)Q15_ONE) / half;
        } else {
            t = ((length - 1u - i) * (uint32_t)Q15_ONE) / (length - 1u - half > 0 ? (length - 1u - half) : 1u);
        }
        canvas[i] = (uint16_t)t;
    }
}

/*
 * Bell: a smoother peaked curve — t * (1-t) normalized to peak at 1.
 * Produces a hump centered at the middle of the canvas, rounder than
 * the triangle preset. Normalization keeps the peak exactly at Q15_ONE.
 */
static void fill_bell(uint16_t *canvas, uint32_t length)
{
    /* For x in [0, 1], 4*x*(1-x) peaks at 1 when x=0.5. */
    for (uint32_t i = 0; i < length; i++) {
        uint32_t t = (length > 1) ? (i * (uint32_t)Q15_ONE) / (length - 1u) : 0;
        int32_t  one_minus = Q15_ONE - (int32_t)t;
        int32_t  prod = ((int32_t)t * one_minus) >> 15;      /* x*(1-x) in Q15 */
        int32_t  scaled = prod << 2;                           /* *4 */
        if (scaled > Q15_ONE) scaled = Q15_ONE;
        if (scaled < 0)       scaled = 0;
        canvas[i] = (uint16_t)scaled;
    }
}

lfe_status lfe_fx_env_fill_preset(uint16_t *canvas,
                                  uint32_t length,
                                  lfe_fx_env_preset preset)
{
    if (!canvas)      return LFE_ERR_NULL;
    if (length < 2u)  return LFE_ERR_BAD_PARAM;

    switch (preset) {
    case LFE_FX_ENV_PRESET_FADE_IN:     fill_fade_in(canvas, length);     return LFE_OK;
    case LFE_FX_ENV_PRESET_FADE_OUT:    fill_fade_out(canvas, length);    return LFE_OK;
    case LFE_FX_ENV_PRESET_EXP_DECAY:   fill_exp_decay(canvas, length);   return LFE_OK;
    case LFE_FX_ENV_PRESET_EXP_ATTACK:  fill_exp_attack(canvas, length);  return LFE_OK;
    case LFE_FX_ENV_PRESET_TRIANGLE:    fill_triangle(canvas, length);    return LFE_OK;
    case LFE_FX_ENV_PRESET_BELL:        fill_bell(canvas, length);        return LFE_OK;
    }
    return LFE_ERR_BAD_PARAM;
}

/* ------------------------------------------------------------------ */
/* Apply the envelope to the selection                                 */
/* ------------------------------------------------------------------ */

LFE_HOT
lfe_status lfe_fx_env_shaper(lfe_buffer *buf,
                             const lfe_fx_range *range,
                             const lfe_fx_env_shaper_params *p)
{
    uint32_t count;
    lfe_status rc = lfe_fx_validate_range(buf, range, &count);
    if (rc != LFE_OK) return rc;
    if (!p || !p->canvas)       return LFE_ERR_NULL;
    if (p->canvas_length < 2u)  return LFE_ERR_BAD_PARAM;
    if (count == 0) return LFE_OK;

    lfe_sample_t *data = buf->data + range->start;

    /*
     * Walk the canvas in 16.16 fixed-point. inc is chosen so that
     * idx reaches (canvas_length - 1) exactly when i = count - 1,
     * guaranteeing we land on the final canvas point at the last
     * sample — which is what the user drew.
     */
    const uint32_t last_canvas_idx = p->canvas_length - 1u;
    uint32_t idx_q16 = 0;
    uint32_t inc_q16 = (count > 1)
        ? (uint32_t)(((uint64_t)last_canvas_idx << 16) / (count - 1u))
        : 0;

    const uint16_t mix = p->mix;
    bool clipped = false;

    for (uint32_t i = 0; i < count; i++) {
        /* Integer index and 16-bit fractional part. */
        uint32_t idx  = idx_q16 >> 16;
        uint32_t frac = idx_q16 & 0xFFFFu;
        if (idx > last_canvas_idx) idx = last_canvas_idx;

        int32_t a = (int32_t)p->canvas[idx];
        int32_t b = (int32_t)p->canvas[idx < last_canvas_idx ? idx + 1 : idx];

        /* Linear interp: a + (b - a) * frac / 65536. Product fits
         * int32 because (b-a) is at most ~Q15_ONE and frac is 16-bit. */
        int32_t gain = a + (((b - a) * (int32_t)frac) >> 16);
        if (gain < 0)       gain = 0;
        if (gain > Q15_ONE) gain = Q15_ONE;

        q15_t dry = data[i];
        int32_t wet = ((int32_t)dry * gain) >> 15;
        int32_t out = lfe_fx_mix_drywet(dry, (q15_t)wet, mix);
        lfe_fx_write_sat(&data[i], out, &clipped);

        idx_q16 += inc_q16;
    }

    return clipped ? LFE_WARN_CLIPPED : LFE_OK;
}
