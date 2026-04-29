/*
 * test_synth.c — Tests for the subtractive synth generator (Phase 4).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Coverage:
 *   - parameter validation (NULL, unknown preset)
 *   - lfe_synth_fill_preset for every preset
 *   - determinism (same params → identical samples, twice)
 *   - amp envelope decays past note-off
 *   - free-form custom patch exercising OSC1_LEVEL / PWM mods
 *   - WAV dumps of each preset for ear-testing
 */

#include "test_main.h"

#include "lfe.h"
#include "util/fixed.h"
#include "util/wav.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SYNTH_RATE   32000u
#define SYNTH_LEN_MS 1000u
#define SYNTH_LEN    ((SYNTH_RATE * SYNTH_LEN_MS) / 1000u)

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

static void gen_and_check(const lfe_synth_params *p, int16_t *buf, uint32_t n,
                          const char *label)
{
    memset(buf, 0, n * sizeof(int16_t));
    lfe_buffer outbuf = { .data = buf, .length = n, .rate = LFE_RATE_32000 };
    lfe_status rc = lfe_gen_synth(&outbuf, p);
    LFE_TEST_ASSERT(rc >= 0, label);
}

/* ------------------------------------------------------------------ */

static void test_synth_param_validation(void)
{
    LFE_TEST_HEADER("synth param validation");

    int16_t buf[64];
    lfe_buffer outbuf = { .data = buf, .length = 64, .rate = LFE_RATE_32000 };
    lfe_synth_params p;
    lfe_synth_fill_preset(&p, LFE_SYNTH_PRESET_LEAD);

    LFE_TEST_ASSERT_EQ(lfe_gen_synth(NULL, &p),      LFE_ERR_NULL,
                       "NULL out → LFE_ERR_NULL");
    LFE_TEST_ASSERT_EQ(lfe_gen_synth(&outbuf, NULL), LFE_ERR_NULL,
                       "NULL params → LFE_ERR_NULL");

    lfe_buffer bad_data = { .data = NULL, .length = 64, .rate = LFE_RATE_32000 };
    LFE_TEST_ASSERT_EQ(lfe_gen_synth(&bad_data, &p), LFE_ERR_NULL,
                       "NULL out->data → LFE_ERR_NULL");

    lfe_synth_params dummy;
    LFE_TEST_ASSERT_EQ(lfe_synth_fill_preset(&dummy, (lfe_synth_preset)9999),
                       LFE_ERR_BAD_PARAM,
                       "unknown preset → LFE_ERR_BAD_PARAM");
    LFE_TEST_ASSERT_EQ(lfe_synth_fill_preset(NULL, LFE_SYNTH_PRESET_LEAD),
                       LFE_ERR_NULL,
                       "fill_preset NULL → LFE_ERR_NULL");
}

static void test_synth_fill_preset_all(void)
{
    LFE_TEST_HEADER("synth fill_preset for all presets");

    static const lfe_synth_preset presets[] = {
        LFE_SYNTH_PRESET_LEAD, LFE_SYNTH_PRESET_PAD,
        LFE_SYNTH_PRESET_PLUCK, LFE_SYNTH_PRESET_BASS,
    };
    for (size_t k = 0; k < sizeof(presets) / sizeof(presets[0]); k++) {
        lfe_synth_params p;
        lfe_status rc = lfe_synth_fill_preset(&p, presets[k]);
        LFE_TEST_ASSERT_EQ(rc, LFE_OK, "fill_preset OK");
        LFE_TEST_ASSERT(p.master_level > 0, "preset master_level > 0");

        int amp_ok = 0;
        for (int j = 0; j < LFE_SYNTH_NUM_MODS; j++) {
            if (p.mods[j].target == LFE_SYNTH_MOD_AMP) { amp_ok = 1; break; }
        }
        LFE_TEST_ASSERT(amp_ok, "preset has an AMP-target mod");
    }
}

static void test_synth_determinism(void)
{
    LFE_TEST_HEADER("synth determinism");

    lfe_synth_params p;
    lfe_synth_fill_preset(&p, LFE_SYNTH_PRESET_PLUCK);
    p.noise_seed = 0x5A5Au;

    int16_t *a = (int16_t *)calloc(SYNTH_LEN, sizeof(int16_t));
    int16_t *b = (int16_t *)calloc(SYNTH_LEN, sizeof(int16_t));
    if (!a || !b) { free(a); free(b); return; }

    gen_and_check(&p, a, SYNTH_LEN, "first gen OK");
    gen_and_check(&p, b, SYNTH_LEN, "second gen OK");

    LFE_TEST_ASSERT(memcmp(a, b, SYNTH_LEN * sizeof(int16_t)) == 0,
                    "same params → byte-identical output");

    free(a); free(b);
}

