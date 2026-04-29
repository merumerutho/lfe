/*
 * test_braids.c — Targeted tests for the Braids port (phases 6a-6c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Scope (option C from the phase-6d planning discussion): tests only
 * where real porting risk lives, not a smoke test per phase.
 *
 *   1. Resource-table sizes  — catch any truncation introduced by the
 *      sed-based port of resources.cc. The upstream sizes are locked
 *      in lfe_braids_resources.h as LUT_X_SIZE / WAV_X_SIZE #defines;
 *      we verify sizeof(array)/sizeof(elem) matches for every retained
 *      table.
 *
 *   2. Interpolation parity  — hand-computed expected values for each
 *      `braids_interp824_*` / `braids_interp88_*` variant. These are
 *      the primitives every oscillator inner loop leans on; a wrong
 *      shift or cast here corrupts every shape silently.
 *
 *   3. SVF stability         — drive a known impulse through the LP
 *      output and verify it decays without wrapping. Mostly a sanity
 *      check that int32 state + BRAIDS_CLIP suffice at typical
 *      cutoff/resonance values.
 *
 *   4. Excitation lifecycle  — trigger, tick to impulse, decay to
 *      near-zero, done() transition.
 *
 * Phase 6d (analog oscillator) will add per-shape WAV dumps in a
 * separate `test_braids_analog.c` gated behind the LFE_BRAIDS_DUMP
 * env var.
 */

#include "test_main.h"

#include "lfe.h"

#include "../src/gen/braids/lfe_braids_dsp.h"
#include "../src/gen/braids/lfe_braids_random.h"
#include "../src/gen/braids/lfe_braids_resources.h"
#include "../src/gen/braids/lfe_braids_svf.h"
#include "../src/gen/braids/lfe_braids_excitation.h"
#include "../src/gen/braids/lfe_braids_analog.h"
#include "../src/gen/braids/lfe_braids_digital.h"
#include "../src/gen/braids/lfe_braids_macro.h"

#include "util/wav.h"

#include <stdlib.h>
#include <string.h>

/* ---- 1. Resource-table sizes ---- */

