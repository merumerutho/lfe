/*
 * test_filter.c — Tests for the state-variable filter primitive.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The SVF tests are mostly behavioral: feed in known signals (sine,
 * step, white noise) and verify the output makes sense for the chosen
 * filter mode and cutoff. We don't try to verify the precise frequency
 * response (that would need a real DFT and golden coefficient tables).
 */

#include "test_main.h"

#include "util/filter.h"
#include "util/noise.h"
#include "util/wavetable.h"
#include "util/fixed.h"
#include "util/wav.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_RATE 32000u

/*
 * Render N samples of a sine at the given frequency through the
 * given filter, write into out, return the peak absolute amplitude
 * seen in the output. Used to compare filter response at different
 * frequencies.
 */
static int peak_through_filter(lfe_filter_state *flt,
                               uint32_t freq_hz,
                               uint32_t n,
                               int16_t *out)
{
    lfe_phase_t phase = 0;
    lfe_phase_t inc   = lfe_freq_to_phase_inc((q24_8_t)(freq_hz << 8),
                                               TEST_RATE);

    int peak = 0;
    /* Skip the first chunk to let the filter settle. */
    const uint32_t settle = 256;
    for (uint32_t i = 0; i < n + settle; i++) {
        q15_t s = lfe_wt_sine_lookup(phase);
        q15_t y = lfe_filter_step(flt, s);
        phase += inc;
        if (i < settle) continue;
        int v = y < 0 ? -y : y;
        if (v > peak) peak = v;
        if (out) out[i - settle] = y;
    }
    return peak;
}

static void test_filter_lowpass_attenuates_highs(void)
{
    LFE_TEST_HEADER("filter LP attenuates highs");

    lfe_wavetable_init();

    lfe_filter_state flt;
    lfe_filter_init(&flt, LFE_FILTER_LP);
    lfe_filter_set_cutoff(&flt, 1000u, TEST_RATE);
    lfe_filter_set_q(&flt, Q15_ONE);  /* no resonance */

    int peak_lo = peak_through_filter(&flt, 200u,  4096, NULL);

    lfe_filter_reset(&flt);
    int peak_hi = peak_through_filter(&flt, 5000u, 4096, NULL);

    printf("    LP cutoff=1kHz Q15_ONE: 200Hz peak=%d, 5kHz peak=%d (ratio %d:1)\n",
           peak_lo, peak_hi,
           peak_hi > 0 ? peak_lo / peak_hi : peak_lo);

    /* At 1 kHz cutoff, a 200 Hz tone should pass through with little
     * attenuation, while a 5 kHz tone should be substantially smaller. */
    LFE_TEST_ASSERT(peak_lo > 20000,
                    "200 Hz tone passes through 1 kHz LP near full level");
    LFE_TEST_ASSERT(peak_hi < peak_lo / 2,
                    "5 kHz tone is attenuated by 1 kHz LP");
}

static void test_filter_highpass_attenuates_lows(void)
{
    LFE_TEST_HEADER("filter HP attenuates lows");

    lfe_filter_state flt;
    lfe_filter_init(&flt, LFE_FILTER_HP);
    lfe_filter_set_cutoff(&flt, 1000u, TEST_RATE);
    lfe_filter_set_q(&flt, Q15_ONE);

    int peak_lo = peak_through_filter(&flt, 200u,  4096, NULL);

    lfe_filter_reset(&flt);
    int peak_hi = peak_through_filter(&flt, 5000u, 4096, NULL);

    printf("    HP cutoff=1kHz Q15_ONE: 200Hz peak=%d, 5kHz peak=%d (ratio %d:1)\n",
           peak_lo, peak_hi,
           peak_lo > 0 ? peak_hi / peak_lo : peak_hi);

    LFE_TEST_ASSERT(peak_hi > 20000,
                    "5 kHz tone passes through 1 kHz HP near full level");
    LFE_TEST_ASSERT(peak_lo < peak_hi / 2,
                    "200 Hz tone is attenuated by 1 kHz HP");
}

