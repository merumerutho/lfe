/*
 * test_drum.c — Tests for the drum generator (Phase 3).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Covers parameter validation, determinism (same params → identical
 * output), basic energy/shape sanity checks on the generated samples,
 * and WAV dumps of each preset for ear-testing. Also exercises the
 * free-form side of the generator — a custom params struct that
 * doesn't match any preset — to make sure the modulation routing
 * works outside the canned shapes.
 */

#include "test_main.h"

#include "lfe.h"
#include "util/fixed.h"   /* Q15_ONE for the freeform-patch literal */
#include "util/wav.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DRUM_RATE   32000u
#define DRUM_LEN_MS 500u
#define DRUM_LEN    ((DRUM_RATE * DRUM_LEN_MS) / 1000u)

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int32_t abs_peak(const int16_t *buf, uint32_t n)
{
    int32_t peak = 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t v = buf[i] < 0 ? -(int32_t)buf[i] : (int32_t)buf[i];
        if (v > peak) peak = v;
    }
    return peak;
}

static int64_t sum_abs(const int16_t *buf, uint32_t n)
{
    int64_t s = 0;
    for (uint32_t i = 0; i < n; i++) {
        s += buf[i] < 0 ? -(int64_t)buf[i] : (int64_t)buf[i];
    }
    return s;
}

static void gen_and_check(const lfe_drum_params *p, int16_t *buf, uint32_t n,
                          const char *label)
{
    memset(buf, 0, n * sizeof(int16_t));
    lfe_buffer outbuf = { .data = buf, .length = n, .rate = LFE_RATE_32000 };
    lfe_status rc = lfe_gen_drum(&outbuf, p);
    /* Success means LFE_OK or LFE_WARN_CLIPPED (positive value). */
    LFE_TEST_ASSERT(rc >= 0, label);
}

/* ------------------------------------------------------------------ */
/* Individual tests                                                    */
/* ------------------------------------------------------------------ */

static void test_drum_param_validation(void)
{
    LFE_TEST_HEADER("drum param validation");

    int16_t buf[64];
    lfe_buffer outbuf = { .data = buf, .length = 64, .rate = LFE_RATE_32000 };
    lfe_drum_params p;
    lfe_drum_fill_preset(&p, LFE_DRUM_PRESET_KICK);

    LFE_TEST_ASSERT_EQ(lfe_gen_drum(NULL, &p),      LFE_ERR_NULL,
                       "NULL out → LFE_ERR_NULL");
    LFE_TEST_ASSERT_EQ(lfe_gen_drum(&outbuf, NULL), LFE_ERR_NULL,
                       "NULL params → LFE_ERR_NULL");

    lfe_buffer bad_data = { .data = NULL, .length = 64, .rate = LFE_RATE_32000 };
    LFE_TEST_ASSERT_EQ(lfe_gen_drum(&bad_data, &p), LFE_ERR_NULL,
                       "NULL out->data → LFE_ERR_NULL");

    /* Fill_preset with unknown enum. */
    lfe_drum_params dummy;
    LFE_TEST_ASSERT_EQ(lfe_drum_fill_preset(&dummy, (lfe_drum_preset)9999),
                       LFE_ERR_BAD_PARAM,
                       "unknown preset → LFE_ERR_BAD_PARAM");

    LFE_TEST_ASSERT_EQ(lfe_drum_fill_preset(NULL, LFE_DRUM_PRESET_KICK),
                       LFE_ERR_NULL,
                       "fill_preset NULL → LFE_ERR_NULL");
}

static void test_drum_fill_preset_all(void)
{
    LFE_TEST_HEADER("drum fill_preset for all presets");

    static const lfe_drum_preset presets[] = {
        LFE_DRUM_PRESET_KICK, LFE_DRUM_PRESET_SNARE,
        LFE_DRUM_PRESET_HAT_CLOSED, LFE_DRUM_PRESET_HAT_OPEN,
        LFE_DRUM_PRESET_TOM, LFE_DRUM_PRESET_CLAP,
        LFE_DRUM_PRESET_KICK_808, LFE_DRUM_PRESET_COWBELL,
    };
    for (size_t k = 0; k < sizeof(presets) / sizeof(presets[0]); k++) {
        lfe_drum_params p;
        lfe_status rc = lfe_drum_fill_preset(&p, presets[k]);
        LFE_TEST_ASSERT_EQ(rc, LFE_OK, "fill_preset OK");
        /* Every preset should have at least one active mod, and
         * master_level should be non-zero or the preset produces
         * silence. */
        LFE_TEST_ASSERT(p.master_level > 0, "preset master_level > 0");
        int any = 0;
        for (int j = 0; j < LFE_DRUM_NUM_MODS; j++) {
            if (p.mods[j].target != LFE_DRUM_MOD_NONE) { any = 1; break; }
        }
        LFE_TEST_ASSERT(any, "preset has at least one active mod");
    }
}

