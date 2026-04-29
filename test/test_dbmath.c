/*
 * test_dbmath.c — round-trip + anchor-point tests for lfe_dbmath.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Anchors:
 *    0 dB → 32767 exactly
 *   -6 dB ≈ 16422 (half-amplitude ± 1 LSB)
 *  -12 dB ≈ 8226
 *  -20 dB ≈ 3277
 *  -64 dB → 0
 *   above 0 dB clamps to 32767, below -64 dB clamps to 0.
 *
 * Round-trip q15 → dB → q15 lands within ±1 LSB for values above
 * -60 dB (below that the LUT is so coarse that ±2 LSB shows up).
 */

#include "test_main.h"
#include "lfe_dbmath.h"

#include <stdlib.h>

static int int_abs(int x) { return x < 0 ? -x : x; }

void lfe_test_dbmath(void)
{
    LFE_TEST_HEADER("dbmath: anchor points");

    LFE_TEST_ASSERT_EQ(lfe_db_to_q15(LFE_DB_ZERO),     32767, "0 dB → Q15_ONE");
    LFE_TEST_ASSERT_EQ(lfe_db_to_q15(LFE_DB_MINUS_INF),    0, "-inf → 0");
    LFE_TEST_ASSERT_EQ(lfe_db_to_q15(LFE_DB_FLOOR_Q8_8),   0, "-64 dB → 0");
    LFE_TEST_ASSERT_EQ(lfe_db_to_q15(LFE_DB_Q8_8(+6.0f)), 32767,
                       "+6 dB clamps to Q15_ONE (no boost)");

    /* -6 dB should give roughly half the linear amplitude. */
    int16_t q6  = lfe_db_to_q15(LFE_DB_MINUS_6);
    int16_t q12 = lfe_db_to_q15(LFE_DB_MINUS_12);
    int16_t q24 = lfe_db_to_q15(LFE_DB_MINUS_24);

    /* Exact expected values are Q15_ONE × 10^(dB/20), rounded:
     *    -6  → 0.501187 × 32767 ≈ 16421
     *    -12 → 0.251189 × 32767 ≈  8231
     *    -24 → 0.063096 × 32767 ≈  2068
     * Slack of ±4 absorbs LUT rounding. */
    LFE_TEST_ASSERT(int_abs(q6  - 16421) <= 4, "-6 dB  ≈ 16421 (±4 LSB)");
    LFE_TEST_ASSERT(int_abs(q12 -  8231) <= 4, "-12 dB ≈ 8231  (±4 LSB)");
    LFE_TEST_ASSERT(int_abs(q24 -  2068) <= 4, "-24 dB ≈ 2068  (±4 LSB)");

    /* Monotonicity: as dB decreases, linear must decrease. */
    LFE_TEST_HEADER("dbmath: monotonicity");
    int16_t prev = lfe_db_to_q15(0);
    int reversed = 0;
    for (int db8 = -1; db8 >= LFE_DB_FLOOR_Q8_8; db8 -= 32) {
        int16_t cur = lfe_db_to_q15((int16_t)db8);
        if (cur > prev) { reversed = 1; break; }
        prev = cur;
    }
    LFE_TEST_ASSERT(!reversed, "monotone decrease from 0 to -64 dB");

    /* Round-trip a handful of anchor dB values through to q15 and back. */
    LFE_TEST_HEADER("dbmath: round-trip");
    const int16_t check_db[] = {
        LFE_DB_ZERO, LFE_DB_MINUS_6, LFE_DB_MINUS_12, LFE_DB_MINUS_24,
        LFE_DB_Q8_8(-30.0f), LFE_DB_Q8_8(-48.0f),
    };
    for (size_t i = 0; i < sizeof(check_db) / sizeof(check_db[0]); i++) {
        int16_t q = lfe_db_to_q15(check_db[i]);
        int16_t d = lfe_q15_to_db(q);
        int err = int_abs((int)d - (int)check_db[i]);
        /* ±0.5 dB tolerance (= 128 in Q8.8). */
        char msg[64];
        snprintf(msg, sizeof(msg), "round-trip db=%d within 0.5 dB", check_db[i]);
        LFE_TEST_ASSERT(err <= 128, msg);
    }

    /* ---- Ergonomic builders ---- */
    LFE_TEST_HEADER("dbmath: ergonomic builders");

    lfe_synth_params sp = { 0 };
    lfe_synth_set_levels_db(&sp,
        LFE_DB_ZERO,     /* master = 0 dB  */
        LFE_DB_MINUS_6,  /* osc1   = -6 dB */
        LFE_DB_MINUS_12, /* osc2   = -12 dB */
        LFE_DB_MINUS_24  /* noise  = -24 dB */
    );
    LFE_TEST_ASSERT_EQ(sp.master_level,
                       lfe_db_to_q15(LFE_DB_ZERO),    "synth: master is 0 dB");
    LFE_TEST_ASSERT_EQ(sp.osc1.level,
                       lfe_db_to_q15(LFE_DB_MINUS_6), "synth: osc1 is -6 dB");
    LFE_TEST_ASSERT_EQ(sp.osc2.level,
                       lfe_db_to_q15(LFE_DB_MINUS_12),"synth: osc2 is -12 dB");
    LFE_TEST_ASSERT_EQ(sp.noise_level,
                       lfe_db_to_q15(LFE_DB_MINUS_24),"synth: noise is -24 dB");

    lfe_drum_params dp = { 0 };
    lfe_drum_set_levels_db(&dp, LFE_DB_ZERO, LFE_DB_MINUS_6, LFE_DB_MINUS_12);
    LFE_TEST_ASSERT_EQ(dp.master_level,
                       lfe_db_to_q15(LFE_DB_ZERO),     "drum: master is 0 dB");
    LFE_TEST_ASSERT_EQ(dp.tone_level,
                       lfe_db_to_q15(LFE_DB_MINUS_6),  "drum: tone is -6 dB");
    LFE_TEST_ASSERT_EQ(dp.noise_level,
                       lfe_db_to_q15(LFE_DB_MINUS_12), "drum: noise is -12 dB");

    /* Inline sugar macro. */
    int16_t g = LFE_GAIN_DB(-6.0f);
    LFE_TEST_ASSERT(int_abs(g - 16421) <= 4,
                    "LFE_GAIN_DB(-6.0f) ≈ 16421");
}
