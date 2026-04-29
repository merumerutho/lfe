/*
 * test_fx.c — Tests for the Phase 5 FX (distortion/filter/delay/env).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The critical invariant for every effect: samples outside the
 * selection range must be byte-identical to the input. Every test
 * builds a "canary" buffer (a recognizable waveform filled across
 * the whole span), applies the effect to a middle slice, and then
 * memcmp's the head and tail regions against a saved reference copy.
 *
 * Beyond the boundary tests: each effect gets a small energy-change
 * sanity check and a WAV dump for ear-testing.
 */

#include "test_main.h"

#include "lfe.h"
#include "util/fixed.h"
#include "util/wav.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FX_RATE   32000u
#define FX_LEN    32000u   /* 1 second at 32 kHz */
#define FX_HEAD   (FX_LEN / 4u)
#define FX_TAIL   (FX_LEN - FX_HEAD)

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void fill_test_signal(int16_t *buf, uint32_t length)
{
    /* A 440 Hz sine at half amplitude — deterministic, easy to see in
     * WAV dumps, non-zero everywhere. */
    lfe_buffer outbuf = { .data = buf, .length = length, .rate = LFE_RATE_32000 };
    lfe_test_tone_params tp = { .freq_hz_q8 = 440u << 8, .amplitude_q15 = Q15_HALF };
    lfe_gen_test_tone(&outbuf, &tp);
}

static int64_t sum_abs(const int16_t *buf, uint32_t n)
{
    int64_t s = 0;
    for (uint32_t i = 0; i < n; i++) {
        s += buf[i] < 0 ? -(int64_t)buf[i] : (int64_t)buf[i];
    }
    return s;
}

/*
 * Core invariant check: after applying an effect to [FX_HEAD, FX_TAIL),
 * the first FX_HEAD samples and last FX_LEN-FX_TAIL samples must match
 * the pristine reference exactly.
 */
static void assert_boundaries_untouched(const int16_t *after,
                                        const int16_t *before,
                                        const char *label)
{
    int head_ok = memcmp(after, before, FX_HEAD * sizeof(int16_t)) == 0;
    int tail_ok = memcmp(after + FX_TAIL, before + FX_TAIL,
                         (FX_LEN - FX_TAIL) * sizeof(int16_t)) == 0;
    LFE_TEST_ASSERT(head_ok, "selection head is untouched");
    LFE_TEST_ASSERT(tail_ok, "selection tail is untouched");
    (void)label;
}

/* ------------------------------------------------------------------ */
/* Distortion                                                          */
/* ------------------------------------------------------------------ */

static void test_fx_distort_hard_clip(void)
{
    LFE_TEST_HEADER("fx distort hard clip");

    int16_t *before = (int16_t *)calloc(FX_LEN, sizeof(int16_t));
    int16_t *buf    = (int16_t *)calloc(FX_LEN, sizeof(int16_t));
    if (!before || !buf) { free(before); free(buf); return; }

    fill_test_signal(before, FX_LEN);
    memcpy(buf, before, FX_LEN * sizeof(int16_t));

    lfe_buffer outbuf = { .data = buf, .length = FX_LEN, .rate = LFE_RATE_32000 };
    lfe_fx_range range = { .start = FX_HEAD, .end = FX_TAIL };
    lfe_fx_distortion_params p = {
        .mode      = LFE_FX_DIST_HARD,
        .drive     = Q15_ONE * 3,           /* 3x drive → clips hard */
        .threshold = Q15_ONE / 2,
        .mix       = Q15_ONE,
        .bit_depth = 0,
    };
    lfe_status rc = lfe_fx_distort(&outbuf, &range, &p);
    LFE_TEST_ASSERT(rc >= 0, "hard distort returns OK (possibly warn-clipped)");

    assert_boundaries_untouched(buf, before, "hard clip");

    /* Interior should have been modified. */
    int changed = memcmp(buf + FX_HEAD, before + FX_HEAD,
                         (FX_TAIL - FX_HEAD) * sizeof(int16_t)) != 0;
    LFE_TEST_ASSERT(changed, "interior samples were modified");

    /* WAV dump for ear-testing. */
    lfe_test_wav_write_mono16("test/output/fx_distort_hard.wav", buf, FX_LEN, FX_RATE);

    free(before); free(buf);
}

