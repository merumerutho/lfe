/*
 * test_envelope.c — Tests for the ADSR envelope primitive.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "test_main.h"

#include "util/envelope.h"
#include "util/wavetable.h"
#include "util/fixed.h"
#include "util/wav.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TEST_RATE 32000u

/*
 * Drive an envelope through trigger → run for `attack_dur` ms (cover
 * attack + decay) → run sustain for `sustain_dur` ms → release →
 * release_dur ms → return total samples produced. Output samples are
 * written to `out` if non-NULL. Caller sizes the buffer.
 */
static uint32_t run_envelope(lfe_env_state *env,
                             int16_t *out,
                             uint32_t attack_dur_ms,
                             uint32_t sustain_dur_ms,
                             uint32_t release_dur_ms)
{
    const uint32_t attack_samples  = (attack_dur_ms  * TEST_RATE) / 1000u;
    const uint32_t sustain_samples = (sustain_dur_ms * TEST_RATE) / 1000u;
    const uint32_t release_samples = (release_dur_ms * TEST_RATE) / 1000u;
    const uint32_t total = attack_samples + sustain_samples + release_samples;

    lfe_env_trigger(env);

    uint32_t i = 0;
    for (; i < attack_samples + sustain_samples; i++) {
        q15_t level = lfe_env_step(env);
        if (out) out[i] = level;
    }

    lfe_env_release(env);

    for (; i < total; i++) {
        q15_t level = lfe_env_step(env);
        if (out) out[i] = level;
    }

    return total;
}

static void test_envelope_basic_shape(void)
{
    LFE_TEST_HEADER("envelope basic shape");

    lfe_env_state env;
    lfe_env_params p = {
        .attack_ms     = 10,
        .decay_ms      = 50,
        .sustain_level = Q15_HALF,    /* 0.5 */
        .release_ms    = 100,
        .peak_level    = Q15_ONE,
    };
    lfe_env_init(&env, &p, TEST_RATE);

    /* After init, envelope should be IDLE at zero. */
    LFE_TEST_ASSERT(lfe_env_is_idle(&env), "init starts in IDLE");
    LFE_TEST_ASSERT_EQ(lfe_env_step(&env), 0, "step in IDLE returns 0");

    /* Trigger and run for long enough to reach sustain. */
    lfe_env_trigger(&env);
    LFE_TEST_ASSERT(!lfe_env_is_idle(&env), "trigger leaves IDLE");

    /* Run 200 ms — well past attack (10) + decay (50) — must be at
     * sustain by then. */
    int16_t last = 0;
    for (uint32_t i = 0; i < (200u * TEST_RATE) / 1000u; i++) {
        last = lfe_env_step(&env);
    }

    /* Allow some rounding tolerance around Q15_HALF (16384). */
    LFE_TEST_ASSERT(last > 16000 && last < 16800,
                    "envelope reaches sustain level (~Q15_HALF)");

    /* Release and run to completion. */
    lfe_env_release(&env);
    for (uint32_t i = 0; i < (200u * TEST_RATE) / 1000u; i++) {
        lfe_env_step(&env);
    }
    LFE_TEST_ASSERT(lfe_env_is_idle(&env), "release returns to IDLE");
}

static void test_envelope_zero_attack(void)
{
    LFE_TEST_HEADER("envelope zero attack");

    lfe_env_state env;
    lfe_env_params p = {
        .attack_ms     = 0,
        .decay_ms      = 0,
        .sustain_level = Q15_ONE,
        .release_ms    = 0,
        .peak_level    = Q15_ONE,
    };
    lfe_env_init(&env, &p, TEST_RATE);

    lfe_env_trigger(&env);
    /* One step should be enough to reach peak with zero attack. */
    int16_t s1 = lfe_env_step(&env);
    LFE_TEST_ASSERT(s1 >= 32000,
                    "zero-attack envelope hits peak in one step");
}

static void test_envelope_determinism(void)
{
    LFE_TEST_HEADER("envelope determinism");

    lfe_env_params p = {
        .attack_ms     = 20,
        .decay_ms      = 80,
        .sustain_level = 24000,
        .release_ms    = 200,
        .peak_level    = 30000,
    };

    const uint32_t total = ((20u + 80u + 200u + 50u) * TEST_RATE) / 1000u;
    int16_t *a = (int16_t *)calloc(total, sizeof(int16_t));
    int16_t *b = (int16_t *)calloc(total, sizeof(int16_t));
    LFE_TEST_ASSERT(a && b, "alloc env determinism buffers");
    if (!a || !b) { free(a); free(b); return; }

    lfe_env_state env_a, env_b;
    lfe_env_init(&env_a, &p, TEST_RATE);
    lfe_env_init(&env_b, &p, TEST_RATE);

    run_envelope(&env_a, a, 20 + 80 + 50 /* a+d+sustain hold */,
                 0, 200);
    run_envelope(&env_b, b, 20 + 80 + 50, 0, 200);

    LFE_TEST_ASSERT(memcmp(a, b, total * sizeof(int16_t)) == 0,
                    "two runs produce identical envelope curves");

    free(a);
    free(b);
}

static void test_envelope_wav_dump(void)
{
    LFE_TEST_HEADER("envelope WAV dump");

    /* Render a 1-second envelope (multiplied by a 440 Hz tone for
     * audibility) so a human can ear-test the shape. */
    const uint32_t length = TEST_RATE; /* 1 second */
    int16_t *buf = (int16_t *)calloc(length, sizeof(int16_t));
    LFE_TEST_ASSERT(buf != NULL, "alloc env wav buf");
    if (!buf) return;

    lfe_env_state env;
    lfe_env_params p = {
        .attack_ms     = 50,
        .decay_ms      = 150,
        .sustain_level = 16000,
        .release_ms    = 300,
        .peak_level    = 32000,
    };
    lfe_env_init(&env, &p, TEST_RATE);

    /* Combine the envelope with a 440 Hz sine carrier so a human can
     * actually hear the envelope shape. The wavetable header is
     * included above; its inline lookup is what we use here. */
    lfe_wavetable_init();  /* idempotent */
    lfe_env_trigger(&env);
    const uint32_t sustain_until = (500u * TEST_RATE) / 1000u;
    lfe_phase_t phase = 0;
    lfe_phase_t inc   = lfe_freq_to_phase_inc(440u << 8, TEST_RATE);

    for (uint32_t i = 0; i < length; i++) {
        if (i == sustain_until) lfe_env_release(&env);
        q15_t env_level = lfe_env_step(&env);
        q15_t sine      = lfe_wt_sine_lookup(phase);
        buf[i] = (int16_t)(((int32_t)sine * env_level) >> 15);
        phase += inc;
    }

    int rc = lfe_test_wav_write_mono16("test/output/envelope_440hz_adsr.wav",
                                       buf, length, TEST_RATE);
    LFE_TEST_ASSERT_EQ(rc, 0, "envelope WAV file written");

    free(buf);
}

void lfe_test_envelope(void)
{
    test_envelope_basic_shape();
    test_envelope_zero_attack();
    test_envelope_determinism();
    test_envelope_wav_dump();
}