static void test_drum_determinism(void)
{
    LFE_TEST_HEADER("drum determinism");

    lfe_drum_params p;
    lfe_drum_fill_preset(&p, LFE_DRUM_PRESET_SNARE);
    /* Force a fixed seed so the LFSR starts identically both times. */
    p.noise_seed = 0xB00Bu;

    int16_t *a = (int16_t *)calloc(DRUM_LEN, sizeof(int16_t));
    int16_t *b = (int16_t *)calloc(DRUM_LEN, sizeof(int16_t));
    if (!a || !b) { free(a); free(b); return; }

    gen_and_check(&p, a, DRUM_LEN, "first gen OK");
    gen_and_check(&p, b, DRUM_LEN, "second gen OK");

    LFE_TEST_ASSERT(memcmp(a, b, DRUM_LEN * sizeof(int16_t)) == 0,
                    "same params → byte-identical output");

    free(a); free(b);
}

static void test_drum_energy_envelope_decay(void)
{
    LFE_TEST_HEADER("drum envelope decays over time");

    /* A kick is mostly tone with a ~180 ms amp decay. The first quarter
     * of a 500 ms buffer should contain substantially more energy than
     * the last quarter. */
    lfe_drum_params p;
    lfe_drum_fill_preset(&p, LFE_DRUM_PRESET_KICK);

    int16_t *buf = (int16_t *)calloc(DRUM_LEN, sizeof(int16_t));
    if (!buf) return;

    gen_and_check(&p, buf, DRUM_LEN, "kick gen OK");

    uint32_t q = DRUM_LEN / 4u;
    int64_t  e_head = sum_abs(buf,             q);
    int64_t  e_tail = sum_abs(buf + DRUM_LEN - q, q);

    LFE_TEST_ASSERT(e_head > 0,              "head has energy");
    LFE_TEST_ASSERT(e_head > e_tail * 4,     "head ≫ tail (envelope decayed)");

    free(buf);
}

static void test_drum_silent_with_zero_amp(void)
{
    LFE_TEST_HEADER("drum silent when master = 0");

    lfe_drum_params p;
    lfe_drum_fill_preset(&p, LFE_DRUM_PRESET_KICK);
    p.master_level = 0;   /* kills amp via the amp-mod multiplication */

    int16_t buf[1024];
    gen_and_check(&p, buf, 1024, "zero-amp gen OK");

    int32_t peak = abs_peak(buf, 1024);
    LFE_TEST_ASSERT_EQ(peak, 0, "output is all zeros");
}

static void test_drum_custom_freeform(void)
{
    LFE_TEST_HEADER("drum free-form custom patch");

    /* A nonsense patch that doesn't match any preset: low-rate tone at
     * 300 Hz, no noise, two AMP mods that stagger (a slow attack
     * crossfade), and a PITCH mod adding a rising sweep. The point of
     * this test is to exercise the routing: multiple mods, non-preset
     * values, signed pitch depth. */
    lfe_drum_params p = {
        .tone_base_hz_q8 = 300u << 8,
        .tone_level      = Q15_ONE,
        .noise_level     = 0,
        .noise_seed      = 0,
        .filter_mode     = LFE_DRUM_FILTER_LP,
        .filter_base_hz  = 8000,
        .filter_q        = Q15_ONE,
        .master_level    = Q15_ONE,
        .mods = {
            /* AMP slot: slow attack, long decay. */
            { .env = { .attack_ms = 40, .decay_ms = 300, .sustain_level = 0,
                       .release_ms = 0, .peak_level = Q15_ONE },
              .target = LFE_DRUM_MOD_AMP,   .depth = Q15_ONE },
            /* PITCH slot: positive depth — rises then falls with env. */
            { .env = { .attack_ms = 20, .decay_ms = 200, .sustain_level = 0,
                       .release_ms = 0, .peak_level = Q15_ONE },
              .target = LFE_DRUM_MOD_PITCH, .depth = 600 },
            /* Unused. */
            { .env = { 0 }, .target = LFE_DRUM_MOD_NONE, .depth = 0 },
        },
    };

    int16_t *buf = (int16_t *)calloc(DRUM_LEN, sizeof(int16_t));
    if (!buf) return;
    gen_and_check(&p, buf, DRUM_LEN, "freeform gen OK");

    int32_t peak = abs_peak(buf, DRUM_LEN);
    LFE_TEST_ASSERT(peak > 4000, "freeform output has substantial energy");

    /* With a 40 ms attack, the very first sample should be near zero. */
    LFE_TEST_ASSERT(buf[0] > -512 && buf[0] < 512,
                    "first sample near zero (slow attack)");

    /* And there should be energy well past the 40 ms mark. */
    uint32_t mid = DRUM_LEN / 2u;
    int64_t e_mid = sum_abs(buf + mid, DRUM_LEN / 8u);
    LFE_TEST_ASSERT(e_mid > 0, "mid-buffer has energy");

    free(buf);
}

