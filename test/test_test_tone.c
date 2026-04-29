/*
 * test_test_tone.c — Tests for lfe_gen_test_tone (Phase 0).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Validates the Phase 0 generator end-to-end:
 *
 *   1. Determinism: same params → byte-identical output buffer.
 *   2. Length: the function writes exactly the requested number of
 *      samples and nothing past the end of the buffer.
 *   3. Amplitude: the output is non-zero and stays within Q15 range.
 *   4. Period: the zero-crossings of a 440 Hz tone at 32 kHz happen
 *      at the expected sample positions (within rounding tolerance).
 *   5. WAV dump: writes a real WAV file to test/output/ for ear-testing.
 *      Not asserted; the existence of the file is the proof.
 *
 * The tests live in this single file; if Phase 0 grows we can split
 * by concern, but at this size flat is fine.
 */

#include "test_main.h"

#include "lfe.h"
#include "util/wav.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* All phase-0 tests use the same buffer geometry. */
#define TEST_RATE   32000u
#define TEST_LENGTH 32000u   /* 1 second */
#define TEST_FREQ   440u
#define TEST_AMP    0x7000   /* about -1 dB */

static void test_test_tone_basic(void)
{
    LFE_TEST_HEADER("test_tone basic");

    int16_t *buf = (int16_t *)calloc(TEST_LENGTH, sizeof(int16_t));
    LFE_TEST_ASSERT(buf != NULL, "alloc buf");
    if (!buf) return;

    lfe_buffer out = {
        .data   = buf,
        .length = TEST_LENGTH,
        .rate   = LFE_RATE_32000,
    };
    lfe_test_tone_params p = {
        .freq_hz_q8    = TEST_FREQ << 8,
        .amplitude_q15 = TEST_AMP,
    };

    lfe_status rc = lfe_gen_test_tone(&out, &p);
    LFE_TEST_ASSERT_EQ(rc, LFE_OK, "lfe_gen_test_tone returns OK");

    /* Output must be non-zero (the function actually wrote something). */
    int nonzero = 0;
    int max_abs = 0;
    for (uint32_t i = 0; i < TEST_LENGTH; i++) {
        int v = buf[i];
        if (v != 0) nonzero++;
        int a = v < 0 ? -v : v;
        if (a > max_abs) max_abs = a;
    }
    LFE_TEST_ASSERT(nonzero > 0, "buffer contains nonzero samples");
    LFE_TEST_ASSERT(max_abs <= 32767, "max amplitude within Q15 range");
    /* The peak should be close to the requested amplitude. Allow some
     * slack for table interpolation rounding. */
    LFE_TEST_ASSERT(max_abs >= (TEST_AMP * 3) / 4,
                    "peak amplitude near requested level");

    free(buf);
}

static void test_test_tone_determinism(void)
{
    LFE_TEST_HEADER("test_tone determinism");

    int16_t *a = (int16_t *)calloc(TEST_LENGTH, sizeof(int16_t));
    int16_t *b = (int16_t *)calloc(TEST_LENGTH, sizeof(int16_t));
    LFE_TEST_ASSERT(a && b, "alloc both buffers");
    if (!a || !b) { free(a); free(b); return; }

    lfe_buffer out_a = { .data = a, .length = TEST_LENGTH, .rate = LFE_RATE_32000 };
    lfe_buffer out_b = { .data = b, .length = TEST_LENGTH, .rate = LFE_RATE_32000 };
    lfe_test_tone_params p = {
        .freq_hz_q8    = TEST_FREQ << 8,
        .amplitude_q15 = TEST_AMP,
    };

    lfe_gen_test_tone(&out_a, &p);
    lfe_gen_test_tone(&out_b, &p);

    int identical = (memcmp(a, b, TEST_LENGTH * sizeof(int16_t)) == 0);
    LFE_TEST_ASSERT(identical, "two runs with same params produce identical output");

    free(a);
    free(b);
}

