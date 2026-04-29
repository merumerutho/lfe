/*
 * lfe_braids_dsp.h — fixed-point DSP primitives for the Braids port.
 *
 * Ported from stmlib/utils/dsp.h (MIT, © 2012 Emilie Gillet).
 * See COPYING.braids for the full license text.
 *
 * C has no overloading, so the C++ `Interpolate824` / `Interpolate88` /
 * `Crossfade` overloads are split into explicit type-suffixed variants
 * (`_s16`, `_u16`, `_u8`). Call sites in the ported oscillator code
 * pick the right one based on LUT element type.
 *
 * All functions are inline — no .c companion. This header is internal
 * to lib/lfe/src/gen/braids and should NOT be exposed through lfe.h.
 */

#ifndef LFE_BRAIDS_DSP_H
#define LFE_BRAIDS_DSP_H

#include <stdint.h>

/* ---- 8.24 phase interpolation (table indexed by top 8 bits, fractional
 *      from the next 16). Three element-type variants. ---- */

static inline int16_t braids_interp824_s16(const int16_t *table, uint32_t phase)
{
    int32_t a = table[phase >> 24];
    int32_t b = table[(phase >> 24) + 1];
    return (int16_t)(a + ((b - a) * (int32_t)((phase >> 8) & 0xffff) >> 16));
}

static inline uint16_t braids_interp824_u16(const uint16_t *table, uint32_t phase)
{
    uint32_t a = table[phase >> 24];
    uint32_t b = table[(phase >> 24) + 1];
    return (uint16_t)(a + ((b - a) * (uint32_t)((phase >> 8) & 0xffff) >> 16));
}

/* uint8_t variant widens to a pseudo-int16 range — lift by 8 then
 * subtract 32768 to center. Matches stmlib exactly. */
static inline int16_t braids_interp824_u8(const uint8_t *table, uint32_t phase)
{
    int32_t a = table[phase >> 24];
    int32_t b = table[(phase >> 24) + 1];
    return (int16_t)((a << 8) +
                     ((b - a) * (int32_t)(phase & 0xffffff) >> 16) - 32768);
}

/* ---- 8.8 index interpolation (top byte selects entry, low byte is
 *      fractional). Two variants. ---- */

static inline uint16_t braids_interp88_u16(const uint16_t *table, uint16_t index)
{
    int32_t a = table[index >> 8];
    int32_t b = table[(index >> 8) + 1];
    return (uint16_t)(a + ((b - a) * (int32_t)(index & 0xff) >> 8));
}

static inline int16_t braids_interp88_s16(const int16_t *table, uint16_t index)
{
    int32_t a = table[index >> 8];
    int32_t b = table[(index >> 8) + 1];
    return (int16_t)(a + ((b - a) * (int32_t)(index & 0xff) >> 8));
}

/* ---- 10.22 phase interpolation (1024-entry tables). ---- */
static inline int16_t braids_interp1022(const int16_t *table, uint32_t phase)
{
    int32_t a = table[phase >> 22];
    int32_t b = table[(phase >> 22) + 1];
    return (int16_t)(a + ((b - a) * (int32_t)((phase >> 6) & 0xffff) >> 16));
}

/* ---- 11.5 phase interpolation (2048-entry tables indexed by top 11 bits). ---- */
static inline int16_t braids_interp115(const int16_t *table, uint16_t phase)
{
    int32_t a = table[phase >> 5];
    int32_t b = table[(phase >> 5) + 1];
    return (int16_t)(a + ((b - a) * (int32_t)(phase & 0x1f) >> 5));
}

/* ---- Linear mix — balance is Q16 (0x0000 = a, 0xFFFF ≈ b). ---- */
static inline int16_t braids_mix_s16(int16_t a, int16_t b, uint16_t balance)
{
    return (int16_t)(((int32_t)a * (65535 - balance) +
                      (int32_t)b * balance) >> 16);
}

static inline uint16_t braids_mix_u16(uint16_t a, uint16_t b, uint16_t balance)
{
    return (uint16_t)(((uint32_t)a * (65535u - balance) +
                       (uint32_t)b * balance) >> 16);
}

/* ---- Crossfade two interpolated tables. ---- */

static inline int16_t braids_crossfade_s16(const int16_t *table_a,
                                           const int16_t *table_b,
                                           uint32_t phase, uint16_t balance)
{
    int32_t a = braids_interp824_s16(table_a, phase);
    int32_t b = braids_interp824_s16(table_b, phase);
    return (int16_t)(a + ((b - a) * (int32_t)balance >> 16));
}

static inline int16_t braids_crossfade_u8(const uint8_t *table_a,
                                          const uint8_t *table_b,
                                          uint32_t phase, uint16_t balance)
{
    int32_t a = braids_interp824_u8(table_a, phase);
    int32_t b = braids_interp824_u8(table_b, phase);
    return (int16_t)(a + ((b - a) * (int32_t)balance >> 16));
}

static inline int16_t braids_crossfade_1022_s16(const int16_t *table_a,
                                                const int16_t *table_b,
                                                uint32_t phase, uint16_t balance)
{
    int32_t a = braids_interp1022(table_a, phase);
    int32_t b = braids_interp1022(table_b, phase);
    return (int16_t)(a + ((b - a) * (int32_t)balance >> 16));
}

static inline int16_t braids_crossfade_115_s16(const int16_t *table_a,
                                               const int16_t *table_b,
                                               uint16_t phase, uint16_t balance)
{
    int32_t a = braids_interp115(table_a, phase);
    int32_t b = braids_interp115(table_b, phase);
    return (int16_t)(a + ((b - a) * (int32_t)balance >> 16));
}

/* ---- stmlib.h spillover ---- */

/* s16 clamp used throughout Braids oscillator inner loops. Macro form
 * preserves the original expression shape and avoids an extra branch
 * in the common "already in range" case. */
#define BRAIDS_CLIP(x)           \
    do {                          \
        if ((x) < -32767) (x) = -32767; \
        if ((x) >  32767) (x) =  32767; \
    } while (0)

#define BRAIDS_CONSTRAIN(var, lo, hi) \
    do {                               \
        if ((var) < (lo)) (var) = (lo); \
        else if ((var) > (hi)) (var) = (hi); \
    } while (0)

#endif /* LFE_BRAIDS_DSP_H */
