/*
 * noise.h — White noise generator (internal).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * 32-bit Galois LFSR producing pseudo-random Q15 samples. Period is
 * 2^32 - 1, far longer than any sample we'll ever generate. Each step
 * is a few cycles on ARM9 — basically free.
 *
 * Pink noise (1/f spectrum) and other colored noises will be added in
 * a follow-up if generators need them. White is the foundation.
 *
 * Internal header — not exposed via lfe.h.
 */

#ifndef LFE_UTIL_NOISE_H
#define LFE_UTIL_NOISE_H

#include <stdint.h>

#include "fixed.h"

typedef struct {
    uint32_t state;
} lfe_noise_state;

/*
 * Initialize the LFSR with a seed. The seed must be non-zero — zero is
 * the absorbing state of the LFSR (it would produce all zeros forever).
 * If you pass 0, the function substitutes a default non-zero seed so
 * the noise generator always produces useful output.
 */
void lfe_noise_init(lfe_noise_state *n, uint32_t seed);

/*
 * Step the LFSR and return the next white noise sample as Q15.
 * Inline for performance — generators call this in a tight inner loop.
 */
static inline q15_t lfe_noise_step(lfe_noise_state *n)
{
    /* Galois LFSR with the maximal-length tap mask 0xD0000001
     * (taps at positions 32, 31, 29, 1 — a known maximal-length
     * configuration for a 32-bit Galois LFSR). */
    uint32_t s = n->state;
    uint32_t lsb = s & 1u;
    s >>= 1;
    if (lsb) s ^= 0xD0000001u;
    n->state = s;

    /* Take the low 16 bits as a signed Q15 value. The cast through
     * int16_t handles the sign extension correctly. */
    return (q15_t)(int16_t)(s & 0xFFFFu);
}

#endif /* LFE_UTIL_NOISE_H */