static void test_braids_resources_sizes(void)
{
    LFE_TEST_HEADER("braids: resource table sizes");

    LFE_TEST_ASSERT_EQ(sizeof(lut_resonator_coefficient) / sizeof(uint16_t),
                       LUT_RESONATOR_COEFFICIENT_SIZE, "resonator_coefficient size");
    LFE_TEST_ASSERT_EQ(sizeof(lut_resonator_scale) / sizeof(uint16_t),
                       LUT_RESONATOR_SCALE_SIZE, "resonator_scale size");
    LFE_TEST_ASSERT_EQ(sizeof(lut_svf_cutoff) / sizeof(uint16_t),
                       LUT_SVF_CUTOFF_SIZE, "svf_cutoff size");
    LFE_TEST_ASSERT_EQ(sizeof(lut_svf_damp) / sizeof(uint16_t),
                       LUT_SVF_DAMP_SIZE, "svf_damp size");
    LFE_TEST_ASSERT_EQ(sizeof(lut_svf_scale) / sizeof(uint16_t),
                       LUT_SVF_SCALE_SIZE, "svf_scale size");
    LFE_TEST_ASSERT_EQ(sizeof(lut_bowing_envelope) / sizeof(uint16_t),
                       LUT_BOWING_ENVELOPE_SIZE, "bowing_envelope size");
    LFE_TEST_ASSERT_EQ(sizeof(lut_bowing_friction) / sizeof(uint16_t),
                       LUT_BOWING_FRICTION_SIZE, "bowing_friction size");
    LFE_TEST_ASSERT_EQ(sizeof(lut_blowing_envelope) / sizeof(uint16_t),
                       LUT_BLOWING_ENVELOPE_SIZE, "blowing_envelope size");
    LFE_TEST_ASSERT_EQ(sizeof(lut_flute_body_filter) / sizeof(uint16_t),
                       LUT_FLUTE_BODY_FILTER_SIZE, "flute_body_filter size");
    LFE_TEST_ASSERT_EQ(sizeof(lut_vco_detune) / sizeof(uint16_t),
                       LUT_VCO_DETUNE_SIZE, "vco_detune size");
    LFE_TEST_ASSERT_EQ(sizeof(lut_env_expo) / sizeof(uint16_t),
                       LUT_ENV_EXPO_SIZE, "env_expo size");
    LFE_TEST_ASSERT_EQ(sizeof(lut_blowing_jet) / sizeof(int16_t),
                       LUT_BLOWING_JET_SIZE, "blowing_jet size");
    LFE_TEST_ASSERT_EQ(sizeof(lut_oscillator_increments) / sizeof(uint32_t),
                       LUT_OSCILLATOR_INCREMENTS_SIZE, "oscillator_increments size");
    LFE_TEST_ASSERT_EQ(sizeof(lut_oscillator_delays) / sizeof(uint32_t),
                       LUT_OSCILLATOR_DELAYS_SIZE, "oscillator_delays size");
    LFE_TEST_ASSERT_EQ(sizeof(lut_env_portamento_increments) / sizeof(uint32_t),
                       LUT_ENV_PORTAMENTO_INCREMENTS_SIZE,
                       "env_portamento_increments size");

    LFE_TEST_ASSERT_EQ(sizeof(wav_formant_sine) / sizeof(int16_t),
                       WAV_FORMANT_SINE_SIZE, "wav_formant_sine size");
    LFE_TEST_ASSERT_EQ(sizeof(wav_formant_square) / sizeof(int16_t),
                       WAV_FORMANT_SQUARE_SIZE, "wav_formant_square size");
    LFE_TEST_ASSERT_EQ(sizeof(wav_sine) / sizeof(int16_t),
                       WAV_SINE_SIZE, "wav_sine size");

    /* The 15 bandlimited comb tables share one size constant. */
    const int16_t *combs[] = {
        wav_bandlimited_comb_0,  wav_bandlimited_comb_1,
        wav_bandlimited_comb_2,  wav_bandlimited_comb_3,
        wav_bandlimited_comb_4,  wav_bandlimited_comb_5,
        wav_bandlimited_comb_6,  wav_bandlimited_comb_7,
        wav_bandlimited_comb_8,  wav_bandlimited_comb_9,
        wav_bandlimited_comb_10, wav_bandlimited_comb_11,
        wav_bandlimited_comb_12, wav_bandlimited_comb_13,
        wav_bandlimited_comb_14,
    };
    for (size_t i = 0; i < sizeof(combs) / sizeof(combs[0]); i++) {
        LFE_TEST_ASSERT(combs[i] != NULL, "comb table non-null");
    }

    /* waveform_table[WAV_BANDLIMITED_COMB_N] must return comb N. The
     * saw-swarm path in analog_oscillator.cc reads waveform_table with
     * `WAV_BANDLIMITED_COMB_0 + index` — if the table's layout drifted
     * during the port we would silently play the wrong wave. */
    for (int i = 0; i <= 14; i++) {
        LFE_TEST_ASSERT_EQ(
            (const void *)waveform_table[WAV_BANDLIMITED_COMB_0 + i],
            (const void *)combs[i],
            "waveform_table layout matches comb index");
    }
}

/* ---- 2. Interpolation parity ---- */

