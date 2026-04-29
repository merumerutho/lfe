/*
 * test_lfo.c — Tests for the internal LFO primitive.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Validates that each LFO shape stays within the Q15 signed range,
 * that a full cycle visits both signs, and that phase advances at the
 * expected rate (a 1 Hz LFO at 32 kHz sample rate produces one full
 * cycle over 32000 samples).
 */

#include "test_main.h"

#include "util/lfo.h"
#include "util/fixed.h"
#include "util/wavetable.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LFO_RATE 32000u

static void test_lfo_shape_bounds(lfe_lfo_shape shape, const char *label)
{
    lfe_lfo_state st;
    lfe_lfo_init(&st, shape, 2u << 8, LFO_RATE);  /* 2 Hz */

    int32_t min = 0x7FFFFFFF, max = -0x7FFFFFFF;
    bool seen_pos = false, seen_neg = false;
    for (uint32_t i = 0; i < LFO_RATE; i++) {
        q15_t v = lfe_lfo_step(&st);
        if (v > max) max = v;
        if (v < min) min = v;
        if (v > 0)   seen_pos = true;
        if (v < 0)   seen_neg = true;
    }

    LFE_TEST_ASSERT(max <=  Q15_ONE,     label);
    LFE_TEST_ASSERT(min >= -Q15_ONE - 1, label);
    LFE_TEST_ASSERT(seen_pos && seen_neg, label);
}

static void test_lfo_shapes(void)
{
    LFE_TEST_HEADER("lfo shape bounds + polarity");

    test_lfo_shape_bounds(LFE_LFO_SHAPE_SINE,     "sine within Q15 + two-sided");
    test_lfo_shape_bounds(LFE_LFO_SHAPE_TRIANGLE, "triangle within Q15 + two-sided");
    test_lfo_shape_bounds(LFE_LFO_SHAPE_SQUARE,   "square within Q15 + two-sided");
    test_lfo_shape_bounds(LFE_LFO_SHAPE_SAW_UP,   "saw_up within Q15 + two-sided");
    test_lfo_shape_bounds(LFE_LFO_SHAPE_SAW_DOWN, "saw_down within Q15 + two-sided");
}

static void test_lfo_rate(void)
{
    LFE_TEST_HEADER("lfo rate matches sample-rate-per-cycle");

    /* Sine at 1 Hz sampled at 32 kHz: we should get one full cycle in
     * ~32000 samples. Detect the cycle by counting sign changes from
     * negative to positive — exactly 1 per cycle. */
    lfe_lfo_state st;
    lfe_lfo_init(&st, LFE_LFO_SHAPE_SINE, 1u << 8, LFO_RATE);

    /* Loop a bit past one full cycle so the upward crossing at the cycle
     * boundary is actually observed. Integer-truncated phase_inc makes
     * the ideal 1-cycle span slightly longer than LFO_RATE samples — at
     * exactly LFO_RATE iterations we land a handful of samples short of
     * the wrap and count zero crossings. A few hundred extra samples is
     * cheap and unambiguous. */
    int zero_crossings_up = 0;
    q15_t prev = lfe_lfo_step(&st);
    for (uint32_t i = 1; i < LFO_RATE + 256; i++) {
        q15_t v = lfe_lfo_step(&st);
        if (prev < 0 && v >= 0) zero_crossings_up++;
        prev = v;
    }

    /* Allow 1..2 upward zero-crossings depending on phase alignment at
     * start (init phase = 0 means sine starts at sine(0) = 0 → the
     * first crossing is at ~sample 0, then again at ~sample 32000). */
    LFE_TEST_ASSERT(zero_crossings_up >= 1 && zero_crossings_up <= 2,
                    "sine LFO @ 1 Hz produces 1-2 upward zero crossings per second");
}

static void test_lfo_zero_rate(void)
{
    LFE_TEST_HEADER("lfo rate=0 stays static");

    /* rate_hz_q8 = 0 → phase_inc = 0 → phase never advances. Sine at
     * phase 0 is 0. */
    lfe_lfo_state st;
    lfe_lfo_init(&st, LFE_LFO_SHAPE_SINE, 0, LFO_RATE);
    for (int i = 0; i < 1000; i++) {
        q15_t v = lfe_lfo_step(&st);
        LFE_TEST_ASSERT(v == 0, "sine @ rate=0 stays at 0");
        if (v != 0) break;
    }
}

static void test_lfo_determinism(void)
{
    LFE_TEST_HEADER("lfo determinism");

    lfe_lfo_state a, b;
    lfe_lfo_init(&a, LFE_LFO_SHAPE_TRIANGLE, 3u << 8, LFO_RATE);
    lfe_lfo_init(&b, LFE_LFO_SHAPE_TRIANGLE, 3u << 8, LFO_RATE);

    for (int i = 0; i < 10000; i++) {
        q15_t va = lfe_lfo_step(&a);
        q15_t vb = lfe_lfo_step(&b);
        LFE_TEST_ASSERT_EQ(va, vb, "same init → same stream");
        if (va != vb) break;
    }
}

void lfe_test_lfo(void)
{
    test_lfo_shapes();
    test_lfo_rate();
    test_lfo_zero_rate();
    test_lfo_determinism();
}
