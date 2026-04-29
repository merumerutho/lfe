/*
 * lfe_braids_random.h — fast 16-bit LCG pseudo-RNG for the Braids port.
 *
 * Ported from stmlib/utils/random.h (MIT, © 2012 Emilie Gillet).
 *
 * Braids' `Random::GetWord` / `Random::GetSample` in C form. State is
 * a single file-scope u32 defined in lfe_braids_random.c — the C++
 * original used a static class member for the same "global seed"
 * semantics. Callers that want reproducible output must call
 * braids_random_seed() first.
 */

#ifndef LFE_BRAIDS_RANDOM_H
#define LFE_BRAIDS_RANDOM_H

#include <stdint.h>

extern uint32_t braids_random_state;

static inline void braids_random_seed(uint32_t seed)
{
    braids_random_state = seed;
}

static inline uint32_t braids_random_word(void)
{
    braids_random_state = braids_random_state * 1664525u + 1013904223u;
    return braids_random_state;
}

static inline int16_t braids_random_sample(void)
{
    return (int16_t)(braids_random_word() >> 16);
}

#endif /* LFE_BRAIDS_RANDOM_H */