static void test_braids_interp(void)
{
    LFE_TEST_HEADER("braids: interpolation primitives");

    /* Linear interp between 0 and 1000 at phase = 0 → 0. */
    const int16_t ramp_s16[3] = { 0, 1000, 2000 };
    LFE_TEST_ASSERT_EQ(braids_interp824_s16(ramp_s16, 0u),
                       0, "interp824_s16 at phase 0");

    /* At phase 0x00800000 (halfway through entry 0) → ~500. */
    int16_t half = braids_interp824_s16(ramp_s16, 0x00800000u);
    LFE_TEST_ASSERT(half >= 498 && half <= 502,
                    "interp824_s16 at half between 0 and 1000");

    /* At phase 0x01000000 (entry 1 exactly) → 1000. */
    LFE_TEST_ASSERT_EQ(braids_interp824_s16(ramp_s16, 0x01000000u),
                       1000, "interp824_s16 at entry 1");

    /* u16 variant — same ramp cast to uint16. */
    const uint16_t ramp_u16[3] = { 100, 300, 500 };
    uint16_t u = braids_interp824_u16(ramp_u16, 0x00800000u);
    LFE_TEST_ASSERT(u >= 199 && u <= 201,
                    "interp824_u16 at half between 100 and 300");

    /* u8 variant — stmlib widens and subtracts 32768 to center. */
    const uint8_t ramp_u8[3] = { 128, 128, 128 };
    int16_t c = braids_interp824_u8(ramp_u8, 0u);
    LFE_TEST_ASSERT_EQ(c, 0, "interp824_u8 centered (128 -> ~0)");

    /* interp88: 8.8 fixed — index's top byte selects entry. */
    LFE_TEST_ASSERT_EQ(braids_interp88_s16(ramp_s16, 0x0000),
                       0, "interp88_s16 at index 0");
    LFE_TEST_ASSERT_EQ(braids_interp88_s16(ramp_s16, 0x0100),
                       1000, "interp88_s16 at index 1");

    /* Mix is `(a*(65535-balance) + b*balance) >> 16`. Multiplier is
     * 65535 (not 65536) and the shift is 16, so results systematically
     * undershoot by up to 1 ulp — this matches upstream Braids exactly.
     * The assertions below check the actual arithmetic, not an
     * idealized linear interpolation. */
    LFE_TEST_ASSERT_EQ(braids_mix_s16(100, 500, 0),
                       99, "mix at balance 0 (upstream: 99, not 100)");
    LFE_TEST_ASSERT_EQ(braids_mix_s16(100, 500, 0xFFFF),
                       499, "mix at balance 0xFFFF (upstream: 499)");
    LFE_TEST_ASSERT_EQ(braids_mix_s16(100, 500, 0x8000),
                       299, "mix at half balance (upstream: 299)");
}

/* ---- 3. SVF stability ---- */

static void test_braids_svf(void)
{
    LFE_TEST_HEADER("braids: SVF stability");

    braids_svf_t s;
    braids_svf_init(&s);
    braids_svf_set_mode(&s, BRAIDS_SVF_MODE_LP);
    braids_svf_set_frequency(&s, 60 << 7);    /* mid range */
    braids_svf_set_resonance(&s, 16384);

    /* Impulse at t=0 then silence — LP output must decay toward 0 and
     * never exceed the CLIP bound. */
    int32_t peak = 0;
    int32_t final_mag;
    for (int i = 0; i < 2048; i++) {
        int32_t in = (i == 0) ? 24000 : 0;
        int32_t y  = braids_svf_process(&s, in);
        int32_t mag = y < 0 ? -y : y;
        if (mag > peak) peak = mag;
    }
    final_mag = s.lp < 0 ? -s.lp : s.lp;
    LFE_TEST_ASSERT(peak <= 32767, "SVF output within s16 bounds");
    LFE_TEST_ASSERT(final_mag < peak,
                    "SVF LP state decays below peak after 2048 samples");
}

/* ---- 4. Excitation lifecycle ---- */

static void test_braids_excitation(void)
{
    LFE_TEST_HEADER("braids: excitation lifecycle");

    braids_excitation_t e;
    braids_excitation_init(&e);
    LFE_TEST_ASSERT(braids_excitation_done(&e), "fresh excitation is done");

    braids_excitation_set_delay(&e, 2);
    braids_excitation_set_decay(&e, 4000);
    braids_excitation_trigger(&e, 20000);
    LFE_TEST_ASSERT(!braids_excitation_done(&e), "triggered excitation not done");

    /* First `delay+1` ticks have the impulse pending. After counter
     * reaches 0 the impulse fires into `state`. */
    int32_t first  = braids_excitation_process(&e);
    int32_t second = braids_excitation_process(&e);
    int32_t third  = braids_excitation_process(&e);
    (void)first; (void)second;
    LFE_TEST_ASSERT(third > 0, "excitation fires after delay ticks");

    /* decay < 4096 should bleed the state toward 0. */
    int32_t prev = third;
    for (int i = 0; i < 100; i++) {
        int32_t cur = braids_excitation_process(&e);
        if (cur > prev) {
            g_lfe_test_failed++;
            fprintf(stderr, "FAIL: excitation rose after impulse (i=%d, %d>%d)\n",
                    i, cur, prev);
            return;
        }
        prev = cur;
    }
    g_lfe_test_passed++;
    LFE_TEST_ASSERT(prev < third,
                    "excitation decayed well below impulse peak");
}