static void test_synth_note_off_release(void)
{
    LFE_TEST_HEADER("synth note-off triggers release");

    /* Use a patch with a long sustain and a clear release so that the
     * energy near the buffer end is dominated by the release decay. */
    lfe_synth_params p;
    lfe_synth_fill_preset(&p, LFE_SYNTH_PRESET_LEAD);
    p.note_off_sample = SYNTH_LEN / 2;  /* release halfway through */

    int16_t *buf = (int16_t *)calloc(SYNTH_LEN, sizeof(int16_t));
    if (!buf) return;
    gen_and_check(&p, buf, SYNTH_LEN, "lead gen OK");

    /* Window comparison: energy in the sustain segment vs energy in
     * the late-release segment. Release should be quieter. */
    uint32_t w = SYNTH_LEN / 8u;
    int64_t e_sustain = sum_abs(buf + (SYNTH_LEN / 2) - w, w);
    int64_t e_release = sum_abs(buf + SYNTH_LEN - w,       w);

    LFE_TEST_ASSERT(e_sustain > 0, "sustain has energy");
    LFE_TEST_ASSERT(e_sustain > e_release * 2,
                    "sustain ≫ late-release (envelope released)");

    free(buf);
}

static void test_synth_silent_with_zero_master(void)
{
    LFE_TEST_HEADER("synth silent when master = 0");

    lfe_synth_params p;
    lfe_synth_fill_preset(&p, LFE_SYNTH_PRESET_LEAD);
    p.master_level = 0;

    int16_t buf[1024];
    gen_and_check(&p, buf, 1024, "zero-master gen OK");
    LFE_TEST_ASSERT_EQ(abs_peak(buf, 1024), 0, "output is all zeros");
}

static void test_synth_custom_freeform(void)
{
    LFE_TEST_HEADER("synth free-form custom patch");

    /* Square wave with a PWM sweep and a separate OSC1 level fade-in.
     * Exercises PULSE_WIDTH and OSC1_LEVEL targets that no preset uses. */
    lfe_synth_params p = {
        .base_hz_q8 = 330u << 8,
        .osc1 = { .wave = LFE_SYNTH_WAVE_SQUARE, .detune_hz = 0,
                  .level = Q15_ONE / 2, .pulse_width = Q15_HALF },
        .osc2 = { .wave = LFE_SYNTH_WAVE_SINE,   .detune_hz = -5,
                  .level = Q15_ONE / 2, .pulse_width = Q15_HALF },
        .noise_level    = 0,
        .noise_seed     = 0,
        .filter_mode    = LFE_DRUM_FILTER_LP,
        .filter_base_hz = 4000,
        .filter_q       = Q15_HALF,
        .master_level   = Q15_ONE,
        .note_off_sample = (SYNTH_LEN * 3) / 4,
        .mods = {
            { .env = { .attack_ms = 10,  .decay_ms = 100, .sustain_level = Q15_ONE,
                       .release_ms = 150, .peak_level = Q15_ONE },
              .target = LFE_SYNTH_MOD_AMP, .depth = Q15_ONE },
            { .env = { .attack_ms = 300, .decay_ms = 0, .sustain_level = Q15_ONE,
                       .release_ms = 0, .peak_level = Q15_ONE },
              .target = LFE_SYNTH_MOD_OSC1_LEVEL, .depth = Q15_ONE / 2 },
            { .env = { .attack_ms = 500, .decay_ms = 300, .sustain_level = 0,
                       .release_ms = 0, .peak_level = Q15_ONE },
              .target = LFE_SYNTH_MOD_PULSE_WIDTH,
              .depth = -(Q15_HALF / 2) },   /* sweep pw from 50% toward 25% */
            { .env = { 0 }, .target = LFE_SYNTH_MOD_NONE, .depth = 0 },
        },
    };

    int16_t *buf = (int16_t *)calloc(SYNTH_LEN, sizeof(int16_t));
    if (!buf) return;
    gen_and_check(&p, buf, SYNTH_LEN, "freeform gen OK");

    int32_t peak = abs_peak(buf, SYNTH_LEN);
    LFE_TEST_ASSERT(peak > 4000, "freeform output has substantial energy");

    /* Slow (10 ms) attack on AMP: first sample should be near silent. */
    LFE_TEST_ASSERT(buf[0] > -2000 && buf[0] < 2000,
                    "first sample near zero (slow amp attack)");

    free(buf);
}