static void test_fx_distort_soft_and_fold(void)
{
    LFE_TEST_HEADER("fx distort soft + fold");

    int16_t *before = (int16_t *)calloc(FX_LEN, sizeof(int16_t));
    int16_t *buf    = (int16_t *)calloc(FX_LEN, sizeof(int16_t));
    if (!before || !buf) { free(before); free(buf); return; }

    fill_test_signal(before, FX_LEN);

    /* Soft */
    memcpy(buf, before, FX_LEN * sizeof(int16_t));
    lfe_buffer outbuf = { .data = buf, .length = FX_LEN, .rate = LFE_RATE_32000 };
    lfe_fx_range range = { .start = FX_HEAD, .end = FX_TAIL };
    lfe_fx_distortion_params soft = {
        .mode = LFE_FX_DIST_SOFT, .drive = Q15_ONE * 4,
        .threshold = Q15_ONE, .mix = Q15_ONE, .bit_depth = 0,
    };
    LFE_TEST_ASSERT(lfe_fx_distort(&outbuf, &range, &soft) >= 0, "soft distort OK");
    assert_boundaries_untouched(buf, before, "soft");
    lfe_test_wav_write_mono16("test/output/fx_distort_soft.wav", buf, FX_LEN, FX_RATE);

    /* Fold */
    memcpy(buf, before, FX_LEN * sizeof(int16_t));
    lfe_fx_distortion_params fold = {
        .mode = LFE_FX_DIST_FOLD, .drive = Q15_ONE * 3,
        .threshold = Q15_ONE / 3, .mix = Q15_ONE, .bit_depth = 0,
    };
    LFE_TEST_ASSERT(lfe_fx_distort(&outbuf, &range, &fold) >= 0, "fold distort OK");
    assert_boundaries_untouched(buf, before, "fold");
    lfe_test_wav_write_mono16("test/output/fx_distort_fold.wav", buf, FX_LEN, FX_RATE);

    free(before); free(buf);
}

static void test_fx_distort_bitcrush(void)
{
    LFE_TEST_HEADER("fx distort bitcrush");

    int16_t *before = (int16_t *)calloc(FX_LEN, sizeof(int16_t));
    int16_t *buf    = (int16_t *)calloc(FX_LEN, sizeof(int16_t));
    if (!before || !buf) { free(before); free(buf); return; }

    fill_test_signal(before, FX_LEN);
    memcpy(buf, before, FX_LEN * sizeof(int16_t));

    lfe_buffer outbuf = { .data = buf, .length = FX_LEN, .rate = LFE_RATE_32000 };
    lfe_fx_range range = { .start = FX_HEAD, .end = FX_TAIL };
    lfe_fx_distortion_params p = {
        .mode = LFE_FX_DIST_BITCRUSH, .drive = Q15_ONE,
        .threshold = 0, .mix = Q15_ONE, .bit_depth = 4,
    };
    LFE_TEST_ASSERT(lfe_fx_distort(&outbuf, &range, &p) >= 0, "bitcrush OK");

    assert_boundaries_untouched(buf, before, "bitcrush");

    /* Bitcrush to 4 bits: every sample in the interior should have
     * its low 11 bits zero. */
    int all_quantized = 1;
    for (uint32_t i = FX_HEAD; i < FX_TAIL; i++) {
        if ((buf[i] & ((1 << 11) - 1)) != 0) { all_quantized = 0; break; }
    }
    LFE_TEST_ASSERT(all_quantized, "bitcrush low bits are zero");

    lfe_test_wav_write_mono16("test/output/fx_distort_bitcrush.wav",
                              buf, FX_LEN, FX_RATE);

    free(before); free(buf);
}

/* Bad-param rejection */
static void test_fx_distort_validation(void)
{
    LFE_TEST_HEADER("fx distort param validation");

    int16_t buf[64];
    lfe_buffer outbuf = { .data = buf, .length = 64, .rate = LFE_RATE_32000 };
    lfe_fx_range ok = { .start = 0, .end = 64 };
    lfe_fx_distortion_params p = {
        .mode = LFE_FX_DIST_HARD, .drive = Q15_ONE, .threshold = Q15_ONE,
        .mix = Q15_ONE, .bit_depth = 0,
    };
    LFE_TEST_ASSERT_EQ(lfe_fx_distort(NULL, &ok, &p), LFE_ERR_NULL,
                       "NULL buf → LFE_ERR_NULL");
    LFE_TEST_ASSERT_EQ(lfe_fx_distort(&outbuf, NULL, &p), LFE_ERR_NULL,
                       "NULL range → LFE_ERR_NULL");
    LFE_TEST_ASSERT_EQ(lfe_fx_distort(&outbuf, &ok, NULL), LFE_ERR_NULL,
                       "NULL params → LFE_ERR_NULL");

    lfe_fx_range bad = { .start = 0, .end = 128 };
    LFE_TEST_ASSERT_EQ(lfe_fx_distort(&outbuf, &bad, &p), LFE_ERR_BAD_PARAM,
                       "end > length → LFE_ERR_BAD_PARAM");

    lfe_fx_range empty = { .start = 32, .end = 32 };
    LFE_TEST_ASSERT_EQ(lfe_fx_distort(&outbuf, &empty, &p), LFE_OK,
                       "empty range → LFE_OK no-op");
}

/* ------------------------------------------------------------------ */
/* Filter                                                              */
/* ------------------------------------------------------------------ */

