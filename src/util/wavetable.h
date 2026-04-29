/*
 * wavetable.h — Internal wavetable interface for the lfe library.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Wavetables are pre-computed at lfe_init() time. The lookup function
 * takes a 32-bit phase accumulator and returns a Q15 sample value with
 * 16-bit linear interpolation between table entries.
 *
 * 512 entries × 2 bytes = 1 KB per table. The tables live in main RAM
 * (BSS) on the NDS build; on the host they live wherever the OS puts
 * BSS. The lookup function itself is small enough to inline cleanly.
 *
 * This is an internal header. The public lfe.h does not expose any
 * of these symbols.
 */

#ifndef LFE_UTIL_WAVETABLE_H
#define LFE_UTIL_WAVETABLE_H

#include <stdint.h>

#include "fixed.h"
#include "platform.h"

/* ------------------------------------------------------------------ */
/* Table layout                                                        */
/* ------------------------------------------------------------------ */

#define LFE_WT_BITS  9
#define LFE_WT_SIZE  (1u << LFE_WT_BITS)   /* 512 entries */
#define LFE_WT_MASK  (LFE_WT_SIZE - 1u)

/* Sine table: one full period, indexed by phase top bits.
 * Values are Q15: -32767..+32767. */
extern q15_t lfe_wt_sine[LFE_WT_SIZE];

/* ------------------------------------------------------------------ */
/* Initialization                                                      */
/* ------------------------------------------------------------------ */

/* Populate all wavetables. Idempotent — calling twice is harmless. */
void lfe_wavetable_init(void);

/* ------------------------------------------------------------------ */
/* Lookup with linear interpolation                                    */
/*                                                                     */
/* phase is the full Q32 phase accumulator. The top LFE_WT_BITS bits   */
/* index the table; the next 16 bits give the interpolation fraction. */
/*                                                                     */
/* Inlined here so the inner loops in generators don't pay a function  */
/* call cost per sample.                                                */
/* ------------------------------------------------------------------ */

LFE_INLINE q15_t lfe_wt_sine_lookup(lfe_phase_t phase)
{
    uint32_t idx  = phase >> (32 - LFE_WT_BITS);          /* 10-bit index */
    uint32_t frac = (phase >> (32 - LFE_WT_BITS - 16))    /* 16-bit fraction */
                  & 0xFFFFu;

    int32_t a = (int32_t)lfe_wt_sine[idx];
    int32_t b = (int32_t)lfe_wt_sine[(idx + 1u) & LFE_WT_MASK];

    /* (b - a) is at most 65534 in magnitude; multiplying by frac
     * (max 65535) overflows int32, so use int64 for the product. */
    int64_t delta = (int64_t)(b - a) * (int64_t)frac;
    return (q15_t)(a + (int32_t)(delta >> 16));
}

#endif /* LFE_UTIL_WAVETABLE_H */
