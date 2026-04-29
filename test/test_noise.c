/*
 * test_noise.c — Tests for the LFSR white noise generator.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "test_main.h"

#include "util/noise.h"
#include "util/wav.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TEST_RATE 32000u

static void test_noise_determinism(void)
{
    LFE_TEST_HEADER("noise determinism");

    lfe_noise_state a, b;
    lfe_noise_init(&a, 0xDEADBEEF);
    lfe_noise_init(&b, 0xDEADBEEF);

    int identical = 1;
    for (int i = 0; i < 10000; i++) {
        if (lfe_noise_step(&a) != lfe_noise_step(&b)) {
            identical = 0;
            break;
        }
    }
    LFE_TEST_ASSERT(identical, "two LFSRs with same seed produce same sequence");
}

static void test_noise_zero_seed_safe(void)
{
    LFE_TEST_HEADER("noise zero seed safe");

    /* The LFSR has zero as an absorbing state — lfe_noise_init must
     * substitute a non-zero default if the user passes zero. */
    lfe_noise_state n;
    lfe_noise_init(&n, 0);

    /* After init the state must be non-zero. */
    LFE_TEST_ASSERT(n.state != 0, "init substitutes default seed for 0");

    /* Confirm the LFSR actually moves (state changes between steps). */
    uint32_t s1 = n.state;
    (void)lfe_noise_step(&n);
    LFE_TEST_ASSERT(n.state != s1, "LFSR state changes after step");
}

static void test_noise_distribution(void)
{
    LFE_TEST_HEADER("noise distribution");

    /* Over many samples, white noise should average around zero and
     * have a roughly uniform distribution. We check the mean and the
     * fraction of samples in each "octant" of the Q15 range. */
    lfe_noise_state n;
    lfe_noise_init(&n, 0xCAFEBABE);

    const int N = 100000;
    int64_t sum = 0;
    int positive = 0, negative = 0;
    int max_seen = 0, min_seen = 0;
    for (int i = 0; i < N; i++) {
        int16_t v = lfe_noise_step(&n);
        sum += v;
        if (v > 0) positive++;
        if (v < 0) negative++;
        if (v > max_seen) max_seen = v;
        if (v < min_seen) min_seen = v;
    }

    /* Mean should be close to zero. With 100k samples and uniform
     * distribution in [-32768, 32767], the expected std deviation of
     * the mean is roughly 32768/sqrt(N*12) ≈ 30. Allow ±200 slack. */
    int64_t mean = sum / N;
    LFE_TEST_ASSERT(mean > -200 && mean < 200,
                    "mean of 100k samples close to zero");

    /* Roughly 50/50 positive vs negative. */
    int balance = positive - negative;
    LFE_TEST_ASSERT(balance > -2000 && balance < 2000,
                    "positive/negative balance is roughly even");

    /* The LFSR should hit values across most of the Q15 range. */
    LFE_TEST_ASSERT(max_seen >  20000, "noise reaches large positive values");
    LFE_TEST_ASSERT(min_seen < -20000, "noise reaches large negative values");
}

static void test_noise_wav_dump(void)
{
    LFE_TEST_HEADER("noise WAV dump");

    /* 0.5 seconds of white noise at half amplitude (so it doesn't
     * blow out the user's speakers when they listen to the WAV). */
    const uint32_t length = TEST_RATE / 2u;
    int16_t *buf = (int16_t *)calloc(length, sizeof(int16_t));
    LFE_TEST_ASSERT(buf != NULL, "alloc noise wav buf");
    if (!buf) return;

    lfe_noise_state n;
    lfe_noise_init(&n, 0x12345678);

    for (uint32_t i = 0; i < length; i++) {
        /* Half amplitude. */
        buf[i] = (int16_t)(lfe_noise_step(&n) >> 1);
    }

    int rc = lfe_test_wav_write_mono16("test/output/noise_white_500ms.wav",
                                       buf, length, TEST_RATE);
    LFE_TEST_ASSERT_EQ(rc, 0, "noise WAV file written");

    free(buf);
}

void lfe_test_noise(void)
{
    test_noise_determinism();
    test_noise_zero_seed_safe();
    test_noise_distribution();
    test_noise_wav_dump();
}