static void test_test_tone_buffer_bounds(void)
{
    LFE_TEST_HEADER("test_tone bounds");

    /* Allocate one extra sample as a sentinel and check the function
     * doesn't write past the requested length. */
    const uint32_t n = 1000;
    int16_t *buf = (int16_t *)calloc(n + 1, sizeof(int16_t));
    LFE_TEST_ASSERT(buf != NULL, "alloc bounds buf");
    if (!buf) return;
    buf[n] = (int16_t)0x5A5A; /* sentinel */

    lfe_buffer out = { .data = buf, .length = n, .rate = LFE_RATE_32000 };
    lfe_test_tone_params p = {
        .freq_hz_q8    = TEST_FREQ << 8,
        .amplitude_q15 = TEST_AMP,
    };

    lfe_gen_test_tone(&out, &p);
    LFE_TEST_ASSERT_EQ(buf[n], (int16_t)0x5A5A,
                       "sentinel past end of buffer is untouched");

    free(buf);
}

static void test_test_tone_zero_crossings(void)
{
    LFE_TEST_HEADER("test_tone zero crossings");

    /* For a 440 Hz tone at 32000 Hz sample rate, one cycle is
     * 32000 / 440 ≈ 72.73 samples. We expect ~14 zero-crossings per
     * 1000 samples (440 Hz × 2 crossings/cycle × 1000/32000 sec). */
    const uint32_t n = 32000;
    int16_t *buf = (int16_t *)calloc(n, sizeof(int16_t));
    LFE_TEST_ASSERT(buf != NULL, "alloc zc buf");
    if (!buf) return;

    lfe_buffer out = { .data = buf, .length = n, .rate = LFE_RATE_32000 };
    lfe_test_tone_params p = {
        .freq_hz_q8    = 440u << 8,
        .amplitude_q15 = 0x7FFF,
    };
    lfe_gen_test_tone(&out, &p);

    int crossings = 0;
    for (uint32_t i = 1; i < n; i++) {
        if ((buf[i - 1] >= 0) != (buf[i] >= 0))
            crossings++;
    }
    /* Expected: 440 Hz × 2 crossings/cycle × 1 second = 880.
     * Allow ±10 for interpolation/rounding. */
    LFE_TEST_ASSERT(crossings >= 870 && crossings <= 890,
                    "440 Hz tone has ~880 zero crossings in 1 second");

    free(buf);
}

static void test_test_tone_wav_dump(void)
{
    LFE_TEST_HEADER("test_tone WAV dump");

    int16_t *buf = (int16_t *)calloc(TEST_LENGTH, sizeof(int16_t));
    LFE_TEST_ASSERT(buf != NULL, "alloc dump buf");
    if (!buf) return;

    lfe_buffer out = { .data = buf, .length = TEST_LENGTH, .rate = LFE_RATE_32000 };
    lfe_test_tone_params p = {
        .freq_hz_q8    = 440u << 8,
        .amplitude_q15 = TEST_AMP,
    };
    lfe_gen_test_tone(&out, &p);

    int rc = lfe_test_wav_write_mono16("test/output/test_tone_440hz_1s.wav",
                                       buf, TEST_LENGTH, TEST_RATE);
    LFE_TEST_ASSERT_EQ(rc, 0, "WAV file written");

    free(buf);
}

static void test_test_tone_param_validation(void)
{
    LFE_TEST_HEADER("test_tone param validation");

    int16_t buf[100];
    lfe_buffer out = { .data = buf, .length = 100, .rate = LFE_RATE_32000 };
    lfe_test_tone_params p = {
        .freq_hz_q8    = 440u << 8,
        .amplitude_q15 = 0x4000,
    };

    /* NULL output buffer */
    LFE_TEST_ASSERT_EQ(lfe_gen_test_tone(NULL, &p), LFE_ERR_NULL,
                       "NULL out → LFE_ERR_NULL");
    /* NULL params */
    LFE_TEST_ASSERT_EQ(lfe_gen_test_tone(&out, NULL), LFE_ERR_NULL,
                       "NULL params → LFE_ERR_NULL");

    /* Frequency above Nyquist (32000 / 2 = 16000 Hz limit) */
    lfe_test_tone_params bad = { .freq_hz_q8 = 17000u << 8, .amplitude_q15 = 0x4000 };
    LFE_TEST_ASSERT_EQ(lfe_gen_test_tone(&out, &bad), LFE_ERR_BAD_PARAM,
                       "above-Nyquist → LFE_ERR_BAD_PARAM");
}

void lfe_test_test_tone(void)
{
    test_test_tone_basic();
    test_test_tone_determinism();
    test_test_tone_buffer_bounds();
    test_test_tone_zero_crossings();
    test_test_tone_wav_dump();
    test_test_tone_param_validation();
}