/* ------------------------------------------------------------------ */
/* Combine mode tests                                                  */
/* ------------------------------------------------------------------ */

/* Build a baseline patch for combine-mode tests: two saws, simple amp
 * envelope, LP filter wide open, no noise. Close to gen_synth's LEAD
 * preset but stripped down so the ONLY difference between the MIX and
 * the target combine mode is the `combine` field. */
static lfe_synth_params combine_test_params(void)
{
    lfe_synth_params p = {
        .base_hz_q8 = 220u << 8,
        .osc1 = { .wave = LFE_SYNTH_WAVE_SAW, .detune_hz =  0,
                  .level = Q15_ONE, .pulse_width = Q15_HALF },
        .osc2 = { .wave = LFE_SYNTH_WAVE_SAW, .detune_hz = +7,
                  .level = Q15_ONE, .pulse_width = Q15_HALF },
        .noise_level    = 0,
        .noise_seed     = 0,
        .filter_mode    = LFE_DRUM_FILTER_LP,
        .filter_base_hz = 8000,        /* wide open so filter doesn't mask mode differences */
        .filter_q       = Q15_HALF,
        .master_level   = Q15_ONE,
        .note_off_sample = 0,
        .mods = {
            { .env = { .attack_ms = 5, .decay_ms = 0, .sustain_level = Q15_ONE,
                       .release_ms = 0, .peak_level = Q15_ONE },
              .target = LFE_SYNTH_MOD_AMP, .depth = Q15_ONE },
            { .env = { 0 }, .target = LFE_SYNTH_MOD_NONE, .depth = 0 },
            { .env = { 0 }, .target = LFE_SYNTH_MOD_NONE, .depth = 0 },
            { .env = { 0 }, .target = LFE_SYNTH_MOD_NONE, .depth = 0 },
        },
        /* .combine / combine_param{1,2} zero-init → MIX */
    };
    return p;
}

static void test_synth_combine_hard_sync(void)
{
    LFE_TEST_HEADER("synth HARD_SYNC combine mode");

    /* Classic sync setup: slave (osc2) an octave above master (osc1).
     * The slave would naturally complete 2 cycles per master cycle, but
     * HARD_SYNC force-resets its phase when the master wraps, so the
     * slave never finishes its second cycle cleanly — that truncated
     * shape is the entire sonic payoff of sync. Override both oscs'
     * detune here instead of in combine_test_params so the RING_MOD /
     * CALVARIO tests (which prefer close frequencies for their own
     * sonic regions) aren't forced into this layout. */
    lfe_synth_params p_mix  = combine_test_params();
    lfe_synth_params p_sync = combine_test_params();
    p_mix.osc2.detune_hz  = +220;   /* 440 Hz, one octave above master */
    p_sync.osc2.detune_hz = +220;
    p_sync.combine = LFE_SYNTH_COMBINE_HARD_SYNC;

    int16_t *a = (int16_t *)calloc(SYNTH_LEN, sizeof(int16_t));
    int16_t *b = (int16_t *)calloc(SYNTH_LEN, sizeof(int16_t));
    if (!a || !b) { free(a); free(b); return; }

    gen_and_check(&p_mix,  a, SYNTH_LEN, "MIX baseline gen OK");
    gen_and_check(&p_sync, b, SYNTH_LEN, "HARD_SYNC gen OK");

    LFE_TEST_ASSERT(memcmp(a, b, SYNTH_LEN * sizeof(int16_t)) != 0,
                    "HARD_SYNC output differs from MIX");

    /* Sanity on the signal shape. We don't check `peak < 32000` because
     * an octave-sync saw legitimately touches the negative rail at each
     * sync point — both oscs are at phase=0 at that instant, and a saw
     * at phase=0 is -32768. That's the correct algorithm output, not
     * destabilization. Instead, verify the signal has substantial energy
     * on BOTH sides of zero — catches the degenerate case where HARD_SYNC
     * somehow pins the filter output to a single rail. */
    int32_t peak = abs_peak(b, SYNTH_LEN);
    LFE_TEST_ASSERT(peak > 4000, "HARD_SYNC has substantial energy");

    bool has_positive = false, has_negative = false;
    for (uint32_t i = 0; i < SYNTH_LEN; i++) {
        if (b[i] >  4000) has_positive = true;
        if (b[i] < -4000) has_negative = true;
        if (has_positive && has_negative) break;
    }
    LFE_TEST_ASSERT(has_positive && has_negative,
                    "HARD_SYNC output varies on both sides of zero");

    /* WAV dumps for ear-testing, one per mode. */
    lfe_test_wav_write_mono16("test/output/synth_combine_mix.wav",
                              a, SYNTH_LEN, SYNTH_RATE);
    lfe_test_wav_write_mono16("test/output/synth_combine_hard_sync.wav",
                              b, SYNTH_LEN, SYNTH_RATE);

    free(a); free(b);
}