/* ---- 5. Analog-oscillator render smoke test ----
 *
 * Renders ~0.25 s of each retained shape at a fixed MIDI pitch (A4
 * = 69) at 96 kHz, verifies the output is non-trivial (not all-zero,
 * not clipping flat), and — if LFE_BRAIDS_DUMP=1 — dumps a WAV to
 * test/output/braids_analog_<shape>.wav for ear-testing.
 *
 * "Non-trivial" is defined as: peak absolute value > 1000 (proves
 * the shape actually fires) AND zero-crossings > 1 (proves it's a
 * waveform, not DC). Tighter regression checks can come later when
 * we have reference dumps captured from an upstream Braids build.
 */

static const int16_t k_a4_pitch = 69 << 7;   /* Braids midi_pitch units */
#define A_OSC_BLOCK   64
#define A_OSC_SAMPLES (96000 / 4)   /* 0.25 s at 96 kHz */

static void render_shape_block(braids_analog_shape shape,
                               int16_t *out, size_t total)
{
    braids_analog_osc_t osc;
    braids_analog_osc_init(&osc);
    braids_analog_osc_set_shape(&osc, shape);
    braids_analog_osc_set_pitch(&osc, k_a4_pitch);
    braids_analog_osc_set_parameter(&osc, 16384);       /* mid */
    braids_analog_osc_set_aux_parameter(&osc, 16384);

    uint8_t sync_in[A_OSC_BLOCK] = {0};
    size_t done = 0;
    while (done < total) {
        size_t n = total - done;
        if (n > A_OSC_BLOCK) n = A_OSC_BLOCK;
        braids_analog_osc_render(&osc, sync_in, out + done, NULL, n);
        done += n;
    }
}

static void smoke_one_shape(braids_analog_shape shape, const char *name)
{
    static int16_t buf[A_OSC_SAMPLES];
    memset(buf, 0, sizeof(buf));
    render_shape_block(shape, buf, A_OSC_SAMPLES);

    int32_t peak = 0;
    int zero_crossings = 0;
    int16_t prev = 0;
    for (size_t i = 0; i < A_OSC_SAMPLES; i++) {
        int32_t mag = buf[i] < 0 ? -buf[i] : buf[i];
        if (mag > peak) peak = mag;
        if (i > 0 && ((prev < 0) != (buf[i] < 0))) zero_crossings++;
        prev = buf[i];
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "braids analog %s: peak > 1000", name);
    LFE_TEST_ASSERT(peak > 1000, msg);
    snprintf(msg, sizeof(msg), "braids analog %s: >1 zero-crossing", name);
    LFE_TEST_ASSERT(zero_crossings > 1, msg);

    if (getenv("LFE_BRAIDS_DUMP")) {
        char path[128];
        snprintf(path, sizeof(path),
                 "test/output/braids_analog_%s.wav", name);
        lfe_test_wav_write_mono16(path, buf, A_OSC_SAMPLES, 96000);
    }
}

static void test_braids_analog_shapes(void)
{
    LFE_TEST_HEADER("braids: analog oscillator shapes");
    smoke_one_shape(BRAIDS_OSC_SHAPE_SAW,          "saw");
    smoke_one_shape(BRAIDS_OSC_SHAPE_VARIABLE_SAW, "variable_saw");
    smoke_one_shape(BRAIDS_OSC_SHAPE_CSAW,         "csaw");
    smoke_one_shape(BRAIDS_OSC_SHAPE_SQUARE,       "square");
    smoke_one_shape(BRAIDS_OSC_SHAPE_TRIANGLE,     "triangle");
    smoke_one_shape(BRAIDS_OSC_SHAPE_SINE,         "sine");
}

/* ---- 6. Digital-oscillator smoke tests ----
 *
 * Sub-phase 6e-1: VOWEL_FOF only; remaining shapes are NULL in
 * fn_table and emit silence until their sub-phase lands. Tests are
 * additive — one test_* per shape, added as each ports in.
 */

#define D_OSC_SAMPLES (96000 / 4)
#define D_OSC_BLOCK   64