static void test_drum_wav_dump_presets(void)
{
    LFE_TEST_HEADER("drum WAV dumps");

    static const struct {
        lfe_drum_preset preset;
        const char     *path;
    } presets[] = {
        { LFE_DRUM_PRESET_KICK,       "test/output/drum_kick.wav"       },
        { LFE_DRUM_PRESET_SNARE,      "test/output/drum_snare.wav"      },
        { LFE_DRUM_PRESET_HAT_CLOSED, "test/output/drum_hat_closed.wav" },
        { LFE_DRUM_PRESET_HAT_OPEN,   "test/output/drum_hat_open.wav"   },
        { LFE_DRUM_PRESET_TOM,        "test/output/drum_tom.wav"        },
        { LFE_DRUM_PRESET_CLAP,       "test/output/drum_clap.wav"       },
        { LFE_DRUM_PRESET_KICK_808,   "test/output/drum_kick_808.wav"   },
        { LFE_DRUM_PRESET_COWBELL,    "test/output/drum_cowbell.wav"    },
    };

    for (size_t k = 0; k < sizeof(presets) / sizeof(presets[0]); k++) {
        lfe_drum_params p;
        lfe_drum_fill_preset(&p, presets[k].preset);

        int16_t *buf = (int16_t *)calloc(DRUM_LEN, sizeof(int16_t));
        if (!buf) continue;

        lfe_buffer outbuf = {
            .data = buf, .length = DRUM_LEN, .rate = LFE_RATE_32000,
        };
        lfe_status rc = lfe_gen_drum(&outbuf, &p);
        LFE_TEST_ASSERT(rc >= 0, "preset gen succeeded");

        int wav_rc = lfe_test_wav_write_mono16(presets[k].path, buf,
                                               DRUM_LEN, DRUM_RATE);
        LFE_TEST_ASSERT_EQ(wav_rc, 0, "preset WAV written");

        free(buf);
    }
}

/* ------------------------------------------------------------------ */
/* Phase-expansion tests (waveshape, drive, LFO)                       */
/* ------------------------------------------------------------------ */

/* Changing tone_wave must change the output — proves the dispatch
 * in drum_tone_sample reaches the render loop. Compare SINE vs
 * SQUARE on an otherwise identical patch. */
static void test_drum_waveshape_affects_output(void)
{
    LFE_TEST_HEADER("drum tone_wave changes output");

    lfe_drum_params sine_p;
    lfe_drum_fill_preset(&sine_p, LFE_DRUM_PRESET_TOM);
    sine_p.tone_wave = LFE_DRUM_WAVE_SINE;

    lfe_drum_params sq_p = sine_p;
    sq_p.tone_wave = LFE_DRUM_WAVE_SQUARE;

    int16_t *a = (int16_t *)calloc(DRUM_LEN, sizeof(int16_t));
    int16_t *b = (int16_t *)calloc(DRUM_LEN, sizeof(int16_t));
    if (!a || !b) { free(a); free(b); return; }

    gen_and_check(&sine_p, a, DRUM_LEN, "tom SINE");
    gen_and_check(&sq_p,   b, DRUM_LEN, "tom SQUARE");

    LFE_TEST_ASSERT(memcmp(a, b, DRUM_LEN * sizeof(int16_t)) != 0,
                    "waveshape changes output");
    LFE_TEST_ASSERT(abs_peak(b, DRUM_LEN) > 2000,
                    "square tom still audible");

    free(a); free(b);
}

/* Drive raises the integrated absolute energy (soft-clipped boost).
 * Use a tone-only patch so filter noise doesn't confuse the measurement. */