static void test_synth_combine_fm(void)
{
    LFE_TEST_HEADER("synth FM combine mode");

    /* Classic FM bell layout: sines on both oscillators, inharmonic
     * carrier:modulator ratio, carrier audible at 200 Hz, modulator at
     * 700 Hz (ratio 1:3.5). At combine_param1 = Q15_ONE the FM depth
     * corresponds to modulation index ~4 — enough for recognizable bell
     * sidebands without going full DX7 scream. */
    lfe_synth_params p_mix = combine_test_params();
    p_mix.base_hz_q8 = 200u << 8;
    p_mix.osc1.wave = LFE_SYNTH_WAVE_SINE;   /* modulator */
    p_mix.osc1.detune_hz = +500;              /* 700 Hz — inharmonic */
    p_mix.osc2.wave = LFE_SYNTH_WAVE_SINE;   /* carrier */
    p_mix.osc2.detune_hz = 0;                 /* 200 Hz */
    /* Drop osc1 level — in a "pure" FM sound the modulator shouldn't
     * contribute to the audio mix, only to phase2. s1 is still used raw
     * (pre-level) for the phase offset computation. */
    p_mix.osc1.level = 0;
    p_mix.osc2.level = Q15_ONE;

    lfe_synth_params p_fm = p_mix;
    p_fm.combine = LFE_SYNTH_COMBINE_FM;
    p_fm.combine_param1 = Q15_ONE;   /* full FM depth → index ~4 */

    int16_t *a = (int16_t *)calloc(SYNTH_LEN, sizeof(int16_t));
    int16_t *b = (int16_t *)calloc(SYNTH_LEN, sizeof(int16_t));
    if (!a || !b) { free(a); free(b); return; }

    gen_and_check(&p_mix, a, SYNTH_LEN, "MIX baseline gen OK");
    gen_and_check(&p_fm,  b, SYNTH_LEN, "FM gen OK");

    LFE_TEST_ASSERT(memcmp(a, b, SYNTH_LEN * sizeof(int16_t)) != 0,
                    "FM output differs from MIX");

    int32_t peak = abs_peak(b, SYNTH_LEN);
    LFE_TEST_ASSERT(peak > 4000, "FM has substantial energy");

    bool has_positive = false, has_negative = false;
    for (uint32_t i = 0; i < SYNTH_LEN; i++) {
        if (b[i] >  4000) has_positive = true;
        if (b[i] < -4000) has_negative = true;
        if (has_positive && has_negative) break;
    }
    LFE_TEST_ASSERT(has_positive && has_negative,
                    "FM output varies on both sides of zero");

    lfe_test_wav_write_mono16("test/output/synth_combine_fm.wav",
                              b, SYNTH_LEN, SYNTH_RATE);

    free(a); free(b);
}