static void render_digital_block(braids_digital_shape shape,
                                 int16_t *out, size_t total,
                                 int16_t p1, int16_t p2)
{
    braids_digital_osc_t osc;
    braids_digital_osc_init(&osc);
    braids_digital_osc_set_shape(&osc, shape);
    braids_digital_osc_set_pitch(&osc, k_a4_pitch);
    braids_digital_osc_set_parameters(&osc, p1, p2);

    uint8_t sync[D_OSC_BLOCK] = {0};
    size_t done = 0;
    while (done < total) {
        size_t n = total - done;
        if (n > D_OSC_BLOCK) n = D_OSC_BLOCK;
        braids_digital_osc_render(&osc, sync, out + done, n);
        done += n;
    }
}

static void test_braids_digital_vowel_fof(void)
{
    LFE_TEST_HEADER("braids: digital VOWEL_FOF");

    static int16_t buf[D_OSC_SAMPLES];
    memset(buf, 0, sizeof(buf));

    /* parameter[0] = vowel (a..u), parameter[1] = voice (bass..soprano).
     * Mid-range values give a defined-but-not-extremal formant set. */
    render_digital_block(BRAIDS_DOSC_SHAPE_VOWEL_FOF, buf, D_OSC_SAMPLES,
                         16384, 16384);

    int32_t peak = 0;
    int zero_crossings = 0;
    int16_t prev = 0;
    for (size_t i = 0; i < D_OSC_SAMPLES; i++) {
        int32_t mag = buf[i] < 0 ? -buf[i] : buf[i];
        if (mag > peak) peak = mag;
        if (i > 0 && ((prev < 0) != (buf[i] < 0))) zero_crossings++;
        prev = buf[i];
    }

    LFE_TEST_ASSERT(peak > 1000, "vowel_fof: peak > 1000");
    LFE_TEST_ASSERT(zero_crossings > 1, "vowel_fof: >1 zero-crossing");

    if (getenv("LFE_BRAIDS_DUMP")) {
        lfe_test_wav_write_mono16(
            "test/output/braids_digital_vowel_fof.wav",
            buf, D_OSC_SAMPLES, 96000);
    }
}

static void test_braids_digital_twin_peaks_noise(void)
{
    LFE_TEST_HEADER("braids: digital TWIN_PEAKS_NOISE");

    static int16_t buf[D_OSC_SAMPLES];
    memset(buf, 0, sizeof(buf));

    /* p0 = Q / resonance intensity, p1 = second-peak offset from
     * pitch. Mid-range picks both well clear of the edge cases. */
    render_digital_block(BRAIDS_DOSC_SHAPE_TWIN_PEAKS_NOISE,
                         buf, D_OSC_SAMPLES, 16384, 16384);

    int32_t peak = 0;
    int zero_crossings = 0;
    int16_t prev = 0;
    for (size_t i = 0; i < D_OSC_SAMPLES; i++) {
        int32_t mag = buf[i] < 0 ? -buf[i] : buf[i];
        if (mag > peak) peak = mag;
        if (i > 0 && ((prev < 0) != (buf[i] < 0))) zero_crossings++;
        prev = buf[i];
    }

    /* Resonant-filtered noise; peaks are lower than full-harmonic
     * shapes and build up over the first few ms of filter state.
     * Threshold matches the physical-model smoke tests. */
    LFE_TEST_ASSERT(peak > 500, "twin_peaks_noise: peak > 500");
    LFE_TEST_ASSERT(zero_crossings > 10,
                    "twin_peaks_noise: > 10 zero-crossings");

    if (getenv("LFE_BRAIDS_DUMP")) {
        lfe_test_wav_write_mono16(
            "test/output/braids_digital_twin_peaks_noise.wav",
            buf, D_OSC_SAMPLES, 96000);
    }
}

static void test_braids_digital_modulation(void)
{
    LFE_TEST_HEADER("braids: digital DIGITAL_MODULATION");

    static int16_t buf[D_OSC_SAMPLES];
    memset(buf, 0, sizeof(buf));

    /* p0 = symbol-rate offset, p1 = data driver. Mid values stay
     * clear of the edge "symbol_count >= 64+4*256" wrap that resets
     * the state machine. */
    render_digital_block(BRAIDS_DOSC_SHAPE_DIGITAL_MODULATION,
                         buf, D_OSC_SAMPLES, 16384, 16384);

    int32_t peak = 0;
    int zero_crossings = 0;
    int16_t prev = 0;
    for (size_t i = 0; i < D_OSC_SAMPLES; i++) {
        int32_t mag = buf[i] < 0 ? -buf[i] : buf[i];
        if (mag > peak) peak = mag;
        if (i > 0 && ((prev < 0) != (buf[i] < 0))) zero_crossings++;
        prev = buf[i];
    }

    LFE_TEST_ASSERT(peak > 1000, "digital_modulation: peak > 1000");
    LFE_TEST_ASSERT(zero_crossings > 10,
                    "digital_modulation: > 10 zero-crossings");

    if (getenv("LFE_BRAIDS_DUMP")) {
        lfe_test_wav_write_mono16(
            "test/output/braids_digital_modulation.wav",
            buf, D_OSC_SAMPLES, 96000);
    }
}

