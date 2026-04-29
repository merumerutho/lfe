/*
 * lfe_dbmath.h — dB ↔ Q15 conversion helpers for UI-side knob mapping.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The lfe library's DSP loops stay in linear Q15 (single-cycle multiplies
 * on ARM946E-S). dB perceptual scaling is a UI concern — a linear volume
 * knob puts the entire musically useful range (0 dB down to ~-30 dB)
 * into the top fifth of travel. This header provides the conversion
 * applied once per knob edit, not per sample.
 *
 * Storage convention:
 *   - dB values are signed Q8.8 (int16_t). Range: -128.0 dB .. +127.996 dB.
 *     Typical UI range: `LFE_DB_MINUS_INF` .. 0. Positive values clamp to
 *     Q15_ONE (the library does not provide >0 dB boost).
 *   - Linear gains are Q15 (int16_t) in [0, Q15_ONE].
 *
 * Table details:
 *   - LUT covers dB ∈ [-64, 0] at 0.5 dB steps → 129 entries
 *     (-64.0 dB ≈ 0.00063 × full-scale ≈ 20 Q15 LSB, well below audibility)
 *   - Values below -64 dB return 0 (effective mute)
 *   - Values at or above 0 dB return Q15_ONE
 *   - Fractional dB steps between LUT points are linearly interpolated
 *     in Q15 (close enough to the true exponential curve at 0.5 dB res
 *     that the step is inaudible).
 *
 * The table is populated lazily on the first call via `powf()`. One
 * ~120-byte static array + a single init flag. On the NDS this adds a
 * single ~0.5 ms init when the user first touches a dB-scaled control.
 */

#ifndef LFE_DBMATH_H
#define LFE_DBMATH_H

#include "lfe.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Q8.8 dB constants. Useful for default-value initializers. */
#define LFE_DB_Q8_8(x)      ((int16_t)((x) * 256.0f))
#define LFE_DB_ZERO         LFE_DB_Q8_8( 0.0f)
#define LFE_DB_MINUS_6      LFE_DB_Q8_8(-6.0f)
#define LFE_DB_MINUS_12     LFE_DB_Q8_8(-12.0f)
#define LFE_DB_MINUS_24     LFE_DB_Q8_8(-24.0f)
#define LFE_DB_MINUS_INF    ((int16_t)-32768)   /* floor; maps to 0 */

/* LUT boundary. Anything strictly below this floors to 0. */
#define LFE_DB_FLOOR_Q8_8   LFE_DB_Q8_8(-64.0f)

/*
 * Convert a Q8.8 dB value to a Q15 linear gain.
 *
 *   db_q8_8 >=    0 * 256  →  Q15_ONE (no >0 dB boost in this library)
 *   db_q8_8 <  -64 * 256  →  0
 *   in between               →  LUT with linear interpolation
 *
 * Safe to call from interrupt context after the first call has warmed
 * the table (the init uses powf, which is not re-entrant on all libcs).
 */
int16_t lfe_db_to_q15(int16_t db_q8_8);

/*
 * Convert a Q15 linear gain back to Q8.8 dB for display. Returns
 * LFE_DB_MINUS_INF if `linear` is 0. Not a hot-path primitive — uses
 * a log2 approximation via CLZ + a small polynomial refinement.
 *
 * Round-trip `db_to_q15 → q15_to_db` lands within ±0.5 dB for inputs
 * in [LFE_DB_FLOOR_Q8_8, 0].
 */
int16_t lfe_q15_to_db(int16_t linear);

/*
 * Convenience sugar for call sites building params structs in dB.
 * Example:
 *
 *   lfe_synth_params p = { 0 };
 *   p.master_level = LFE_GAIN_DB(-6.0f);     // not a compile-time constant
 *
 * Prefer these at the UI / preset authoring layer; library-internal
 * presets stay in raw Q15 literals so they don't depend on the LUT.
 */
#define LFE_GAIN_DB(x_float_db)  (lfe_db_to_q15(LFE_DB_Q8_8(x_float_db)))

/* ------------------------------------------------------------------ */
/* Ergonomic builders                                                  */
/* ------------------------------------------------------------------ */

/*
 * Set all four level fields of a synth params struct in one call.
 * Pass Q8.8 dB values (or LFE_DB_Q8_8(float) literals). Fields that
 * aren't gain-class (filter cutoff, envelope times, …) must still be
 * set by the caller — this helper only touches the gain-class fields.
 *
 * Declared `static inline` in the header so callers don't need to link
 * against an extra .o, and the compiler can fold the four LUT calls
 * when the arguments are compile-time known constants.
 */
static inline void lfe_synth_set_levels_db(
    lfe_synth_params *p,
    int16_t master_db_q8_8,
    int16_t osc1_db_q8_8,
    int16_t osc2_db_q8_8,
    int16_t noise_db_q8_8)
{
    p->master_level    = (uint16_t)lfe_db_to_q15(master_db_q8_8);
    p->osc1.level      = (uint16_t)lfe_db_to_q15(osc1_db_q8_8);
    p->osc2.level      = (uint16_t)lfe_db_to_q15(osc2_db_q8_8);
    p->noise_level     = (uint16_t)lfe_db_to_q15(noise_db_q8_8);
}

/*
 * Same contract for drum params: master + tone + noise.
 */
static inline void lfe_drum_set_levels_db(
    lfe_drum_params *p,
    int16_t master_db_q8_8,
    int16_t tone_db_q8_8,
    int16_t noise_db_q8_8)
{
    p->master_level = (uint16_t)lfe_db_to_q15(master_db_q8_8);
    p->tone_level   = (uint16_t)lfe_db_to_q15(tone_db_q8_8);
    p->noise_level  = (uint16_t)lfe_db_to_q15(noise_db_q8_8);
}

#ifdef __cplusplus
}
#endif

#endif /* LFE_DBMATH_H */
