/*
 * fx_ott.c — 3-band "OTT-style" multiband compressor.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Signal chain per sample:
 *
 *   in → *in_gain → LR4_low (88 Hz)  → (LOW,    above_low)
 *                   above_low → LR4_high (2500 Hz) → (MID, HIGH)
 *   each band → |follower| → gain_lut → *band_gain
 *   low + mid + high → *out_gain → saturate → out
 *
 * The per-band gain LUT (256 entries) is rebuilt on every call from the
 * thresholds, fixed ratios, and the (depth * up/down knob) amount. This
 * keeps the per-sample loop in pure integer with two table lookups per
 * band instead of a powf.
 *
 * Phase 1: downward compression + per-band makeup gain + in/out gain.
 * Depth and upward compression land in Phase 2 (hooks in the LUT build
 * are already parameterized to make that a one-site change).
 */

#include "fx_common.h"

#include "util/biquad.h"
#include "util/crossover.h"
#include "util/env_follower.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Algorithm constants                                                 */
/* ------------------------------------------------------------------ */

/* Classic OTT split freqs (approx). Fixed — surface is kept deliberately
 * small. If we later want tuneable crossovers, they go on the params
 * struct; the LR4 primitive doesn't care. */
#define OTT_LOW_XOVER_HZ    88u
#define OTT_HIGH_XOVER_HZ   2500u

/* Downward compression threshold — signal above this is compressed with
 * ratio 3:1. -24 dBFS matches the shipping Xfer default closely. */
#define OTT_THRESH_DOWN_DB  (-24.0f)
#define OTT_DOWN_RATIO      (3.0f)   /* 3:1 downward */

/* Upward compression threshold — signal below this is expanded toward
 * it with ratio 1:3. Unused in Phase 1 but the LUT builder already
 * honors it, so flipping upward on in Phase 2 is a knob-wiring change. */
#define OTT_THRESH_UP_DB    (-40.0f)
#define OTT_UP_RATIO        (3.0f)   /* 1:3 upward (boost below thresh) */

/* Attack / release time ranges (ms) driven by the `time` knob. */
#define OTT_ATK_MS_FAST     1u
#define OTT_ATK_MS_SLOW     10u
#define OTT_REL_MS_FAST     50u
#define OTT_REL_MS_SLOW     500u

/* LUT resolution — 256 entries keyed on the top 8 bits of the Q15
 * envelope. More than enough for downward compression (signal is loud
 * when above threshold, so top bits have good precision there). */
#define OTT_LUT_SIZE 256

/* Gain LUT entries are Q15. Entries can exceed Q15_ONE when upward
 * compression is active (boost below threshold). uint32 lets us cap
 * the boost at ~+48 dB (≈250× ≈ 8.2 M in Q15 units) without clipping
 * in the LUT itself — the sample-×-gain product uses int64 so the
 * downstream math handles the wider range cleanly. */
typedef uint32_t ott_gain_q15;

/* +48 dB boost ceiling — enough for OTT's "over the top" upward
 * character on quiet content without going absurd. */
#define OTT_GAIN_LUT_MAX  8388607u    /* ≈ 32767 * 256 ≈ +48 dB */

/* ------------------------------------------------------------------ */
/* Per-band gain LUT builder                                            */
/* ------------------------------------------------------------------ */

/*
 * Populate a 256-entry LUT of gain_q15 keyed on the top 8 bits of the
 * envelope (env_q15 >> 7). Uses float math — called once per apply()
 * per band, not in the hot loop.
 *
 *   down_scale in [0, 1]   — scales downward gain reduction (dB)
 *   up_scale   in [0, 1]   — scales upward gain boost (dB)
 *
 * down_scale = depth * down_band_q15 (both 0..1); same shape for upward.
 */