static void test_fx_filter_lp(void)
{
    LFE_TEST_HEADER("fx filter LP over selection");

    int16_t *before = (int16_t *)calloc(FX_LEN, sizeof(int16_t));
    int16_t *buf    = (int16_t *)calloc(FX_LEN, sizeof(int16_t));
    if (!before || !buf) { free(before); free(buf); return; }

    /* Use a bright signal (saw-ish tone) so a LP filter has something
     * to strip. Build it as noise modulated by the sine. */
    lfe_buffer outbuf = { .data = before, .length = FX_LEN, .rate = LFE_RATE_32000 };
    lfe_test_tone_params tp = { .freq_hz_q8 = 2000u << 8, .amplitude_q15 = Q15_HALF };
    lfe_gen_test_tone(&outbuf, &tp);

    memcpy(buf, before, FX_LEN * sizeof(int16_t));
    outbuf.data = buf;

    lfe_fx_range range = { .start = FX_HEAD, .end = FX_TAIL };
    lfe_fx_filter_params p = {
        .mode = LFE_DRUM_FILTER_LP, .cutoff_hz = 300,
        .q = Q15_ONE, .mix = Q15_ONE,
    };
    LFE_TEST_ASSERT(lfe_fx_filter(&outbuf, &range, &p) >= 0, "filter OK");

    assert_boundaries_untouched(buf, before, "filter");

    /* LP at 300 Hz against a 2 kHz input → interior energy drops. */
    int64_t e_before = sum_abs(before + FX_HEAD, FX_TAIL - FX_HEAD);
    int64_t e_after  = sum_abs(buf + FX_HEAD,    FX_TAIL - FX_HEAD);
    LFE_TEST_ASSERT(e_after < e_before / 2,
                    "LP filter attenuated the high-freq input");

    lfe_test_wav_write_mono16("test/output/fx_filter_lp.wav", buf, FX_LEN, FX_RATE);

    free(before); free(buf);
}

/* Bandpass keeps energy near the center freq and strips content far
 * above and below. Input has a bright 2 kHz tone; BP at 500 Hz should
 * cut most of it. Test just checks attenuation — exact magnitude
 * depends on Q. */
static void test_fx_filter_bp(void)
{
    LFE_TEST_HEADER("fx filter BP (biquad)");

    int16_t *before = (int16_t *)calloc(FX_LEN, sizeof(int16_t));
    int16_t *buf    = (int16_t *)calloc(FX_LEN, sizeof(int16_t));
    if (!before || !buf) { free(before); free(buf); return; }

    lfe_buffer outbuf = { .data = before, .length = FX_LEN, .rate = LFE_RATE_32000 };
    lfe_test_tone_params tp = { .freq_hz_q8 = 2000u << 8, .amplitude_q15 = Q15_HALF };
    lfe_gen_test_tone(&outbuf, &tp);

    memcpy(buf, before, FX_LEN * sizeof(int16_t));
    outbuf.data = buf;

    lfe_fx_range range = { .start = FX_HEAD, .end = FX_TAIL };
    lfe_fx_filter_params p = {
        .mode = LFE_DRUM_FILTER_BP, .cutoff_hz = 500,
        .q = Q15_ONE / 2, .mix = Q15_ONE,
    };
    LFE_TEST_ASSERT(lfe_fx_filter(&outbuf, &range, &p) >= 0, "BP filter OK");

    assert_boundaries_untouched(buf, before, "BP");

    int64_t e_before = sum_abs(before + FX_HEAD, FX_TAIL - FX_HEAD);
    int64_t e_after  = sum_abs(buf + FX_HEAD,    FX_TAIL - FX_HEAD);
    LFE_TEST_ASSERT(e_after < e_before / 2,
                    "BP filter attenuates 2 kHz content when centered at 500 Hz");

    lfe_test_wav_write_mono16("test/output/fx_filter_bp.wav", buf, FX_LEN, FX_RATE);
    free(before); free(buf);
}

/* Notch nulls content at the center freq, passes everything else. With
 * a pure 2 kHz tone and notch centered at 2 kHz, interior energy should
 * drop dramatically. */
