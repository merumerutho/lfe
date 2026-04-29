/*
 * fixed.h — Fixed-point math primitives for the lfe library.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ARM9 has no FPU; software floating point would be 50-200x slower
 * than integer for the same DSP work. Everything in this library is
 * fixed-point. The conventions used throughout:
 *
 *   Q15  — signed 16-bit, range [-1.0, +1.0). Format used for audio
 *          samples and many filter coefficients. 1 sign bit + 15
 *          fraction bits. The literal 0x7FFF represents +1.0 (almost),
 *          0x8000 represents -1.0.
 *
 *   Q31  — signed 32-bit, range [-1.0, +1.0). Used for accumulators
 *          and intermediate results that need more headroom than Q15.
 *
 *   Q24.8 — 32-bit unsigned, range [0, 16777215.996...). Used for
 *           frequencies in Hz with sub-Hz precision. 440 Hz = 440 << 8
 *           = 112640.
 *
 *   Q16.16 — 32-bit signed, general-purpose 16-bit integer + 16-bit
 *            fractional value. Useful for envelope levels and rates.
 *
 *   Phase  — 32-bit unsigned, the full range represents one full
 *            oscillator cycle (0 = start, 0xFFFFFFFF = just before
 *            wrap). Add the per-sample increment to advance.
 *
 * Macros and inline functions in this file should compile to a few
 * machine instructions on both ARM9 and x86. Avoid anything that
 * pulls in libgcc soft-float helpers.
 */

#ifndef LFE_UTIL_FIXED_H
#define LFE_UTIL_FIXED_H

#include <stdint.h>

#include "platform.h"

/* ------------------------------------------------------------------ */
/* Type aliases — purely documentary, the C type system doesn't       */
/* enforce them, but they make function signatures self-explanatory.   */
/* ------------------------------------------------------------------ */

typedef int16_t  q15_t;
typedef int32_t  q31_t;
typedef uint32_t q24_8_t;
typedef int32_t  q16_16_t;
typedef uint32_t lfe_phase_t;

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define Q15_ONE       0x7FFF
#define Q15_NEG_ONE   ((q15_t)0x8000)
#define Q15_ZERO      0
#define Q15_HALF      0x4000

#define Q31_ONE       0x7FFFFFFF
#define Q31_NEG_ONE   ((q31_t)0x80000000)
#define Q31_ZERO      0

/* ------------------------------------------------------------------ */
/* Saturation                                                          */
/* ------------------------------------------------------------------ */

LFE_INLINE q15_t lfe_sat_q15(int32_t v)
{
    if (v >  Q15_ONE)     return  Q15_ONE;
    if (v < -Q15_ONE - 1) return -Q15_ONE - 1;
    return (q15_t)v;
}

LFE_INLINE q31_t lfe_sat_q31(int64_t v)
{
    if (v >  (int64_t)Q31_ONE)        return  Q31_ONE;
    if (v < -(int64_t)Q31_ONE - 1)    return -Q31_ONE - 1;
    return (q31_t)v;
}

/* ------------------------------------------------------------------ */
/* Multiplication                                                      */
/*                                                                     */
/* These compile to one or two ARM9 instructions when the optimizer    */
/* is on (SMULL or SMULBB on the ARM946E-S).                           */
/* ------------------------------------------------------------------ */

/* Multiply two Q15 values, return Q15. Result is rounded toward zero. */
LFE_INLINE q15_t lfe_q15_mul(q15_t a, q15_t b)
{
    return (q15_t)(((int32_t)a * (int32_t)b) >> 15);
}

/* Multiply Q15 by Q15, return Q31 accumulator. */
LFE_INLINE q31_t lfe_q15_mul_q31(q15_t a, q15_t b)
{
    return ((int32_t)a * (int32_t)b) << 1;
}

/* Multiply Q31 by Q31, return Q31. Uses 64-bit intermediate. */
LFE_INLINE q31_t lfe_q31_mul(q31_t a, q31_t b)
{
    return (q31_t)(((int64_t)a * (int64_t)b) >> 31);
}

/* ------------------------------------------------------------------ */
/* Phase / frequency conversions                                       */
/* ------------------------------------------------------------------ */

/*
 * Convert a Q24.8 frequency in Hz to a phase increment.
 *
 * Phase is a 32-bit unsigned representing one full cycle. To advance
 * `freq_hz` cycles per second at `sample_rate` Hz sampling, the
 * per-sample phase increment is:
 *
 *     inc = (freq_hz / sample_rate) * 2^32
 *
 * In Q24.8: freq_hz_q8 / 256 = freq_hz, so the formula becomes:
 *
 *     inc = (freq_hz_q8 << 24) / sample_rate
 *
 * which fits a 64-bit intermediate. The result is the integer phase
 * increment to add per output sample.
 */
LFE_INLINE lfe_phase_t lfe_freq_to_phase_inc(q24_8_t freq_hz_q8,
                                              uint32_t sample_rate)
{
    /* (freq_hz_q8 << 24) is up to 2^32 already; use 64-bit. */
    return (lfe_phase_t)(((uint64_t)freq_hz_q8 << 24) / sample_rate);
}

/* Helper for the common "I have a plain integer Hz" case. */
LFE_INLINE q24_8_t lfe_hz(uint32_t hz)
{
    return (q24_8_t)(hz << 8);
}

/* ------------------------------------------------------------------ */
/* Min / max                                                           */
/* ------------------------------------------------------------------ */

LFE_INLINE int32_t lfe_min_i32(int32_t a, int32_t b) { return a < b ? a : b; }
LFE_INLINE int32_t lfe_max_i32(int32_t a, int32_t b) { return a > b ? a : b; }
LFE_INLINE int32_t lfe_clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

#endif /* LFE_UTIL_FIXED_H */