static void ott_build_gain_lut(ott_gain_q15 *lut,
                               float down_scale,
                               float up_scale)
{
    const float thresh_down_lin = powf(10.0f, OTT_THRESH_DOWN_DB / 20.0f);
    const float thresh_up_lin   = powf(10.0f, OTT_THRESH_UP_DB   / 20.0f);
    const float down_slope      = (OTT_DOWN_RATIO - 1.0f) / OTT_DOWN_RATIO;  /* 2/3 */
    const float up_slope        = (OTT_UP_RATIO   - 1.0f);                    /* 2.0 */

    for (int i = 0; i < OTT_LUT_SIZE; i++) {
        /* Midpoint of the bucket so rounding errors don't bias one side. */
        float env = ((float)i + 0.5f) * (128.0f / 32768.0f);
        if (env < 1e-6f) env = 1e-6f;   /* avoid log(0) */

        float env_db = 20.0f * log10f(env);
        float gain_db = 0.0f;

        if (env_db > OTT_THRESH_DOWN_DB) {
            float excess = env_db - OTT_THRESH_DOWN_DB;
            gain_db -= excess * down_slope * down_scale;
        }
        if (env < thresh_up_lin) {
            /* Upward boost — active when up_scale > 0 (Phase 2 wiring). */
            float deficit = OTT_THRESH_UP_DB - env_db;
            gain_db += deficit * up_slope * up_scale;
        }

        /* Convert back to linear Q15 with headroom for boost. */
        float gain_lin = powf(10.0f, gain_db / 20.0f);
        float gain_q   = gain_lin * 32767.0f;

        if (gain_q < 0.0f)                       gain_q = 0.0f;
        if (gain_q > (float)OTT_GAIN_LUT_MAX)    gain_q = (float)OTT_GAIN_LUT_MAX;
        lut[i] = (ott_gain_q15)(gain_q + 0.5f);
    }
}

/* Apply a gain LUT to one band sample. Multiplies sample * lut[env] >> 15.
 * Uses int64 intermediate: gain up to ~+48 dB (8.4M) × band up to Q15_ONE
 * (32767) → ~2.7e11, well beyond int32 range. */
static inline int32_t ott_apply_band_gain(q15_t band_sample,
                                          q15_t env_q15,
                                          const ott_gain_q15 *lut)
{
    int idx = (int)env_q15 >> 7;
    if (idx < 0)                 idx = 0;
    if (idx >= OTT_LUT_SIZE)     idx = OTT_LUT_SIZE - 1;
    int64_t g = (int64_t)lut[idx];
    return (int32_t)(((int64_t)band_sample * g) >> 15);
}

/* ------------------------------------------------------------------ */
/* Time knob → attack/release ms                                        */
/* ------------------------------------------------------------------ */

/* Map the Q15 `time` knob into attack_ms and release_ms in the
 * configured fast/slow range. Linear interpolation in ms — fine for
 * the musical feel (log-in-time mapping is a Phase 2+ refinement). */
static void ott_time_to_ms(uint16_t time_q15,
                           uint32_t *atk_ms,
                           uint32_t *rel_ms)
{
    uint32_t t = time_q15 > Q15_ONE ? Q15_ONE : time_q15;
    *atk_ms = OTT_ATK_MS_FAST +
              ((OTT_ATK_MS_SLOW - OTT_ATK_MS_FAST) * t) / Q15_ONE;
    *rel_ms = OTT_REL_MS_FAST +
              ((OTT_REL_MS_SLOW - OTT_REL_MS_FAST) * t) / Q15_ONE;
}

/* ------------------------------------------------------------------ */
/* Main entry point                                                     */
/* ------------------------------------------------------------------ */