static void test_fx_filter_notch(void)
{
    LFE_TEST_HEADER("fx filter Notch (biquad)");

    int16_t *before = (int16_t *)calloc(FX_LEN, sizeof(int16_t));
    int16_t *buf    = (int16_t *)calloc(FX_LEN, sizeof(int16_t));
    if (!before || !buf) { free(before); free(buf); return; }

    lfe_buffer outbuf = { .data = before, .length = FX_LEN, .rate = LFE_RATE_32000 };
    lfe_test_tone_params tp = { .freq_hz_q8 = 2000u << 8, .amplitude_q15 = Q15_HALF };
    lfe_gen_test_tone(&outbuf, &tp);

    memcpy(buf, before, FX_LEN * sizeof(int16_t));
    outbuf.data = buf;

    lfe_fx_range range = { .start = FX_HEAD, .end = FX_TAIL };
    lfe_fx_filter_params p = {
        .mode = LFE_DRUM_FILTER_NOTCH, .cutoff_hz = 2000,
        .q = Q15_ONE / 4, .mix = Q15_ONE,
    };
    LFE_TEST_ASSERT(lfe_fx_filter(&outbuf, &range, &p) >= 0, "Notch filter OK");

    assert_boundaries_untouched(buf, before, "Notch");

    int64_t e_before = sum_abs(before + FX_HEAD, FX_TAIL - FX_HEAD);
    int64_t e_after  = sum_abs(buf + FX_HEAD,    FX_TAIL - FX_HEAD);
    LFE_TEST_ASSERT(e_after < e_before / 2,
                    "Notch filter removes content at its center frequency");

    lfe_test_wav_write_mono16("test/output/fx_filter_notch.wav", buf, FX_LEN, FX_RATE);
    free(before); free(buf);
}

/* ------------------------------------------------------------------ */
/* Delay                                                               */
/* ------------------------------------------------------------------ */

static void test_fx_delay_basic(void)
{
    LFE_TEST_HEADER("fx delay basic with feedback");

    int16_t *before = (int16_t *)calloc(FX_LEN, sizeof(int16_t));
    int16_t *buf    = (int16_t *)calloc(FX_LEN, sizeof(int16_t));
    if (!before || !buf) { free(before); free(buf); return; }

    /* Short percussive burst at the start of the selection so echoes
     * are visually obvious in the WAV. Silence everywhere else. */
    fill_test_signal(before, FX_LEN);
    /* Silence outside the selection and the last 3/4 of the selection. */
    memset(before, 0, FX_HEAD * sizeof(int16_t));
    memset(before + FX_TAIL, 0, (FX_LEN - FX_TAIL) * sizeof(int16_t));
    /* Keep only the first 1/8 of the selection as the "attack". */
    uint32_t burst_end = FX_HEAD + (FX_TAIL - FX_HEAD) / 8u;
    memset(before + burst_end, 0, (FX_TAIL - burst_end) * sizeof(int16_t));

    memcpy(buf, before, FX_LEN * sizeof(int16_t));

    /* Scratch for a 120 ms delay at 32 kHz = 3840 samples. */
    uint32_t delay_ms = 120;
    uint32_t scratch_len = (delay_ms * FX_RATE) / 1000u + 16;
    int16_t *scratch = (int16_t *)calloc(scratch_len, sizeof(int16_t));
    if (!scratch) { free(before); free(buf); return; }

    lfe_buffer outbuf = { .data = buf, .length = FX_LEN, .rate = LFE_RATE_32000 };
    lfe_fx_range range = { .start = FX_HEAD, .end = FX_TAIL };
    lfe_fx_delay_params p = {
        .delay_ms = delay_ms,
        .feedback = (Q15_ONE * 3) / 5,    /* 0.6 */
        .mix      = Q15_HALF,
        .scratch  = scratch,
        .scratch_length = scratch_len,
    };
    LFE_TEST_ASSERT(lfe_fx_delay(&outbuf, &range, &p) >= 0, "delay OK");

    assert_boundaries_untouched(buf, before, "delay");

    /* The echoes should produce energy later in the selection where
     * the original signal was silent. */
    uint32_t echo_region_start = burst_end;
    uint32_t echo_region_end   = FX_TAIL;
    int64_t e = sum_abs(buf + echo_region_start,
                        echo_region_end - echo_region_start);
    LFE_TEST_ASSERT(e > 0, "delay produced echoes past the burst");

    lfe_test_wav_write_mono16("test/output/fx_delay.wav", buf, FX_LEN, FX_RATE);

    free(scratch); free(before); free(buf);
}

static void test_fx_delay_validation(void)
{
    LFE_TEST_HEADER("fx delay validation");

    int16_t buf[256];
    int16_t scratch[64];
    lfe_buffer outbuf = { .data = buf, .length = 256, .rate = LFE_RATE_32000 };
    lfe_fx_range range = { .start = 0, .end = 256 };
    lfe_fx_delay_params p = {
        .delay_ms = 100,   /* 3200 samples — scratch too small */
        .feedback = 0, .mix = Q15_HALF,
        .scratch = scratch, .scratch_length = 64,
    };
    LFE_TEST_ASSERT_EQ(lfe_fx_delay(&outbuf, &range, &p), LFE_ERR_BUF_TOO_SMALL,
                       "undersized scratch → LFE_ERR_BUF_TOO_SMALL");

    p.delay_ms = 0;
    LFE_TEST_ASSERT_EQ(lfe_fx_delay(&outbuf, &range, &p), LFE_ERR_BAD_PARAM,
                       "zero delay → LFE_ERR_BAD_PARAM");
}

/* ------------------------------------------------------------------ */
/* Envelope shaper                                                     */
/* ------------------------------------------------------------------ */

