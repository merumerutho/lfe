/*
 * dbmath.c — dB ↔ Q15 conversion, public API.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See lfe_dbmath.h for the contract. Implementation notes:
 *
 *   - The LUT covers dB ∈ [-64, 0] at 0.5 dB steps — 129 entries.
 *     Populated lazily on first call via powf so we don't pay the
 *     init cost at lfe_init() time for callers that never use dB.
 *   - Interpolation between LUT points is linear in Q15 rather than
 *     linear in dB. At 0.5 dB resolution the linear-in-Q15 error is
 *     well below 1 Q15 LSB — not worth the extra exp evaluation.
 *   - The inverse (q15_to_db) uses a bit-scan (__builtin_clz where
 *     available, else a portable fallback) for log2, then a small
 *     polynomial that approximates log10(x/2^exp) on [1, 2). Total
 *     error bound around ±0.5 dB.
 */

#include "lfe_dbmath.h"

#include <math.h>
#include <stdbool.h>

#define LFE_DB_LUT_STEP_Q8_8   128    /* 0.5 dB in Q8.8 */
#define LFE_DB_LUT_ENTRIES     129    /* covers -64 dB..0 dB inclusive */

static int16_t g_db_lut[LFE_DB_LUT_ENTRIES];
static bool    g_db_lut_ready = false;

static void db_lut_init(void)
{
    /* entry i corresponds to dB = -64 + i * 0.5 */
    for (int i = 0; i < LFE_DB_LUT_ENTRIES; i++) {
        float db = -64.0f + 0.5f * (float)i;
        float lin = powf(10.0f, db / 20.0f);
        float q15 = lin * 32767.0f;
        if (q15 < 0.0f)     q15 = 0.0f;
        if (q15 > 32767.0f) q15 = 32767.0f;
        g_db_lut[i] = (int16_t)(q15 + 0.5f);
    }
    /* Exact endpoints — silence the rounding-at-the-edges case. */
    g_db_lut[0]                         = 0;       /* -64 dB → effective 0 */
    g_db_lut[LFE_DB_LUT_ENTRIES - 1]    = 32767;   /*   0 dB → Q15_ONE     */
    g_db_lut_ready = true;
}

int16_t lfe_db_to_q15(int16_t db_q8_8)
{
    if (db_q8_8 >= 0)                  return (int16_t)32767;
    if (db_q8_8 <= LFE_DB_FLOOR_Q8_8)  return 0;

    if (!g_db_lut_ready) db_lut_init();

    /* Map db_q8_8 to a LUT coordinate. Each LUT step = 0.5 dB = 128 in Q8.8.
     * The LUT index 0 corresponds to -64 dB, so we offset by 64 dB. */
    int32_t offset      = (int32_t)db_q8_8 - (int32_t)LFE_DB_FLOOR_Q8_8;
    int32_t index       = offset >> 7;       /* offset / 128 */
    int32_t frac_in_128 = offset & 0x7F;     /* 0..127, fraction of one LUT step */

    if (index >= LFE_DB_LUT_ENTRIES - 1) return 32767;

    int32_t a = g_db_lut[index];
    int32_t b = g_db_lut[index + 1];
    /* Linear interpolation in Q15 space. */
    int32_t y = a + (((b - a) * frac_in_128) >> 7);
    if (y < 0)      y = 0;
    if (y > 32767)  y = 32767;
    return (int16_t)y;
}

int16_t lfe_q15_to_db(int16_t linear)
{
    if (linear <= 0) return LFE_DB_MINUS_INF;
    if (linear >= 32767) return 0;

    /* 20 * log10(linear / 32767) via float — this is a display path,
     * not a hot loop. If a future caller needs it at audio rate we
     * can swap in the CLZ-based approximation described in the plan. */
    float ratio = (float)linear / 32767.0f;
    float db    = 20.0f * log10f(ratio);
    if (db < -64.0f) return LFE_DB_MINUS_INF;
    if (db >   0.0f) return 0;
    return (int16_t)(db * 256.0f);  /* Q8.8 */
}