static void test_braids_digital_plucked(void)
{
    LFE_TEST_HEADER("braids: digital PLUCKED");

    static int16_t buf[D_OSC_SAMPLES];
    memset(buf, 0, sizeof(buf));

    /* p0 = pluck "sustain" bias (below 16384 → loss, above → no loss);
     * p1 = initial-excitation width. `_init` sets strike=1 so the
     * first render fills voice 0's delay line with noise. Further
     * blocks in the same run don't re-strike — we hear the string
     * decay. */
    render_digital_block(BRAIDS_DOSC_SHAPE_PLUCKED, buf, D_OSC_SAMPLES,
                         16384, 16384);

    int32_t peak = 0;
    int zero_crossings = 0;
    int16_t prev = 0;
    for (size_t i = 0; i < D_OSC_SAMPLES; i++) {
        int32_t mag = buf[i] < 0 ? -buf[i] : buf[i];
        if (mag > peak) peak = mag;
        if (i > 0 && ((prev < 0) != (buf[i] < 0))) zero_crossings++;
        prev = buf[i];
    }

    LFE_TEST_ASSERT(peak > 1000, "plucked: peak > 1000");
    LFE_TEST_ASSERT(zero_crossings > 10,
                    "plucked: > 10 zero-crossings");

    if (getenv("LFE_BRAIDS_DUMP")) {
        lfe_test_wav_write_mono16(
            "test/output/braids_digital_plucked.wav",
            buf, D_OSC_SAMPLES, 96000);
    }
}

/* Shared waveguide smoke-test body — same peak+ZC rules, different
 * shape + dump filename. */
static void test_braids_digital_waveguide(braids_digital_shape shape,
                                          const char *name)
{
    char header[64];
    snprintf(header, sizeof(header), "braids: digital %s", name);
    LFE_TEST_HEADER(header);

    static int16_t buf[D_OSC_SAMPLES];
    memset(buf, 0, sizeof(buf));

    render_digital_block(shape, buf, D_OSC_SAMPLES, 16384, 16384);

    int32_t peak = 0;
    int zero_crossings = 0;
    int16_t prev = 0;
    for (size_t i = 0; i < D_OSC_SAMPLES; i++) {
        int32_t mag = buf[i] < 0 ? -buf[i] : buf[i];
        if (mag > peak) peak = mag;
        if (i > 0 && ((prev < 0) != (buf[i] < 0))) zero_crossings++;
        prev = buf[i];
    }

    char msg[80];
    snprintf(msg, sizeof(msg), "%s: peak > 500", name);
    LFE_TEST_ASSERT(peak > 500, msg);
    snprintf(msg, sizeof(msg), "%s: > 5 zero-crossings", name);
    LFE_TEST_ASSERT(zero_crossings > 5, msg);

    if (getenv("LFE_BRAIDS_DUMP")) {
        char path[128];
        snprintf(path, sizeof(path),
                 "test/output/braids_digital_%s.wav", name);
        lfe_test_wav_write_mono16(path, buf, D_OSC_SAMPLES, 96000);
    }
}

/* COMB_FILTER is an in-place filter that expects the caller to fill
 * the buffer with dry signal first. So it gets its own helper. */