static void test_fx_env_presets(void)
{
    LFE_TEST_HEADER("fx env_shaper fill_preset all shapes");

    uint16_t canvas[64];

    /* Fade-in: first ~0, last ~Q15_ONE */
    lfe_fx_env_fill_preset(canvas, 64, LFE_FX_ENV_PRESET_FADE_IN);
    LFE_TEST_ASSERT(canvas[0] <= 64, "fade-in starts near 0");
    LFE_TEST_ASSERT(canvas[63] >= Q15_ONE - 64, "fade-in ends near Q15_ONE");

    /* Fade-out: first ~Q15_ONE, last ~0 */
    lfe_fx_env_fill_preset(canvas, 64, LFE_FX_ENV_PRESET_FADE_OUT);
    LFE_TEST_ASSERT(canvas[0] >= Q15_ONE - 64, "fade-out starts near Q15_ONE");
    LFE_TEST_ASSERT(canvas[63] <= 64, "fade-out ends near 0");

    /* Exp decay: peak at start, zero at end */
    lfe_fx_env_fill_preset(canvas, 64, LFE_FX_ENV_PRESET_EXP_DECAY);
    LFE_TEST_ASSERT(canvas[0] == Q15_ONE, "exp decay starts at Q15_ONE");
    LFE_TEST_ASSERT(canvas[63] < 256, "exp decay ends near 0");

    /* Exp attack: mirror */
    lfe_fx_env_fill_preset(canvas, 64, LFE_FX_ENV_PRESET_EXP_ATTACK);
    LFE_TEST_ASSERT(canvas[0] == 0, "exp attack starts at 0");
    LFE_TEST_ASSERT(canvas[63] >= Q15_ONE - 16, "exp attack ends near Q15_ONE");

    /* Bell: peak in the middle */
    lfe_fx_env_fill_preset(canvas, 64, LFE_FX_ENV_PRESET_BELL);
    LFE_TEST_ASSERT(canvas[32] >= Q15_ONE - 64, "bell peaks near the middle");
    LFE_TEST_ASSERT(canvas[0] < 1024,  "bell starts near zero");
    LFE_TEST_ASSERT(canvas[63] < 1024, "bell ends near zero");
}

static void test_fx_env_applied(void)
{
    LFE_TEST_HEADER("fx env_shaper apply over selection");

    int16_t *before = (int16_t *)calloc(FX_LEN, sizeof(int16_t));
    int16_t *buf    = (int16_t *)calloc(FX_LEN, sizeof(int16_t));
    if (!before || !buf) { free(before); free(buf); return; }

    fill_test_signal(before, FX_LEN);
    memcpy(buf, before, FX_LEN * sizeof(int16_t));

    uint16_t canvas[128];
    lfe_fx_env_fill_preset(canvas, 128, LFE_FX_ENV_PRESET_EXP_DECAY);

    lfe_buffer outbuf = { .data = buf, .length = FX_LEN, .rate = LFE_RATE_32000 };
    lfe_fx_range range = { .start = FX_HEAD, .end = FX_TAIL };
    lfe_fx_env_shaper_params p = {
        .canvas = canvas, .canvas_length = 128, .mix = Q15_ONE,
    };
    LFE_TEST_ASSERT(lfe_fx_env_shaper(&outbuf, &range, &p) >= 0, "env shaper OK");

    assert_boundaries_untouched(buf, before, "env shaper");

    /* Exp decay: energy near selection start ≫ energy near selection end. */
    uint32_t w = (FX_TAIL - FX_HEAD) / 8u;
    int64_t e_head = sum_abs(buf + FX_HEAD, w);
    int64_t e_tail = sum_abs(buf + FX_TAIL - w, w);
    LFE_TEST_ASSERT(e_head > e_tail * 4,
                    "exp decay envelope: head ≫ tail");

    lfe_test_wav_write_mono16("test/output/fx_env_expdecay.wav", buf, FX_LEN, FX_RATE);

    free(before); free(buf);
}

/* ------------------------------------------------------------------ */
/* Normalize                                                           */
/* ------------------------------------------------------------------ */