static void test_filter_determinism(void)
{
    LFE_TEST_HEADER("filter determinism");

    /* Two filters fed identical input from independent sources should
     * produce byte-identical output. */
    lfe_filter_state a, b;
    lfe_filter_init(&a, LFE_FILTER_LP);
    lfe_filter_init(&b, LFE_FILTER_LP);
    lfe_filter_set_cutoff(&a, 800u, TEST_RATE);
    lfe_filter_set_cutoff(&b, 800u, TEST_RATE);
    lfe_filter_set_q(&a, 0x4000);
    lfe_filter_set_q(&b, 0x4000);

    lfe_noise_state na, nb;
    lfe_noise_init(&na, 0xDEADBEEF);
    lfe_noise_init(&nb, 0xDEADBEEF);

    int diff = 0;
    for (int i = 0; i < 10000; i++) {
        q15_t va = lfe_filter_step(&a, lfe_noise_step(&na));
        q15_t vb = lfe_filter_step(&b, lfe_noise_step(&nb));
        if (va != vb) { diff++; break; }
    }
    LFE_TEST_ASSERT_EQ(diff, 0, "two LP filters with same input agree byte-for-byte");
}

static void test_filter_stability(void)
{
    LFE_TEST_HEADER("filter stability");

    /* Push white noise through a moderately resonant LP and verify
     * the output stays within Q15 range — no NaN-equivalent runaway. */
    lfe_filter_state flt;
    lfe_filter_init(&flt, LFE_FILTER_LP);
    lfe_filter_set_cutoff(&flt, 2000u, TEST_RATE);
    lfe_filter_set_q(&flt, 0x2000);   /* moderately resonant */

    lfe_noise_state n;
    lfe_noise_init(&n, 0x55AA55AA);

    int max_abs = 0;
    for (int i = 0; i < 32000; i++) {
        q15_t y = lfe_filter_step(&flt, (int16_t)(lfe_noise_step(&n) >> 1));
        int v = y < 0 ? -y : y;
        if (v > max_abs) max_abs = v;
    }

    /* The filter is allowed to amplify peaks somewhat with resonance
     * but must not produce out-of-range values (the saturation step
     * in lfe_filter_step caps at Q15_ONE = 32767). */
    LFE_TEST_ASSERT(max_abs <= 32767,
                    "resonant LP output stays within Q15 range");
    LFE_TEST_ASSERT(max_abs > 1000,
                    "resonant LP produces nontrivial output");
}

static void test_filter_wav_dump(void)
{
    LFE_TEST_HEADER("filter WAV dump");

    /* Filtered noise sweep: noise through a LP whose cutoff sweeps
     * from low to high over 1 second. Listening to this is a quick
     * sanity check that the filter sounds approximately right. */
    const uint32_t length = TEST_RATE;  /* 1 second */
    int16_t *buf = (int16_t *)calloc(length, sizeof(int16_t));
    LFE_TEST_ASSERT(buf != NULL, "alloc filter wav buf");
    if (!buf) return;

    lfe_filter_state flt;
    lfe_filter_init(&flt, LFE_FILTER_LP);
    lfe_filter_set_q(&flt, 0x3000);

    lfe_noise_state n;
    lfe_noise_init(&n, 0xC0FFEE);

    for (uint32_t i = 0; i < length; i++) {
        /* Sweep cutoff from 100 Hz at i=0 to 8000 Hz at i=length-1. */
        uint32_t cutoff = 100u + (7900u * i) / length;
        lfe_filter_set_cutoff(&flt, cutoff, TEST_RATE);
        buf[i] = lfe_filter_step(&flt, (int16_t)(lfe_noise_step(&n) >> 1));
    }

    int rc = lfe_test_wav_write_mono16("test/output/filter_lp_sweep.wav",
                                       buf, length, TEST_RATE);
    LFE_TEST_ASSERT_EQ(rc, 0, "filter sweep WAV file written");

    free(buf);
}

void lfe_test_filter(void)
{
    test_filter_lowpass_attenuates_highs();
    test_filter_highpass_attenuates_lows();
    test_filter_determinism();
    test_filter_stability();
    test_filter_wav_dump();
}
