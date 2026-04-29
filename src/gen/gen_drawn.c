/*
 * gen_drawn.c — Drawn-waveform generator (Phase 2).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Phase 2 takes the free-drawing functionality that used to live
 * inside maxtracker's sample_view.c and lifts it into the lfe library.
 * The library now owns the algorithm — preset waveform generation and
 * canvas-to-PCM conversion — while the maxtracker UI is responsible
 * only for stylus input and visual rendering of the canvas.
 *
 * Canvas convention:
 *   - signed 8-bit (-128..+127)
 *   - one full cycle of the waveform
 *   - typically 256 points but the API takes a length parameter
 *
 * Output convention:
 *   - one int16 per canvas point
 *   - scaled by canvas[i] << 8 (so -128 → -32768, +127 → +32512)
 */

#include "lfe.h"

#include "util/noise.h"
#include "util/platform.h"

#include <stdint.h>

/* Forward declaration of the internal init check from lfe.c. */
extern bool lfe_is_initialized(void);

/* ------------------------------------------------------------------ */
/* Sine lookup for the SINE preset                                     */
/* ------------------------------------------------------------------ */

/*
 * 64 entries covering one quarter period of sine (0 to PI/2). Values
 * are in [0, 127], representing sin(i * (PI/2) / 64) * 127.
 *
 * The full 256-entry one-cycle sine is built by reflection: the second
 * quarter mirrors the first (sin(PI/2 + x) = sin(PI/2 - x)), and the
 * second half negates the first (sin(PI + x) = -sin(x)).
 *
 * Same table that lived in maxtracker's sample_view.c — moved verbatim
 * so the byte-exact behavior is preserved across the refactor.
 */
static const int8_t sin_lut[64] = {
      0,   3,   6,   9,  12,  16,  19,  22,
     25,  28,  31,  34,  37,  40,  43,  46,
     49,  51,  54,  57,  60,  63,  65,  68,
     71,  73,  76,  78,  81,  83,  85,  88,
     90,  92,  94,  96,  98, 100, 102, 104,
    106, 107, 109, 111, 112, 113, 115, 116,
    117, 118, 120, 121, 122, 122, 123, 124,
    124, 125, 125, 126, 126, 126, 127, 127,
};

/*
 * Compute one full sine cycle value for a 0..255 phase index. The
 * input is masked to 8 bits so callers don't have to bounds-check.
 * Output is signed int8 in [-128..+127].
 */
static int8_t sine_canvas_value(int i)
{
    i &= 255;
    int quarter = i & 63;
    int half    = i & 128;
    int flip    = i & 64;

    int8_t val;
    if (flip)
        val = sin_lut[63 - quarter];
    else
        val = sin_lut[quarter];

    return half ? (int8_t)(-val - 1) : val;
}

/* ------------------------------------------------------------------ */
/* Preset generation                                                   */
/* ------------------------------------------------------------------ */

LFE_HOT
lfe_status lfe_drawn_fill_preset(int8_t *canvas, uint32_t length,
                                 lfe_drawn_preset preset)
{
    if (!canvas)    return LFE_ERR_NULL;
    if (length == 0) return LFE_ERR_BAD_PARAM;

    switch (preset) {
    case LFE_DRAWN_PRESET_SINE:
        /* Map each canvas index to a phase in [0, 256) and look up the
         * sine table. For length == 256 this is a 1:1 mapping; for
         * other lengths the phase steps non-uniformly but still covers
         * one full cycle. */
        for (uint32_t i = 0; i < length; i++) {
            uint32_t phase = (i * 256u) / length;
            canvas[i] = sine_canvas_value((int)phase);
        }
        return LFE_OK;

    case LFE_DRAWN_PRESET_SAW:
        /* Linear ramp from -128 to nearly +127 over the full length. */
        for (uint32_t i = 0; i < length; i++) {
            int v = (int)((i * 256u) / length) - 128;
            if (v >  127) v =  127;
            if (v < -128) v = -128;
            canvas[i] = (int8_t)v;
        }
        return LFE_OK;

    case LFE_DRAWN_PRESET_SQUARE:
        /* First half at -128, second half at +127. */
        for (uint32_t i = 0; i < length; i++) {
            canvas[i] = (i < length / 2u) ? (int8_t)-128 : (int8_t)127;
        }
        return LFE_OK;

    case LFE_DRAWN_PRESET_TRIANGLE: {
        /* Triangle: -127 at i=0, +127 at i=length/2, -127 at i=length-1.
         * The peak isn't quite full Q15 because of integer-division
         * rounding; close enough for a starter waveform. */
        uint32_t half = length / 2u;
        if (half == 0) half = 1;
        for (uint32_t i = 0; i < length; i++) {
            int v;
            if (i < half) {
                v = ((int)(i * 254u) / (int)half) - 127;
            } else {
                v = 127 - ((int)((i - half) * 254u) / (int)half);
            }
            if (v >  127) v =  127;
            if (v < -128) v = -128;
            canvas[i] = (int8_t)v;
        }
        return LFE_OK;
    }

    case LFE_DRAWN_PRESET_NOISE: {
        /* Deterministic white noise via the lfe LFSR. The seed matches
         * the legacy maxtracker code (0xACE1) so a fresh canvas at the
         * default length matches the old behavior byte-for-byte. */
        lfe_noise_state n;
        lfe_noise_init(&n, 0xACE1u);
        for (uint32_t i = 0; i < length; i++) {
            int16_t s = lfe_noise_step(&n);
            canvas[i] = (int8_t)(s >> 8);  /* take high byte */
        }
        return LFE_OK;
    }
    }

    return LFE_ERR_BAD_PARAM;
}

/* ------------------------------------------------------------------ */
/* Canvas → int16 sample                                                */
/* ------------------------------------------------------------------ */

LFE_HOT
lfe_status lfe_gen_drawn(lfe_buffer *out, const lfe_drawn_params *p)
{
    if (!out || !out->data || !p || !p->canvas)
        return LFE_ERR_NULL;
    if (p->canvas_length == 0)
        return LFE_ERR_BAD_PARAM;
    if (out->length != p->canvas_length)
        return LFE_ERR_BUF_TOO_SMALL;
    if (!lfe_is_initialized())
        return LFE_ERR_NOT_INIT;

    /* Each canvas byte (-128..+127) becomes one int16 sample by a
     * left shift of 8. The negative endpoint maps cleanly to -32768
     * (the most negative Q15 value); the positive endpoint maps to
     * +32512, slightly under +32767 — that's accepted to keep the
     * conversion symmetric and free of overflow. */
    const int8_t *src = p->canvas;
    lfe_sample_t *dst = out->data;
    const uint32_t n  = out->length;

    for (uint32_t i = 0; i < n; i++) {
        dst[i] = (lfe_sample_t)((int32_t)src[i] << 8);
    }

    return LFE_OK;
}
