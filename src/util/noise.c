/*
 * noise.c — White noise initializer.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The hot path (lfe_noise_step) is inlined in noise.h, so this file
 * only contains the init function. It exists as a .c file for symmetry
 * with the other primitives and to give the linker something concrete
 * to pull in.
 */

#include "noise.h"

void lfe_noise_init(lfe_noise_state *n, uint32_t seed)
{
    if (!n) return;
    /* Non-zero default to avoid the LFSR's absorbing state. The
     * specific value doesn't matter — any non-zero seed gives the
     * same maximum-length sequence, just starting at a different
     * point in the cycle. */
    n->state = (seed != 0u) ? seed : 0xACE1B5C3u;
}