static void test_synth_combine_ring_mod(void)
{
    LFE_TEST_HEADER("synth RING_MOD combine mode");

    /* Classic ring mod layout: two sines, 200 × 500 Hz. Ring mod of
     * pure sines lands energy at the sum and difference frequencies
     * (200+500 = 700, 500-200 = 300) with NOTHING at the inputs — so
     * MIX at these freqs sounds like two pitched tones, RING_MOD like
     * a totally different chord. Very easy to ear-distinguish. */
    lfe_synth_params p_mix = combine_test_params();
    p_mix.base_hz_q8 = 200u << 8;
    p_mix.osc1.wave = LFE_SYNTH_WAVE_SINE;
    p_mix.osc1.detune_hz = 0;                 /* 200 Hz */
    p_mix.osc2.wave = LFE_SYNTH_WAVE_SINE;
    p_mix.osc2.detune_hz = +300;              /* 500 Hz */
    p_mix.osc1.level = Q15_ONE;
    p_mix.osc2.level = Q15_ONE;

    lfe_synth_params p_rm = p_mix;
    p_rm.combine = LFE_SYNTH_COMBINE_RING_MOD;

    int16_t *a = (int16_t *)calloc(SYNTH_LEN, sizeof(int16_t));
    int16_t *b = (int16_t *)calloc(SYNTH_LEN, sizeof(int16_t));
    if (!a || !b) { free(a); free(b); return; }

    gen_and_check(&p_mix, a, SYNTH_LEN, "MIX baseline gen OK");
    gen_and_check(&p_rm,  b, SYNTH_LEN, "RING_MOD gen OK");

    LFE_TEST_ASSERT(memcmp(a, b, SYNTH_LEN * sizeof(int16_t)) != 0,
                    "RING_MOD output differs from MIX");

    int32_t peak = abs_peak(b, SYNTH_LEN);
    LFE_TEST_ASSERT(peak > 4000, "RING_MOD has substantial energy");

    bool has_positive = false, has_negative = false;
    for (uint32_t i = 0; i < SYNTH_LEN; i++) {
        if (b[i] >  4000) has_positive = true;
        if (b[i] < -4000) has_negative = true;
        if (has_positive && has_negative) break;
    }
    LFE_TEST_ASSERT(has_positive && has_negative,
                    "RING_MOD output varies on both sides of zero");

    /* Silencing either osc via its level should silence the output.
     * This validates the "level zero → silence" property that falls
     * naturally out of applying per-osc levels before the multiply. */
    lfe_synth_params p_rm_silent = p_rm;
    p_rm_silent.osc1.level = 0;
    int16_t *s = (int16_t *)calloc(SYNTH_LEN, sizeof(int16_t));
    if (s) {
        gen_and_check(&p_rm_silent, s, SYNTH_LEN, "RING_MOD osc1 muted");
        LFE_TEST_ASSERT_EQ(abs_peak(s, SYNTH_LEN), 0,
                           "RING_MOD with osc1.level=0 is silent");
        free(s);
    }

    lfe_test_wav_write_mono16("test/output/synth_combine_ring_mod.wav",
                              b, SYNTH_LEN, SYNTH_RATE);

    free(a); free(b);
}