static void test_drum_drive_affects_output(void)
{
    LFE_TEST_HEADER("drum drive adds energy");

    lfe_drum_params clean = {
        .tone_base_hz_q8 = 200u << 8,
        .tone_level      = Q15_ONE / 2,   /* deliberately below full */
        .tone_wave       = LFE_DRUM_WAVE_SINE,
        .master_level    = Q15_ONE,
        .drive           = 0,
        .mods = {
            { .env = { .attack_ms = 0, .decay_ms = 0,
                       .sustain_level = Q15_ONE,
                       .release_ms = 0, .peak_level = Q15_ONE },
              .target = LFE_DRUM_MOD_AMP, .depth = Q15_ONE },
            { .env = { 0 }, .target = LFE_DRUM_MOD_NONE, .depth = 0 },
            { .env = { 0 }, .target = LFE_DRUM_MOD_NONE, .depth = 0 },
        },
    };

    lfe_drum_params hot = clean;
    hot.drive = Q15_ONE;  /* maximum */

    int16_t *a = (int16_t *)calloc(DRUM_LEN, sizeof(int16_t));
    int16_t *b = (int16_t *)calloc(DRUM_LEN, sizeof(int16_t));
    if (!a || !b) { free(a); free(b); return; }

    gen_and_check(&clean, a, DRUM_LEN, "clean tone");
    gen_and_check(&hot,   b, DRUM_LEN, "driven tone");

    int64_t e_clean = sum_abs(a, DRUM_LEN);
    int64_t e_hot   = sum_abs(b, DRUM_LEN);

    LFE_TEST_ASSERT(e_hot > e_clean,
                    "driven output has more integrated energy");
    LFE_TEST_ASSERT(abs_peak(b, DRUM_LEN) <= Q15_ONE,
                    "drive output never exceeds Q15");

    free(a); free(b);
}

/* A depth-zero LFO must behave identically to an OFF LFO. Guards
 * against the LFO loop running even when it shouldn't. */
static void test_drum_lfo_zero_depth_is_bypassed(void)
{
    LFE_TEST_HEADER("drum LFO with depth=0 is bypassed");

    lfe_drum_params p_off;
    lfe_drum_fill_preset(&p_off, LFE_DRUM_PRESET_TOM);
    p_off.lfo.dest = LFE_DRUM_LFO_DEST_OFF;
    p_off.lfo.cfg.depth = 0;

    lfe_drum_params p_zero = p_off;
    /* Non-OFF dest but depth 0 — LFO init happens but output is 0. */
    p_zero.lfo.dest = LFE_DRUM_LFO_DEST_TONE_LEVEL;
    p_zero.lfo.cfg.shape = LFE_LFO_SHAPE_SINE;
    p_zero.lfo.cfg.rate_hz_q8 = 5u << 8;
    p_zero.lfo.cfg.depth = 0;

    int16_t *a = (int16_t *)calloc(DRUM_LEN, sizeof(int16_t));
    int16_t *b = (int16_t *)calloc(DRUM_LEN, sizeof(int16_t));
    if (!a || !b) { free(a); free(b); return; }

    gen_and_check(&p_off,  a, DRUM_LEN, "LFO OFF");
    gen_and_check(&p_zero, b, DRUM_LEN, "LFO on-but-zero-depth");

    LFE_TEST_ASSERT(memcmp(a, b, DRUM_LEN * sizeof(int16_t)) == 0,
                    "depth-zero LFO is a no-op");

    free(a); free(b);
}

/* A non-zero LFO on TONE_LEVEL must perturb the output. */
static void test_drum_lfo_reaches_output(void)
{
    LFE_TEST_HEADER("drum LFO modulation reaches output");

    lfe_drum_params p_base;
    lfe_drum_fill_preset(&p_base, LFE_DRUM_PRESET_TOM);
    p_base.lfo.dest = LFE_DRUM_LFO_DEST_OFF;
    p_base.lfo.cfg.depth = 0;

    lfe_drum_params p_lfo = p_base;
    p_lfo.lfo.dest = LFE_DRUM_LFO_DEST_TONE_LEVEL;
    p_lfo.lfo.cfg.shape = LFE_LFO_SHAPE_SINE;
    p_lfo.lfo.cfg.rate_hz_q8 = 8u << 8;
    p_lfo.lfo.cfg.depth = Q15_ONE / 2;

    int16_t *a = (int16_t *)calloc(DRUM_LEN, sizeof(int16_t));
    int16_t *b = (int16_t *)calloc(DRUM_LEN, sizeof(int16_t));
    if (!a || !b) { free(a); free(b); return; }

    gen_and_check(&p_base, a, DRUM_LEN, "LFO OFF");
    gen_and_check(&p_lfo,  b, DRUM_LEN, "LFO active");

    LFE_TEST_ASSERT(memcmp(a, b, DRUM_LEN * sizeof(int16_t)) != 0,
                    "LFO active changes output");

    free(a); free(b);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

void lfe_test_drum(void)
{
    test_drum_param_validation();
    test_drum_fill_preset_all();
    test_drum_determinism();
    test_drum_energy_envelope_decay();
    test_drum_silent_with_zero_amp();
    test_drum_custom_freeform();
    test_drum_waveshape_affects_output();
    test_drum_drive_affects_output();
    test_drum_lfo_zero_depth_is_bypassed();
    test_drum_lfo_reaches_output();
    test_drum_wav_dump_presets();
}