LFE_HOT
lfe_status lfe_fx_ott(lfe_buffer *buf,
                      const lfe_fx_range *range,
                      const lfe_fx_ott_params *p)
{
    uint32_t count;
    lfe_status rc = lfe_fx_validate_range(buf, range, &count);
    if (rc != LFE_OK) return rc;
    if (!p) return LFE_ERR_NULL;
    if (count == 0) return LFE_OK;

    const uint32_t sr = (uint32_t)buf->rate;
    if (sr == 0) return LFE_ERR_BAD_PARAM;

    /* -------- setup -------- */

    lfe_lr4_state xo_low;
    lfe_lr4_state xo_high;
    lfe_lr4_init(&xo_low,  OTT_LOW_XOVER_HZ,  sr);
    lfe_lr4_init(&xo_high, OTT_HIGH_XOVER_HZ, sr);

    uint32_t atk_ms, rel_ms;
    ott_time_to_ms(p->time, &atk_ms, &rel_ms);

    lfe_env_follower_state ef_low, ef_mid, ef_high;
    lfe_env_follower_init(&ef_low,  atk_ms, rel_ms, sr);
    lfe_env_follower_init(&ef_mid,  atk_ms, rel_ms, sr);
    lfe_env_follower_init(&ef_high, atk_ms, rel_ms, sr);

    /* Depth scales both upward and downward contributions uniformly.
     * Clamp to [0, 1]. At depth=0 the compressor is effectively bypassed
     * regardless of the per-band up/down settings. */
    float depth_norm = (float)p->depth / 32767.0f;
    if (depth_norm < 0.0f) depth_norm = 0.0f;
    if (depth_norm > 1.0f) depth_norm = 1.0f;

    ott_gain_q15 lut_low [OTT_LUT_SIZE];
    ott_gain_q15 lut_mid [OTT_LUT_SIZE];
    ott_gain_q15 lut_high[OTT_LUT_SIZE];

    ott_build_gain_lut(lut_low,
                       depth_norm * ((float)p->down_low  / 32767.0f),
                       depth_norm * ((float)p->up_low    / 32767.0f));
    ott_build_gain_lut(lut_mid,
                       depth_norm * ((float)p->down_mid  / 32767.0f),
                       depth_norm * ((float)p->up_mid    / 32767.0f));
    ott_build_gain_lut(lut_high,
                       depth_norm * ((float)p->down_high / 32767.0f),
                       depth_norm * ((float)p->up_high   / 32767.0f));

    /* Makeup + in/out gains (Q15) — cached as int32 for the loop. */
    const int32_t in_gain   = (int32_t)p->in_gain;
    const int32_t out_gain  = (int32_t)p->out_gain;
    const int32_t gain_low  = (int32_t)p->gain_low;
    const int32_t gain_mid  = (int32_t)p->gain_mid;
    const int32_t gain_high = (int32_t)p->gain_high;

    /* -------- per-sample loop -------- */

    lfe_sample_t *data = buf->data + range->start;
    bool clipped = false;

    for (uint32_t i = 0; i < count; i++) {
        /* Input trim. */
        int32_t x = ((int32_t)data[i] * in_gain) >> 15;
        if (x >  Q15_ONE)     x =  Q15_ONE;
        if (x < -Q15_ONE - 1) x = -Q15_ONE - 1;

        /* Serial LR4 split. */
        q15_t low, above_low;
        lfe_lr4_step(&xo_low, (q15_t)x, &low, &above_low);

        q15_t mid, high;
        lfe_lr4_step(&xo_high, above_low, &mid, &high);

        /* Envelope detection per band. */
        q15_t env_lo = lfe_env_follower_step(&ef_low,  low);
        q15_t env_md = lfe_env_follower_step(&ef_mid,  mid);
        q15_t env_hi = lfe_env_follower_step(&ef_high, high);

        /* Compress each band (Q15 sample × LUT gain). Values can exceed
         * Q15 range after upward boost, so we stay in int64 from here. */
        int64_t c_lo = ott_apply_band_gain(low,  env_lo, lut_low);
        int64_t c_md = ott_apply_band_gain(mid,  env_md, lut_mid);
        int64_t c_hi = ott_apply_band_gain(high, env_hi, lut_high);

        c_lo = (c_lo * gain_low)  >> 15;
        c_md = (c_md * gain_mid)  >> 15;
        c_hi = (c_hi * gain_high) >> 15;

        int64_t mixed = c_lo + c_md + c_hi;
        mixed = (mixed * out_gain) >> 15;

        /* Final saturation to Q15. */
        int32_t mixed32;
        if      (mixed >  Q15_ONE)     { mixed32 =  Q15_ONE;     clipped = true; }
        else if (mixed < -Q15_ONE - 1) { mixed32 = -Q15_ONE - 1; clipped = true; }
        else                             mixed32 = (int32_t)mixed;
        data[i] = (lfe_sample_t)mixed32;
    }

    return clipped ? LFE_WARN_CLIPPED : LFE_OK;
}