static void test_synth_combine_calvario(void)
{
    LFE_TEST_HEADER("synth CALVARIO combine mode");

    /* "Chorus" region: two saws at close frequencies, both gains at
     * 0 dB (unity, full XOR bit-window). The plan predicts a super-
     * saw-adjacent texture in this region, and at minimum we expect
     * the output to differ from a plain MIX of the same two saws. */
    lfe_synth_params p_mix = combine_test_params();
    p_mix.osc1.wave = LFE_SYNTH_WAVE_SAW;
    p_mix.osc2.wave = LFE_SYNTH_WAVE_SAW;
    p_mix.osc1.detune_hz = -2;
    p_mix.osc2.detune_hz = +2;
    p_mix.osc1.level = Q15_ONE;
    p_mix.osc2.level = Q15_ONE;

    lfe_synth_params p_cal = p_mix;
    p_cal.combine = LFE_SYNTH_COMBINE_CALVARIO;
    p_cal.combine_param1 = 0;   /* 0 dB → unity gain */
    p_cal.combine_param2 = 0;

    int16_t *a = (int16_t *)calloc(SYNTH_LEN, sizeof(int16_t));
    int16_t *b = (int16_t *)calloc(SYNTH_LEN, sizeof(int16_t));
    if (!a || !b) { free(a); free(b); return; }

    gen_and_check(&p_mix, a, SYNTH_LEN, "MIX baseline gen OK");
    gen_and_check(&p_cal, b, SYNTH_LEN, "CALVARIO 0 dB gen OK");

    LFE_TEST_ASSERT(memcmp(a, b, SYNTH_LEN * sizeof(int16_t)) != 0,
                    "CALVARIO output differs from MIX");

    int32_t peak = abs_peak(b, SYNTH_LEN);
    LFE_TEST_ASSERT(peak > 4000, "CALVARIO has substantial energy");

    bool has_positive = false, has_negative = false;
    for (uint32_t i = 0; i < SYNTH_LEN; i++) {
        if (b[i] >  4000) has_positive = true;
        if (b[i] < -4000) has_negative = true;
        if (has_positive && has_negative) break;
    }
    LFE_TEST_ASSERT(has_positive && has_negative,
                    "CALVARIO output varies on both sides of zero");

    /* Heavy attenuation: both gains at -40 dB. The XOR still happens
     * but on values with a much smaller bit window (top bits zeroed
     * out by the attenuation), so the output amplitude should shrink
     * and the texture should get subtler / quieter than the 0 dB case.
     * We assert the peak drops meaningfully — not bit-exactly, since
     * the XOR is nonlinear, but by a clear factor. */
    lfe_synth_params p_cal_quiet = p_cal;
    p_cal_quiet.combine_param1 = -(40 << 8);  /* -40 dB Q8.8 */
    p_cal_quiet.combine_param2 = -(40 << 8);

    int16_t *c = (int16_t *)calloc(SYNTH_LEN, sizeof(int16_t));
    if (c) {
        gen_and_check(&p_cal_quiet, c, SYNTH_LEN, "CALVARIO -40 dB gen OK");
        int32_t peak_quiet = abs_peak(c, SYNTH_LEN);
        LFE_TEST_ASSERT(peak_quiet * 4 < peak,
                        "CALVARIO at -40 dB is at least 4x quieter than 0 dB");
        free(c);
    }

    /* Asymmetric gains: one operand at 0 dB, one at -30 dB. Distinct
     * bit-window widths per operand — an interesting sonic region
     * the plan memo specifically called out. Just a smoke test that
     * the mode runs and produces non-silence here. */
    lfe_synth_params p_cal_asym = p_cal;
    p_cal_asym.combine_param1 = 0;
    p_cal_asym.combine_param2 = -(30 << 8);

    int16_t *d = (int16_t *)calloc(SYNTH_LEN, sizeof(int16_t));
    if (d) {
        gen_and_check(&p_cal_asym, d, SYNTH_LEN, "CALVARIO asym gen OK");
        LFE_TEST_ASSERT(abs_peak(d, SYNTH_LEN) > 1000,
                        "CALVARIO asymmetric has some energy");
        lfe_test_wav_write_mono16("test/output/synth_combine_calvario_asym.wav",
                                  d, SYNTH_LEN, SYNTH_RATE);
        free(d);
    }

    lfe_test_wav_write_mono16("test/output/synth_combine_calvario.wav",
                              b, SYNTH_LEN, SYNTH_RATE);

    free(a); free(b);
}