static void test_braids_digital_comb_filter(void)
{
    LFE_TEST_HEADER("braids: digital COMB_FILTER");

    static int16_t buf[D_OSC_SAMPLES];
    /* Fill with a simple repeating pulse train at ~440 Hz so the comb
     * has something spectrally rich to feed back on. */
    for (size_t i = 0; i < D_OSC_SAMPLES; i++) {
        buf[i] = (int16_t)((i % 218) < 10 ? 16000 : -16000);
    }

    braids_digital_osc_t osc;
    braids_digital_osc_init(&osc);
    braids_digital_osc_set_shape(&osc, BRAIDS_DOSC_SHAPE_COMB_FILTER);
    braids_digital_osc_set_pitch(&osc, k_a4_pitch);
    braids_digital_osc_set_parameters(&osc, 16384, 20000);  /* moderate resonance */

    uint8_t sync[D_OSC_BLOCK] = {0};
    size_t done = 0;
    while (done < D_OSC_SAMPLES) {
        size_t n = D_OSC_SAMPLES - done;
        if (n > D_OSC_BLOCK) n = D_OSC_BLOCK;
        braids_digital_osc_render(&osc, sync, buf + done, n);
        done += n;
    }

    int32_t peak = 0;
    for (size_t i = 0; i < D_OSC_SAMPLES; i++) {
        int32_t mag = buf[i] < 0 ? -buf[i] : buf[i];
        if (mag > peak) peak = mag;
    }
    LFE_TEST_ASSERT(peak > 500, "comb_filter: peak > 500");

    if (getenv("LFE_BRAIDS_DUMP")) {
        lfe_test_wav_write_mono16(
            "test/output/braids_digital_comb_filter.wav",
            buf, D_OSC_SAMPLES, 96000);
    }
}

/* ---- MACRO layer: one smoke test iterating all 18 shapes.
 *
 * Each shape renders 0.25 s at A4, mid-range parameters. We assert
 * the usual peak + zero-crossing floor, then optionally dump a WAV
 * per shape for ear-testing. A single static buffer is reused across
 * shapes so the test's stack/data footprint stays small. */

static const char *k_macro_names[BRAIDS_MACRO_COUNT] = {
    "csaw", "morph", "saw_square", "sine_triangle",
    "triple_saw", "triple_square", "triple_triangle", "triple_sine",
    "triple_ring_mod", "saw_swarm", "saw_comb",
    "vowel_fof", "plucked", "bowed", "blown", "fluted",
    "twin_peaks_noise", "digital_modulation",
};

static void test_braids_macro_all_shapes(void)
{
    LFE_TEST_HEADER("braids: MACRO layer — 18 shapes");

    static int16_t buf[D_OSC_SAMPLES];

    for (int i = 0; i < BRAIDS_MACRO_COUNT; i++) {
        memset(buf, 0, sizeof(buf));
        braids_macro_osc_t m;
        braids_macro_osc_init(&m);
        braids_macro_osc_set_shape(&m, (braids_macro_shape)i);
        braids_macro_osc_set_pitch(&m, k_a4_pitch);
        braids_macro_osc_set_parameters(&m, 16384, 16384);
        braids_macro_osc_strike(&m);

        braids_macro_osc_render(&m, buf, D_OSC_SAMPLES);

        int32_t peak = 0;
        int zero_crossings = 0;
        int16_t prev = 0;
        for (size_t k = 0; k < D_OSC_SAMPLES; k++) {
            int32_t mag = buf[k] < 0 ? -buf[k] : buf[k];
            if (mag > peak) peak = mag;
            if (k > 0 && ((prev < 0) != (buf[k] < 0))) zero_crossings++;
            prev = buf[k];
        }

        char msg[80];
        snprintf(msg, sizeof(msg), "macro %s: peak > 500", k_macro_names[i]);
        LFE_TEST_ASSERT(peak > 500, msg);
        snprintf(msg, sizeof(msg), "macro %s: > 1 zero-crossing",
                 k_macro_names[i]);
        LFE_TEST_ASSERT(zero_crossings > 1, msg);

        if (getenv("LFE_BRAIDS_DUMP")) {
            char path[128];
            snprintf(path, sizeof(path),
                     "test/output/braids_macro_%s.wav", k_macro_names[i]);
            lfe_test_wav_write_mono16(path, buf, D_OSC_SAMPLES, 96000);
        }
    }
}

/* ---- Public API smoke tests ----
 *
 * Exercises the lfe_gen_braids() entry point end-to-end: generates
 * each shape at three rates (8/16/32 kHz) and verifies the rate-
 * conversion didn't squash everything to silence. Also a determinism
 * test: same seed → identical output.
 */