static void test_fx_normalize_dc_and_peak(void)
{
    LFE_TEST_HEADER("fx normalize: DC removal + peak scaling");

    /* Build a signal with known DC offset + attenuated amplitude.
     * Base: 440 Hz sine at Q15_HALF amplitude. We'll shift the whole
     * buffer by +4000 (DC) and halve it again before processing, so
     * the effect has real work to do. */
    int16_t *buf    = malloc(FX_LEN * sizeof(int16_t));
    int16_t *before = malloc(FX_LEN * sizeof(int16_t));
    fill_test_signal(buf, FX_LEN);
    for (uint32_t i = FX_HEAD; i < FX_TAIL; i++) {
        int32_t v = ((int32_t)buf[i] >> 2) + 4000;   /* quarter-amp + DC */
        buf[i] = (int16_t)v;
    }
    memcpy(before, buf, FX_LEN * sizeof(int16_t));

    lfe_buffer outbuf = { .data = buf, .length = FX_LEN, .rate = LFE_RATE_32000 };
    lfe_fx_range range = { .start = FX_HEAD, .end = FX_TAIL };
    lfe_fx_normalize_params np = { .target_peak = Q15_ONE };

    lfe_status rc = lfe_fx_normalize(&outbuf, &range, &np);
    LFE_TEST_ASSERT(rc == LFE_OK || rc == LFE_WARN_CLIPPED,
                    "normalize: returns OK or CLIPPED");

    /* Selection mean should be ~0 after DC removal. The check happens
     * AFTER the gain stage, so a ½-LSB rounding residual in the
     * computed DC gets multiplied by the normalization gain
     * (~8× here, Q15_ONE / peak_4k). Tolerance accounts for this. */
    int64_t sum = 0;
    for (uint32_t i = FX_HEAD; i < FX_TAIL; i++) sum += buf[i];
    int64_t avg = sum / (int64_t)(FX_TAIL - FX_HEAD);
    LFE_TEST_ASSERT(avg >= -20 && avg <= 20,
                    "normalize: post-DC-remove mean is near zero");

    /* Peak magnitude should be ~Q15_ONE. Allow 2-LSB slack for
     * rounding in the gain multiply. */
    int32_t peak = 0;
    for (uint32_t i = FX_HEAD; i < FX_TAIL; i++) {
        int32_t mag = buf[i] < 0 ? -buf[i] : buf[i];
        if (mag > peak) peak = mag;
    }
    LFE_TEST_ASSERT(peak >= Q15_ONE - 2 && peak <= Q15_ONE,
                    "normalize: peak reaches target_peak");

    assert_boundaries_untouched(buf, before, "normalize");

    lfe_test_wav_write_mono16("test/output/fx_normalize.wav",
                              buf, FX_LEN, FX_RATE);
    free(before); free(buf);
}

static void test_fx_normalize_silent_is_noop(void)
{
    LFE_TEST_HEADER("fx normalize: silent selection is a no-op");

    int16_t *buf = calloc(FX_LEN, sizeof(int16_t));
    lfe_buffer outbuf = { .data = buf, .length = FX_LEN, .rate = LFE_RATE_32000 };
    lfe_fx_range range = { .start = 0, .end = FX_LEN };
    lfe_fx_normalize_params np = { .target_peak = Q15_ONE };

    lfe_status rc = lfe_fx_normalize(&outbuf, &range, &np);
    LFE_TEST_ASSERT_EQ(rc, LFE_OK, "silent normalize returns OK");

    for (uint32_t i = 0; i < FX_LEN; i++) {
        if (buf[i] != 0) {
            g_lfe_test_failed++;
            fprintf(stderr, "FAIL: silent buffer touched at i=%u (%d)\n",
                    i, buf[i]);
            free(buf);
            return;
        }
    }
    g_lfe_test_passed++;
    free(buf);
}

static void test_fx_normalize_validation(void)
{
    LFE_TEST_HEADER("fx normalize: validation");

    int16_t buf_data[16] = {0};
    lfe_buffer b = { .data = buf_data, .length = 16, .rate = LFE_RATE_32000 };
    lfe_fx_range r = { .start = 0, .end = 16 };
    lfe_fx_normalize_params p = { .target_peak = Q15_ONE };

    LFE_TEST_ASSERT_EQ(lfe_fx_normalize(NULL, &r, &p),
                       LFE_ERR_NULL, "NULL buf");
    LFE_TEST_ASSERT_EQ(lfe_fx_normalize(&b, NULL, &p),
                       LFE_ERR_NULL, "NULL range");
    LFE_TEST_ASSERT_EQ(lfe_fx_normalize(&b, &r, NULL),
                       LFE_ERR_NULL, "NULL params");

    lfe_fx_range bad = { .start = 0, .end = 17 };
    LFE_TEST_ASSERT_EQ(lfe_fx_normalize(&b, &bad, &p),
                       LFE_ERR_BAD_PARAM, "range.end > length");
}

/* ------------------------------------------------------------------ */
/* REVERSE                                                             */
/* ------------------------------------------------------------------ */