static void test_synth_combine_param_mod(void)
{
    LFE_TEST_HEADER("synth combine_param1 envelope mod");

    /* Validate the LFE_SYNTH_MOD_COMBINE_PARAM1 mod-target wiring by
     * running two identical FM patches — one with static depth=0, one
     * with an envelope ramping depth 0→Q15_ONE over a 500 ms attack —
     * and asserting that the outputs converge early (env≈0 for both)
     * but diverge late (static stays at 0, swept hits full FM). If the
     * mod-target case were silently dropped, late_diff would stay at
     * the same order as early_diff and the assertion would fail. */
    lfe_synth_params p_static = combine_test_params();
    p_static.base_hz_q8 = 200u << 8;
    p_static.osc1.wave = LFE_SYNTH_WAVE_SINE;
    p_static.osc1.detune_hz = +500;            /* modulator at 700 Hz */
    p_static.osc2.wave = LFE_SYNTH_WAVE_SINE;
    p_static.osc2.detune_hz = 0;               /* carrier at 200 Hz */
    p_static.osc1.level = 0;                   /* pure FM: mod inaudible */
    p_static.osc2.level = Q15_ONE;
    p_static.combine = LFE_SYNTH_COMBINE_FM;
    p_static.combine_param1 = 0;               /* no FM baseline */

    lfe_synth_params p_swept = p_static;
    /* Slot 0 is the AMP env from combine_test_params; slot 1 is free.
     * Use it for a slow ramp on COMBINE_PARAM1. Depth = Q15_ONE means
     * the mod accumulator adds up to Q15_ONE to combine_param1 at peak
     * envelope, which maps to modulation index ~4 (the full range). */
    p_swept.mods[1] = (lfe_synth_mod){
        .env = { .attack_ms = 500, .decay_ms = 0,
                 .sustain_level = Q15_ONE, .release_ms = 0,
                 .peak_level = Q15_ONE },
        .target = LFE_SYNTH_MOD_COMBINE_PARAM1,
        .depth  = Q15_ONE,
    };

    int16_t *a = (int16_t *)calloc(SYNTH_LEN, sizeof(int16_t));
    int16_t *b = (int16_t *)calloc(SYNTH_LEN, sizeof(int16_t));
    if (!a || !b) { free(a); free(b); return; }

    gen_and_check(&p_static, a, SYNTH_LEN, "static depth=0 gen OK");
    gen_and_check(&p_swept,  b, SYNTH_LEN, "swept depth gen OK");

    /* Sum of squared differences in an early and a late window. SSD
     * is cheap, doesn't need libm, and gives a clear magnitude signal
     * of "how different are these two buffers here". */
    uint32_t w = 100;
    int64_t early_diff = 0;
    for (uint32_t i = 0; i < w; i++) {
        int32_t d = (int32_t)a[i] - (int32_t)b[i];
        early_diff += (int64_t)d * d;
    }
    int64_t late_diff = 0;
    for (uint32_t i = SYNTH_LEN - w; i < SYNTH_LEN; i++) {
        int32_t d = (int32_t)a[i] - (int32_t)b[i];
        late_diff += (int64_t)d * d;
    }

    LFE_TEST_ASSERT(late_diff > 1000,
                    "swept FM has substantial late-window divergence");
    /* Late divergence should be at least 100× the early divergence.
     * If the mod target weren't wired to the hot loop, both windows
     * would be identical (diff≈0) and this would fail the ratio. */
    LFE_TEST_ASSERT(late_diff > early_diff * 100,
                    "envelope on COMBINE_PARAM1 produces time-varying FM");

    lfe_test_wav_write_mono16("test/output/synth_combine_fm_swept.wav",
                              b, SYNTH_LEN, SYNTH_RATE);

    free(a); free(b);
}

static void test_synth_wav_dump_presets(void)
{
    LFE_TEST_HEADER("synth WAV dumps");

    static const struct {
        lfe_synth_preset preset;
        const char      *path;
    } presets[] = {
        { LFE_SYNTH_PRESET_LEAD,  "test/output/synth_lead.wav"  },
        { LFE_SYNTH_PRESET_PAD,   "test/output/synth_pad.wav"   },
        { LFE_SYNTH_PRESET_PLUCK, "test/output/synth_pluck.wav" },
        { LFE_SYNTH_PRESET_BASS,  "test/output/synth_bass.wav"  },
    };

    for (size_t k = 0; k < sizeof(presets) / sizeof(presets[0]); k++) {
        lfe_synth_params p;
        lfe_synth_fill_preset(&p, presets[k].preset);

        int16_t *buf = (int16_t *)calloc(SYNTH_LEN, sizeof(int16_t));
        if (!buf) continue;

        lfe_buffer outbuf = {
            .data = buf, .length = SYNTH_LEN, .rate = LFE_RATE_32000,
        };
        lfe_status rc = lfe_gen_synth(&outbuf, &p);
        LFE_TEST_ASSERT(rc >= 0, "preset gen succeeded");

        int wav_rc = lfe_test_wav_write_mono16(presets[k].path, buf,
                                               SYNTH_LEN, SYNTH_RATE);
        LFE_TEST_ASSERT_EQ(wav_rc, 0, "preset WAV written");

        free(buf);
    }
}

/* ------------------------------------------------------------------ */

void lfe_test_synth(void)
{
    test_synth_param_validation();
    test_synth_fill_preset_all();
    test_synth_determinism();
    test_synth_note_off_release();
    test_synth_silent_with_zero_master();
    test_synth_custom_freeform();
    test_synth_combine_hard_sync();
    test_synth_combine_fm();
    test_synth_combine_ring_mod();
    test_synth_combine_calvario();
    test_synth_combine_param_mod();
    test_synth_wav_dump_presets();
}