static void test_gen_braids_one(lfe_braids_shape shape, uint32_t rate,
                                const char *name)
{
    /* 0.25 s at the given rate. Stack-allocated; max is 32 kHz × 0.25 s
     * = 8000 s16 samples = 16 KB — fine. */
    int16_t buf[8000];
    uint32_t length = rate / 4;
    if (length > sizeof(buf) / sizeof(buf[0])) length = sizeof(buf)/sizeof(buf[0]);

    lfe_buffer outbuf = { .data = buf, .length = length, .rate = (lfe_rate)rate };
    lfe_braids_params p = {
        .shape       = shape,
        .pitch_hz_q8 = 440u << 8,    /* A4 */
        .timbre      = 16384,
        .color       = 16384,
        .seed        = 0x42,         /* deterministic */
    };

    lfe_status rc = lfe_gen_braids(&outbuf, &p);
    char msg[80];
    snprintf(msg, sizeof(msg), "gen_braids %s @ %u Hz returns OK or CLIPPED",
             name, (unsigned)rate);
    LFE_TEST_ASSERT(rc == LFE_OK || rc == LFE_WARN_CLIPPED, msg);

    int32_t peak = 0;
    for (uint32_t i = 0; i < length; i++) {
        int32_t mag = buf[i] < 0 ? -buf[i] : buf[i];
        if (mag > peak) peak = mag;
    }
    snprintf(msg, sizeof(msg), "gen_braids %s @ %u Hz: peak > 200",
             name, (unsigned)rate);
    /* Lower threshold than the macro test — averaging-based decimation
     * attenuates highs by ~3 dB at the lowest rates. */
    LFE_TEST_ASSERT(peak > 200, msg);
}

static void test_gen_braids_public_api(void)
{
    LFE_TEST_HEADER("braids: public lfe_gen_braids() at multiple rates");

    /* Spot-check: one easy shape (CSAW) at all three rates, plus
     * one rate (32 kHz) across all 18 shapes for breadth. */
    for (uint32_t r = 8000; r <= 32000; r *= 2) {
        test_gen_braids_one(LFE_BRAIDS_SHAPE_CSAW, r, "csaw");
    }
    for (int s = 0; s < LFE_BRAIDS_SHAPE_COUNT; s++) {
        test_gen_braids_one((lfe_braids_shape)s, 32000, k_macro_names[s]);
    }
}

static void test_gen_braids_determinism(void)
{
    LFE_TEST_HEADER("braids: same seed → identical output");

    int16_t buf_a[2000];
    int16_t buf_b[2000];
    lfe_buffer ba = { .data = buf_a, .length = 2000, .rate = LFE_RATE_8000 };
    lfe_buffer bb = { .data = buf_b, .length = 2000, .rate = LFE_RATE_8000 };
    lfe_braids_params p = {
        .shape       = LFE_BRAIDS_SHAPE_PLUCKED,  /* uses the RNG */
        .pitch_hz_q8 = 220u << 8,
        .timbre      = 16384,
        .color       = 16384,
        .seed        = 0xCAFEBABE,
    };

    lfe_gen_braids(&ba, &p);
    lfe_gen_braids(&bb, &p);

    LFE_TEST_ASSERT(memcmp(buf_a, buf_b, sizeof(buf_a)) == 0,
                    "two seeded renders produce byte-identical output");
}

/* ---- Entry point ---- */

void lfe_test_braids(void)
{
    test_braids_resources_sizes();
    test_braids_interp();
    test_braids_svf();
    test_braids_excitation();
    test_braids_analog_shapes();
    test_braids_digital_vowel_fof();
    test_braids_digital_twin_peaks_noise();
    test_braids_digital_modulation();
    test_braids_digital_plucked();
    test_braids_digital_waveguide(BRAIDS_DOSC_SHAPE_BOWED,  "bowed");
    test_braids_digital_waveguide(BRAIDS_DOSC_SHAPE_BLOWN,  "blown");
    test_braids_digital_waveguide(BRAIDS_DOSC_SHAPE_FLUTED, "fluted");
    test_braids_digital_waveguide(BRAIDS_DOSC_SHAPE_TRIPLE_RING_MOD,
                                  "triple_ring_mod");
    test_braids_digital_waveguide(BRAIDS_DOSC_SHAPE_SAW_SWARM,
                                  "saw_swarm");
    test_braids_digital_comb_filter();
    test_braids_macro_all_shapes();
    test_gen_braids_public_api();
    test_gen_braids_determinism();
}