static void test_fx_reverse_basic(void)
{
    int16_t buf[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    lfe_buffer b = { .data = buf, .length = 8, .rate = LFE_RATE_32000 };
    lfe_fx_range r = { .start = 0, .end = 8 };

    lfe_status rc = lfe_fx_reverse(&b, &r);
    LFE_TEST_ASSERT_EQ(rc, LFE_OK, "reverse full OK");
    LFE_TEST_ASSERT_EQ(buf[0], 8, "first sample");
    LFE_TEST_ASSERT_EQ(buf[7], 1, "last sample");
    LFE_TEST_ASSERT_EQ(buf[3], 5, "middle sample");
}

static void test_fx_reverse_subrange(void)
{
    int16_t buf[8] = { 10, 20, 30, 40, 50, 60, 70, 80 };
    lfe_buffer b = { .data = buf, .length = 8, .rate = LFE_RATE_32000 };
    lfe_fx_range r = { .start = 2, .end = 6 };

    lfe_fx_reverse(&b, &r);
    LFE_TEST_ASSERT_EQ(buf[0], 10, "head untouched");
    LFE_TEST_ASSERT_EQ(buf[1], 20, "head untouched");
    LFE_TEST_ASSERT_EQ(buf[2], 50, "reversed [2]");
    LFE_TEST_ASSERT_EQ(buf[3], 40, "reversed [3]");
    LFE_TEST_ASSERT_EQ(buf[4], 30, "reversed [4]");
    LFE_TEST_ASSERT_EQ(buf[5], 60, "tail boundary");
    LFE_TEST_ASSERT_EQ(buf[6], 70, "tail untouched");
    LFE_TEST_ASSERT_EQ(buf[7], 80, "tail untouched");
}

static void test_fx_reverse_double(void)
{
    int16_t orig[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    int16_t buf[8];
    memcpy(buf, orig, sizeof(buf));
    lfe_buffer b = { .data = buf, .length = 8, .rate = LFE_RATE_32000 };
    lfe_fx_range r = { .start = 0, .end = 8 };

    lfe_fx_reverse(&b, &r);
    lfe_fx_reverse(&b, &r);
    LFE_TEST_ASSERT(memcmp(buf, orig, sizeof(buf)) == 0,
                    "double reverse restores original");
}

static void test_fx_reverse_validation(void)
{
    int16_t buf[4] = { 0 };
    lfe_buffer b = { .data = buf, .length = 4, .rate = LFE_RATE_32000 };
    lfe_fx_range r = { .start = 0, .end = 4 };

    LFE_TEST_ASSERT_EQ(lfe_fx_reverse(NULL, &r), LFE_ERR_NULL, "NULL buf");
    LFE_TEST_ASSERT_EQ(lfe_fx_reverse(&b, NULL), LFE_ERR_NULL, "NULL range");

    lfe_fx_range bad = { .start = 0, .end = 10 };
    LFE_TEST_ASSERT_EQ(lfe_fx_reverse(&b, &bad), LFE_ERR_BAD_PARAM, "end > length");

    lfe_fx_range empty = { .start = 2, .end = 2 };
    LFE_TEST_ASSERT_EQ(lfe_fx_reverse(&b, &empty), LFE_OK, "empty range is noop");
}

/* ------------------------------------------------------------------ */
/* BITCRUSH                                                            */
/* ------------------------------------------------------------------ */

static void test_fx_bitcrush_quantizes(void)
{
    int16_t *buf = (int16_t *)calloc(FX_LEN, sizeof(int16_t));
    LFE_TEST_ASSERT(buf != NULL, "alloc");
    for (uint32_t i = 0; i < FX_LEN; i++)
        buf[i] = (int16_t)(((int32_t)i * 65534 / (int32_t)FX_LEN) - 32767);

    lfe_buffer b = { .data = buf, .length = FX_LEN, .rate = LFE_RATE_32000 };
    lfe_fx_range r = { .start = 0, .end = FX_LEN };
    lfe_fx_bitcrush_params p = {
        .bit_depth = 4, .rate_div = 1, .dither = 0, .mix = Q15_ONE
    };

    lfe_status rc = lfe_fx_bitcrush(&b, &r, &p);
    LFE_TEST_ASSERT(rc >= 0, "bitcrush OK");

    /* 4-bit depth: values should be multiples of 2^(15-4) = 2048. */
    int non_quantized = 0;
    for (uint32_t i = 0; i < FX_LEN; i++) {
        int16_t v = buf[i];
        if (v > -32768 && v < 32767 && (v & 0x7FF) != 0)
            non_quantized++;
    }
    LFE_TEST_ASSERT(non_quantized == 0, "all samples quantized to 4-bit grid");
    free(buf);
}

static void test_fx_bitcrush_rate_reduction(void)
{
    int16_t buf[64];
    for (int i = 0; i < 64; i++) buf[i] = (int16_t)(i * 100);

    lfe_buffer b = { .data = buf, .length = 64, .rate = LFE_RATE_32000 };
    lfe_fx_range r = { .start = 0, .end = 64 };
    lfe_fx_bitcrush_params p = {
        .bit_depth = 15, .rate_div = 4, .dither = 0, .mix = Q15_ONE
    };

    lfe_fx_bitcrush(&b, &r, &p);

    /* With rate_div=4 and bit_depth=15 (no quantization), every group
     * of 4 samples should hold the value of the first in the group. */
    for (int g = 0; g < 16; g++) {
        int16_t expected = buf[g * 4];
        for (int j = 1; j < 4; j++) {
            LFE_TEST_ASSERT_EQ(buf[g * 4 + j], expected,
                               "held sample within group");
        }
    }
}

static void test_fx_bitcrush_dither(void)
{
    /* With dithering on, quantized output should differ from
     * non-dithered output (different noise floor shape). */
    int16_t *nodith = (int16_t *)calloc(FX_LEN, sizeof(int16_t));
    int16_t *dith   = (int16_t *)calloc(FX_LEN, sizeof(int16_t));
    LFE_TEST_ASSERT(nodith && dith, "alloc");

    for (uint32_t i = 0; i < FX_LEN; i++) {
        int16_t v = (int16_t)(((int32_t)i * 65534 / (int32_t)FX_LEN) - 32767);
        nodith[i] = v;
        dith[i]   = v;
    }

    lfe_buffer b1 = { .data = nodith, .length = FX_LEN, .rate = LFE_RATE_32000 };
    lfe_buffer b2 = { .data = dith,   .length = FX_LEN, .rate = LFE_RATE_32000 };
    lfe_fx_range r = { .start = 0, .end = FX_LEN };

    lfe_fx_bitcrush_params p_no = { .bit_depth = 4, .rate_div = 1, .dither = 0, .mix = Q15_ONE };
    lfe_fx_bitcrush_params p_di = { .bit_depth = 4, .rate_div = 1, .dither = 1, .mix = Q15_ONE };

    lfe_fx_bitcrush(&b1, &r, &p_no);
    lfe_fx_bitcrush(&b2, &r, &p_di);

    int differ = 0;
    for (uint32_t i = 0; i < FX_LEN; i++)
        if (nodith[i] != dith[i]) differ++;

    LFE_TEST_ASSERT(differ > (int)(FX_LEN / 4),
                    "dithered output differs from undithered");
    free(nodith);
    free(dith);
}

static void test_fx_bitcrush_boundaries(void)
{
    int16_t *before = (int16_t *)calloc(FX_LEN, sizeof(int16_t));
    int16_t *buf    = (int16_t *)calloc(FX_LEN, sizeof(int16_t));
    LFE_TEST_ASSERT(before && buf, "alloc");

    for (uint32_t i = 0; i < FX_LEN; i++) {
        int16_t v = (int16_t)(i & 0x7FFF);
        before[i] = v;
        buf[i]    = v;
    }

    lfe_buffer b = { .data = buf, .length = FX_LEN, .rate = LFE_RATE_32000 };
    lfe_fx_range r = { .start = FX_HEAD, .end = FX_TAIL };
    lfe_fx_bitcrush_params p = {
        .bit_depth = 4, .rate_div = 2, .dither = 1, .mix = Q15_ONE
    };

    lfe_fx_bitcrush(&b, &r, &p);

    LFE_TEST_ASSERT(memcmp(buf, before, FX_HEAD * 2) == 0,
                    "head untouched");
    LFE_TEST_ASSERT(memcmp(buf + FX_TAIL, before + FX_TAIL,
                           (FX_LEN - FX_TAIL) * 2) == 0,
                    "tail untouched");
    free(before);
    free(buf);
}

static void test_fx_bitcrush_validation(void)
{
    int16_t buf[4] = { 0 };
    lfe_buffer b = { .data = buf, .length = 4, .rate = LFE_RATE_32000 };
    lfe_fx_range r = { .start = 0, .end = 4 };
    lfe_fx_bitcrush_params p = { .bit_depth = 8, .rate_div = 1, .dither = 0, .mix = Q15_ONE };

    LFE_TEST_ASSERT_EQ(lfe_fx_bitcrush(NULL, &r, &p), LFE_ERR_NULL, "NULL buf");
    LFE_TEST_ASSERT_EQ(lfe_fx_bitcrush(&b, NULL, &p), LFE_ERR_NULL, "NULL range");
    LFE_TEST_ASSERT_EQ(lfe_fx_bitcrush(&b, &r, NULL), LFE_ERR_NULL, "NULL params");
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

void lfe_test_fx(void)
{
    test_fx_distort_hard_clip();
    test_fx_distort_soft_and_fold();
    test_fx_distort_bitcrush();
    test_fx_distort_validation();
    test_fx_filter_lp();
    test_fx_filter_bp();
    test_fx_filter_notch();
    test_fx_delay_basic();
    test_fx_delay_validation();
    test_fx_env_presets();
    test_fx_env_applied();
    test_fx_normalize_dc_and_peak();
    test_fx_normalize_silent_is_noop();
    test_fx_normalize_validation();
    test_fx_reverse_basic();
    test_fx_reverse_subrange();
    test_fx_reverse_double();
    test_fx_reverse_validation();
    test_fx_bitcrush_quantizes();
    test_fx_bitcrush_rate_reduction();
    test_fx_bitcrush_dither();
    test_fx_bitcrush_boundaries();
    test_fx_bitcrush_validation();
}
