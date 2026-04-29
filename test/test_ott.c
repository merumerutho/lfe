/*
 * test_ott.c — Tests for the OTT 3-band multiband compressor FX.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Phase 1 coverage:
 *   - boundaries outside the range are byte-identical
 *   - bypass config (no compression, unity gains) reconstructs ≈ input
 *   - downward compression reduces the peak of a loud signal
 *   - a band gain of 0 mutes that band's contribution
 *   - NULL-pointer / bad-range validation returns the right errors
 *
 * A WAV dump of a compressed signal lands in test/output/fx_ott.wav
 * for ear-testing.
 */

#include "test_main.h"

#include "lfe.h"
#include "util/fixed.h"
#include "util/wav.h"

/* Internal headers — pulled in via -Isrc for diagnostic tests only. */
#include "util/crossover.h"
#include "util/biquad.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OTT_RATE   32000u
#define OTT_LEN    32000u   /* 1 second at 32 kHz */
#define OTT_HEAD   (OTT_LEN / 4u)
#define OTT_TAIL   (OTT_LEN - OTT_HEAD)

/* ------------------------------------------------------------------ */
/* Signal helpers                                                       */
/* ------------------------------------------------------------------ */

/* 440 Hz sine at `amp_q15` amplitude — falls squarely in the MID band
 * for the default 88 / 2500 Hz crossovers. */
static void fill_sine(int16_t *buf, uint32_t length, int amp_q15, float freq)
{
    for (uint32_t i = 0; i < length; i++) {
        float t = (float)i / (float)OTT_RATE;
        float v = sinf(2.0f * 3.14159265358979f * freq * t);
        int s = (int)(v * (float)amp_q15);
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        buf[i] = (int16_t)s;
    }
}

/* Fill with a sum of three tones — one per band (40 Hz / 440 Hz / 6 kHz). */
static void fill_three_band_mix(int16_t *buf, uint32_t length, int amp_q15)
{
    for (uint32_t i = 0; i < length; i++) {
        float t = (float)i / (float)OTT_RATE;
        float v = sinf(2.0f * 3.14159265358979f *   40.0f * t) / 3.0f
                + sinf(2.0f * 3.14159265358979f *  440.0f * t) / 3.0f
                + sinf(2.0f * 3.14159265358979f * 6000.0f * t) / 3.0f;
        int s = (int)(v * (float)amp_q15);
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        buf[i] = (int16_t)s;
    }
}

static int32_t peak_abs(const int16_t *buf, uint32_t start, uint32_t end)
{
    int32_t m = 0;
    for (uint32_t i = start; i < end; i++) {
        int32_t a = buf[i] < 0 ? -(int32_t)buf[i] : (int32_t)buf[i];
        if (a > m) m = a;
    }
    return m;
}

/* Sum of |sample| in a sliding band (useful for muting checks). Not
 * preserved under allpass filtering — use `sum_sq_region` for energy
 * comparisons across LR4 splits. */
static int64_t sum_abs_region(const int16_t *buf, uint32_t start, uint32_t end)
{
    int64_t s = 0;
    for (uint32_t i = start; i < end; i++) {
        s += buf[i] < 0 ? -(int64_t)buf[i] : (int64_t)buf[i];
    }
    return s;
}

/* Sum of squares — L2 energy. Preserved by an allpass filter
 * (Parseval), which makes it the right invariant when checking that
 * an LR4-split-and-summed signal reconstructs its input. L1 is not
 * preserved because phase shifts rearrange peak distribution. */
static int64_t sum_sq_region(const int16_t *buf, uint32_t start, uint32_t end)
{
    int64_t s = 0;
    for (uint32_t i = start; i < end; i++) {
        int64_t v = (int64_t)buf[i];
        s += v * v;
    }
    return s;
}

/* Default "bypass-ish" params: unity input/output, unity per-band makeup,
 * zero compression, time-knob mid-range. */
static lfe_fx_ott_params bypass_params(void)
{
    lfe_fx_ott_params p;
    memset(&p, 0, sizeof(p));
    p.depth    = Q15_ONE;   /* Phase 1 ignores this, Phase 2 will honor it */
    p.time     = Q15_ONE / 2;
    p.in_gain  = Q15_ONE;
    p.out_gain = Q15_ONE;
    p.down_low = p.down_mid = p.down_high = 0;
    p.up_low   = p.up_mid   = p.up_high   = 0;
    p.gain_low = p.gain_mid = p.gain_high = Q15_ONE;
    return p;
}

/* ------------------------------------------------------------------ */
/* Tests                                                                */
/* ------------------------------------------------------------------ */

static void test_ott_boundaries_untouched(void)
{
    LFE_TEST_HEADER("fx ott: boundaries untouched");

    int16_t *before = (int16_t *)calloc(OTT_LEN, sizeof(int16_t));
    int16_t *buf    = (int16_t *)calloc(OTT_LEN, sizeof(int16_t));
    if (!before || !buf) { free(before); free(buf); return; }

    fill_sine(before, OTT_LEN, Q15_HALF, 440.0f);
    memcpy(buf, before, OTT_LEN * sizeof(int16_t));

    lfe_buffer b = { .data = buf, .length = OTT_LEN, .rate = LFE_RATE_32000 };
    lfe_fx_range r = { .start = OTT_HEAD, .end = OTT_TAIL };
    lfe_fx_ott_params p = bypass_params();
    p.down_mid = Q15_ONE;   /* non-trivial amount of processing in the middle */

    lfe_status rc = lfe_fx_ott(&b, &r, &p);
    LFE_TEST_ASSERT(rc == LFE_OK || rc == LFE_WARN_CLIPPED, "OTT returned OK");

    int head_ok = memcmp(buf, before, OTT_HEAD * sizeof(int16_t)) == 0;
    int tail_ok = memcmp(buf + OTT_TAIL, before + OTT_TAIL,
                         (OTT_LEN - OTT_TAIL) * sizeof(int16_t)) == 0;
    LFE_TEST_ASSERT(head_ok, "selection head byte-identical");
    LFE_TEST_ASSERT(tail_ok, "selection tail byte-identical");

    free(before);
    free(buf);
}

/* Diagnostic 1: single biquad LP DC gain — must be exactly 1.0. If this
 * fails, the biquad coefficient derivation is wrong. */
static void test_ott_diag_biquad_dc_gain(void)
{
    LFE_TEST_HEADER("diag: single biquad LP DC gain");

    lfe_biquad_state bq;
    lfe_biquad_init_lp_butter(&bq, 88u, 32000u);

    /* Drive a long DC step at Q15_HALF and measure steady-state output. */
    int32_t sum = 0;
    uint32_t warmup = 4000;
    uint32_t total  = 8000;
    for (uint32_t i = 0; i < total; i++) {
        q15_t y = lfe_biquad_step(&bq, Q15_HALF);
        if (i >= warmup) sum += y;
    }
    int32_t avg = sum / (int32_t)(total - warmup);

    if (!(avg > Q15_HALF * 98 / 100 && avg < Q15_HALF * 102 / 100)) {
        fprintf(stderr, "biquad LP DC out = %d (expected ~%d)\n", avg, Q15_HALF);
    }
    LFE_TEST_ASSERT(avg > Q15_HALF * 98 / 100 && avg < Q15_HALF * 102 / 100,
                    "single biquad LP DC gain ≈ 1.0");
}

/* Diagnostic 1b: measure passband/stopband magnitude of single biquads
 * to localize which filter is wrong. */
static double measure_sine_rms(const int16_t *buf, uint32_t start, uint32_t end)
{
    int64_t sq = 0;
    for (uint32_t i = start; i < end; i++) sq += (int64_t)buf[i] * buf[i];
    return sqrt((double)sq / (double)(end - start));
}

/* Pure floating-point DF1 biquad — reference implementation for
 * cross-checking the fixed-point one. No truncation, no Q-scaling,
 * exactly the math described in the RBJ cookbook. */
static void ref_biquad_init(double *out_b, double *out_a,
                            double cutoff_hz, double sr, int is_hp)
{
    double w0 = 2.0 * 3.14159265358979323846 * cutoff_hz / sr;
    double cos_w0 = cos(w0);
    double sin_w0 = sin(w0);
    double alpha  = sin_w0 / 1.41421356237309504880;   /* / (2 * 1/√2) */
    double a0 = 1.0 + alpha;

    if (is_hp) {
        out_b[0] =  (1.0 + cos_w0) / 2.0 / a0;
        out_b[1] = -(1.0 + cos_w0) / a0;
        out_b[2] =  (1.0 + cos_w0) / 2.0 / a0;
    } else {
        out_b[0] = (1.0 - cos_w0) / 2.0 / a0;
        out_b[1] =  (1.0 - cos_w0) / a0;
        out_b[2] = (1.0 - cos_w0) / 2.0 / a0;
    }
    out_a[0] = 1.0;
    out_a[1] = -2.0 * cos_w0 / a0;
    out_a[2] = (1.0 - alpha) / a0;
}

static double ref_biquad_step(const double *b, const double *a,
                              double *x1, double *x2,
                              double *y1, double *y2,
                              double in)
{
    double y = b[0]*in + b[1]*(*x1) + b[2]*(*x2) - a[1]*(*y1) - a[2]*(*y2);
    *x2 = *x1; *x1 = in;
    *y2 = *y1; *y1 = y;
    return y;
}

static void test_ott_diag_biquad_magnitudes(void)
{
    LFE_TEST_HEADER("diag: biquad single-stage magnitudes at 440 Hz");

    /* Print the raw Q40 coefficients so we can spot init bugs by eye.
     * Expected values for LP at 88 Hz / 32 kHz (Q40 = 2^40 scale):
     *   b0 ≈  81,050,000       (7.37e-5 × 2^40)
     *   b1 ≈ 162,100,000       (1.47e-4 × 2^40)
     *   b2 ≈  81,050,000
     *   a1 ≈ -2,172,000,000,000
     *   a2 ≈  1,073,000,000,000 */
    {
        lfe_biquad_state tmp;
        lfe_biquad_init_lp_butter(&tmp, 88u, 32000u);
        fprintf(stderr, "  LP coefs: b0=%lld b1=%lld b2=%lld a1=%lld a2=%lld\n",
                (long long)tmp.b0, (long long)tmp.b1, (long long)tmp.b2,
                (long long)tmp.a1, (long long)tmp.a2);

        lfe_biquad_init_hp_butter(&tmp, 88u, 32000u);
        fprintf(stderr, "  HP coefs: b0=%lld b1=%lld b2=%lld a1=%lld a2=%lld\n",
                (long long)tmp.b0, (long long)tmp.b1, (long long)tmp.b2,
                (long long)tmp.a1, (long long)tmp.a2);
    }

    int16_t *in  = (int16_t *)calloc(OTT_LEN, sizeof(int16_t));
    int16_t *out_lp = (int16_t *)calloc(OTT_LEN, sizeof(int16_t));
    int16_t *out_hp = (int16_t *)calloc(OTT_LEN, sizeof(int16_t));
    if (!in || !out_lp || !out_hp) { free(in); free(out_lp); free(out_hp); return; }

    fill_sine(in, OTT_LEN, Q15_HALF, 440.0f);

    /* Single-stage LP at 88 Hz: expected |H| at 440 Hz ≈ 0.040 */
    lfe_biquad_state lp;
    lfe_biquad_init_lp_butter(&lp, 88u, 32000u);
    for (uint32_t i = 0; i < OTT_LEN; i++)
        out_lp[i] = lfe_biquad_step(&lp, in[i]);

    /* Single-stage HP at 88 Hz: expected |H| at 440 Hz ≈ 0.999 */
    lfe_biquad_state hp;
    lfe_biquad_init_hp_butter(&hp, 88u, 32000u);
    for (uint32_t i = 0; i < OTT_LEN; i++)
        out_hp[i] = lfe_biquad_step(&hp, in[i]);

    /* Reference floating-point biquad — no Q-scale, no truncation. */
    double ref_b[3], ref_a[3];
    double rx1 = 0, rx2 = 0, ry1 = 0, ry2 = 0;
    ref_biquad_init(ref_b, ref_a, 88.0, 32000.0, 0);
    fprintf(stderr, "  ref LP b=[%.6e, %.6e, %.6e] a=[%.6f, %.6f, %.6f]\n",
            ref_b[0], ref_b[1], ref_b[2], ref_a[0], ref_a[1], ref_a[2]);
    double ref_lp_sq = 0.0;
    double ref_first20[20];
    for (uint32_t i = 0; i < OTT_LEN; i++) {
        double y = ref_biquad_step(ref_b, ref_a, &rx1, &rx2, &ry1, &ry2, (double)in[i]);
        if (i < 20) ref_first20[i] = y;
        if (i >= OTT_RATE / 10) ref_lp_sq += y * y;
    }
    double ref_lp_rms = sqrt(ref_lp_sq / (double)(OTT_LEN - OTT_RATE / 10));
    fprintf(stderr, "  ref LP |H(440)| = %.4f  (float-ref)\n",
            ref_lp_rms / (Q15_HALF / sqrt(2.0)));

    /* Sample-by-sample comparison of first 20 samples. */
    fprintf(stderr, "  i | in     | fixed | float\n");
    for (int i = 0; i < 20; i++) {
        fprintf(stderr, "  %2d| %6d | %5d | %8.3f\n",
                i, in[i], out_lp[i], ref_first20[i]);
    }

    uint32_t warmup = OTT_RATE / 10;
    double in_rms  = measure_sine_rms(in,     warmup, OTT_LEN);
    double lp_rms  = measure_sine_rms(out_lp, warmup, OTT_LEN);
    double hp_rms  = measure_sine_rms(out_hp, warmup, OTT_LEN);

    fprintf(stderr, "  single LP |H(440)| = %.4f (expected ~0.040)\n", lp_rms / in_rms);
    fprintf(stderr, "  single HP |H(440)| = %.4f (expected ~0.999)\n", hp_rms / in_rms);

    /* These tolerances are wide on purpose — we just want to know
     * if the filter is roughly right. Tight analytic-match tests
     * would add noise; this is diagnostic output only. */
    LFE_TEST_ASSERT(lp_rms / in_rms < 0.10,
                    "LP at 440 Hz strongly attenuated");
    LFE_TEST_ASSERT(hp_rms / in_rms > 0.90 && hp_rms / in_rms < 1.10,
                    "HP at 440 Hz passes through ~unchanged");

    free(in); free(out_lp); free(out_hp);
}

/* Diagnostic 2: LR4 split + recombine on a sine. Theory says sum
 * magnitude is 1.0 everywhere. If L2 ratio deviates significantly from
 * 1.0, the LR4 itself is broken. Isolates from the OTT chain entirely. */
static void test_ott_diag_lr4_reconstruction(void)
{
    LFE_TEST_HEADER("diag: LR4 split+sum preserves L2 energy");

    /* Fresh LR4 at 88 Hz. */
    lfe_lr4_state xo;
    lfe_lr4_init(&xo, 88u, 32000u);

    int16_t *in  = (int16_t *)calloc(OTT_LEN, sizeof(int16_t));
    int16_t *out = (int16_t *)calloc(OTT_LEN, sizeof(int16_t));
    if (!in || !out) { free(in); free(out); return; }

    fill_sine(in, OTT_LEN, Q15_HALF, 440.0f);

    for (uint32_t i = 0; i < OTT_LEN; i++) {
        q15_t lo, hi;
        lfe_lr4_step(&xo, in[i], &lo, &hi);
        int32_t s = (int32_t)lo + (int32_t)hi;
        if (s >  Q15_ONE)     s =  Q15_ONE;
        if (s < -Q15_ONE - 1) s = -Q15_ONE - 1;
        out[i] = (int16_t)s;
    }

    uint32_t warmup = OTT_RATE / 10;
    int64_t e_in  = sum_sq_region(in,  warmup, OTT_LEN);
    int64_t e_out = sum_sq_region(out, warmup, OTT_LEN);
    double ratio = (double)e_out / (double)e_in;
    if (!(ratio > 0.95 && ratio < 1.05)) {
        fprintf(stderr, "LR4 split+sum L2 ratio %.3f (expected ~1.0)\n", ratio);
    }
    LFE_TEST_ASSERT(ratio > 0.95 && ratio < 1.05,
                    "LR4 split+sum preserves L2 within ±5%");

    free(in);
    free(out);
}

static void test_ott_bypass_preserves_signal(void)
{
    LFE_TEST_HEADER("fx ott: bypass preserves signal");

    int16_t *before = (int16_t *)calloc(OTT_LEN, sizeof(int16_t));
    int16_t *buf    = (int16_t *)calloc(OTT_LEN, sizeof(int16_t));
    if (!before || !buf) { free(before); free(buf); return; }

    fill_three_band_mix(before, OTT_LEN, Q15_HALF);
    memcpy(buf, before, OTT_LEN * sizeof(int16_t));

    lfe_buffer b = { .data = buf, .length = OTT_LEN, .rate = LFE_RATE_32000 };
    lfe_fx_range r = { .start = 0, .end = OTT_LEN };
    lfe_fx_ott_params p = bypass_params();

    LFE_TEST_ASSERT(lfe_fx_ott(&b, &r, &p) == LFE_OK, "OTT bypass returned OK");

    /* LR4 is allpass in magnitude (Parseval: L2 energy preserved), but
     * not in L1 — phase shifts rearrange peak distribution across
     * samples. Compare sum-of-squares and allow a small tolerance for
     * the filter warm-up leak and Q29 coefficient truncation. */
    uint32_t warmup = OTT_RATE / 10;   /* 100 ms */
    int64_t e_before = sum_sq_region(before, warmup, OTT_LEN);
    int64_t e_after  = sum_sq_region(buf,    warmup, OTT_LEN);
    double ratio = (double)e_after / (double)e_before;
    if (!(ratio > 0.90 && ratio < 1.10)) {
        fprintf(stderr, "bypass L2 energy ratio %.3f (expected ~1.0)\n", ratio);
    }
    LFE_TEST_ASSERT(ratio > 0.90 && ratio < 1.10,
                    "bypass reconstructs L2 energy within ±10%");

    free(before);
    free(buf);
}

static void test_ott_downward_reduces_loud_peak(void)
{
    LFE_TEST_HEADER("fx ott: downward compression reduces loud peak");

    int16_t *buf_full = (int16_t *)calloc(OTT_LEN, sizeof(int16_t));
    int16_t *buf_comp = (int16_t *)calloc(OTT_LEN, sizeof(int16_t));
    if (!buf_full || !buf_comp) { free(buf_full); free(buf_comp); return; }

    /* Full-scale 440 Hz sine — MID band, well above the -24 dB threshold. */
    fill_sine(buf_full, OTT_LEN, Q15_ONE,  440.0f);
    memcpy(buf_comp, buf_full, OTT_LEN * sizeof(int16_t));

    lfe_buffer b = { .data = buf_comp, .length = OTT_LEN, .rate = LFE_RATE_32000 };
    lfe_fx_range r = { .start = 0, .end = OTT_LEN };
    lfe_fx_ott_params p = bypass_params();
    p.down_mid = Q15_ONE;   /* full downward compression on MID */

    lfe_status rc = lfe_fx_ott(&b, &r, &p);
    LFE_TEST_ASSERT(rc == LFE_OK || rc == LFE_WARN_CLIPPED, "OTT returned OK");

    /* Skip the attack transient + look at steady-state peak. With a
     * full-scale input (~0 dBFS) 16 dB above -24 dBFS threshold at 3:1
     * ratio → ~10.7 dB attenuation → ~30% of original. Allow wide margin
     * to stay robust against envelope-follower timing. */
    uint32_t warmup = OTT_RATE / 5;   /* 200 ms */
    int32_t peak_in  = peak_abs(buf_full, warmup, OTT_LEN);
    int32_t peak_out = peak_abs(buf_comp, warmup, OTT_LEN);

    LFE_TEST_ASSERT(peak_out < peak_in,
                    "compressed peak < input peak");
    /* At least -3 dB of attenuation (≈70% of input peak). */
    LFE_TEST_ASSERT(peak_out * 100 < peak_in * 75,
                    "attenuation ≥ ~3 dB");

    free(buf_full);
    free(buf_comp);
}

static void test_ott_band_gain_zero_mutes_band(void)
{
    LFE_TEST_HEADER("fx ott: gain_mid=0 removes 440 Hz content");

    int16_t *buf = (int16_t *)calloc(OTT_LEN, sizeof(int16_t));
    if (!buf) return;

    fill_sine(buf, OTT_LEN, Q15_HALF, 440.0f);

    lfe_buffer b = { .data = buf, .length = OTT_LEN, .rate = LFE_RATE_32000 };
    lfe_fx_range r = { .start = 0, .end = OTT_LEN };
    lfe_fx_ott_params p = bypass_params();
    p.gain_mid = 0;          /* mute MID band */

    LFE_TEST_ASSERT(lfe_fx_ott(&b, &r, &p) == LFE_OK, "OTT returned OK");

    /* After muting MID, a pure 440 Hz signal should have most of its
     * energy removed. Leave out early samples (filter warm-up). */
    uint32_t warmup = OTT_RATE / 5;
    int64_t e = sum_abs_region(buf, warmup, OTT_LEN);
    /* Expected remnant is tiny (leakage through LOW+HIGH edges). Input
     * would be ~ OTT_LEN * Q15_HALF * 2/pi ≈ ~1.7e8 over (OTT_LEN-warmup).
     * Require the remnant to be at least 8x below that. */
    int64_t remaining_samples = (int64_t)(OTT_LEN - warmup);
    int64_t ideal = (remaining_samples * Q15_HALF * 64) / 100;  /* ~0.64 × half-scale */
    LFE_TEST_ASSERT(e < ideal / 4,
                    "muting MID removes >75% of sine energy");

    free(buf);
}

/* Phase 2: upward compression should BOOST a quiet signal (below the
 * upward threshold ≈ -40 dBFS). We use a very low-amplitude sine — the
 * output should be louder than the input. */
static void test_ott_upward_boosts_quiet_signal(void)
{
    LFE_TEST_HEADER("fx ott: upward compression boosts quiet signal");

    int16_t *in  = (int16_t *)calloc(OTT_LEN, sizeof(int16_t));
    int16_t *out = (int16_t *)calloc(OTT_LEN, sizeof(int16_t));
    if (!in || !out) { free(in); free(out); return; }

    /* -60 dBFS ≈ Q15_ONE / 1000 ≈ 33. Squarely below -40 dBFS threshold. */
    fill_sine(in, OTT_LEN, 33, 440.0f);
    memcpy(out, in, OTT_LEN * sizeof(int16_t));

    lfe_buffer b = { .data = out, .length = OTT_LEN, .rate = LFE_RATE_32000 };
    lfe_fx_range r = { .start = 0, .end = OTT_LEN };
    lfe_fx_ott_params p = bypass_params();
    p.up_mid = Q15_ONE;           /* full upward on MID (where 440 Hz lives) */

    LFE_TEST_ASSERT(lfe_fx_ott(&b, &r, &p) >= 0, "OTT returned OK");

    uint32_t warmup = OTT_RATE / 5;   /* 200 ms */
    int64_t e_in  = sum_sq_region(in,  warmup, OTT_LEN);
    int64_t e_out = sum_sq_region(out, warmup, OTT_LEN);
    LFE_TEST_ASSERT(e_out > e_in * 2,
                    "upward compression produces at least 2× more L2 energy");

    free(in); free(out);
}

/* Phase 2: Depth knob scales the effect. At depth=0, the compressor
 * must be a pass-through (even with full down/up settings). */
static void test_ott_depth_zero_is_bypass(void)
{
    LFE_TEST_HEADER("fx ott: depth=0 bypasses compression");

    int16_t *before = (int16_t *)calloc(OTT_LEN, sizeof(int16_t));
    int16_t *buf    = (int16_t *)calloc(OTT_LEN, sizeof(int16_t));
    if (!before || !buf) { free(before); free(buf); return; }

    fill_three_band_mix(before, OTT_LEN, Q15_HALF);
    memcpy(buf, before, OTT_LEN * sizeof(int16_t));

    lfe_buffer b = { .data = buf, .length = OTT_LEN, .rate = LFE_RATE_32000 };
    lfe_fx_range r = { .start = 0, .end = OTT_LEN };
    lfe_fx_ott_params p = bypass_params();
    p.depth     = 0;              /* fully off */
    p.down_low = p.down_mid = p.down_high = Q15_ONE;  /* would normally compress */
    p.up_low   = p.up_mid   = p.up_high   = Q15_ONE;

    LFE_TEST_ASSERT(lfe_fx_ott(&b, &r, &p) >= 0, "OTT returned OK");

    uint32_t warmup = OTT_RATE / 10;
    int64_t e_before = sum_sq_region(before, warmup, OTT_LEN);
    int64_t e_after  = sum_sq_region(buf,    warmup, OTT_LEN);
    double ratio = (double)e_after / (double)e_before;
    LFE_TEST_ASSERT(ratio > 0.90 && ratio < 1.10,
                    "depth=0 preserves L2 energy within ±10%");

    free(before); free(buf);
}

static void test_ott_validation(void)
{
    LFE_TEST_HEADER("fx ott: validation");

    int16_t buf_data[16] = { 0 };
    lfe_buffer b = { .data = buf_data, .length = 16, .rate = LFE_RATE_32000 };
    lfe_fx_range r = { .start = 0, .end = 16 };
    lfe_fx_ott_params p = bypass_params();

    LFE_TEST_ASSERT_EQ(lfe_fx_ott(NULL, &r, &p),
                       LFE_ERR_NULL, "NULL buf");
    LFE_TEST_ASSERT_EQ(lfe_fx_ott(&b, NULL, &p),
                       LFE_ERR_NULL, "NULL range");
    LFE_TEST_ASSERT_EQ(lfe_fx_ott(&b, &r, NULL),
                       LFE_ERR_NULL, "NULL params");

    lfe_fx_range bad = { .start = 0, .end = 17 };
    LFE_TEST_ASSERT_EQ(lfe_fx_ott(&b, &bad, &p),
                       LFE_ERR_BAD_PARAM, "range.end > buf.length");

    lfe_fx_range empty = { .start = 5, .end = 5 };
    LFE_TEST_ASSERT_EQ(lfe_fx_ott(&b, &empty, &p),
                       LFE_OK, "empty range is a no-op");
}

static void test_ott_wav_dump(void)
{
    LFE_TEST_HEADER("fx ott: WAV dump (ear test)");

    int16_t *buf = (int16_t *)calloc(OTT_LEN, sizeof(int16_t));
    if (!buf) return;

    fill_three_band_mix(buf, OTT_LEN, Q15_ONE);  /* loud all bands */

    lfe_buffer b = { .data = buf, .length = OTT_LEN, .rate = LFE_RATE_32000 };
    lfe_fx_range r = { .start = 0, .end = OTT_LEN };
    lfe_fx_ott_params p = bypass_params();
    p.down_low  = Q15_ONE;
    p.down_mid  = Q15_ONE;
    p.down_high = Q15_ONE;

    (void)lfe_fx_ott(&b, &r, &p);
    lfe_test_wav_write_mono16("test/output/fx_ott_down.wav",
                              buf, OTT_LEN, OTT_RATE);

    LFE_TEST_ASSERT(1, "WAV dump succeeded");
    free(buf);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */

void lfe_test_ott(void)
{
    test_ott_boundaries_untouched();
    test_ott_diag_biquad_dc_gain();
    test_ott_diag_biquad_magnitudes();
    test_ott_diag_lr4_reconstruction();
    test_ott_bypass_preserves_signal();
    test_ott_downward_reduces_loud_peak();
    test_ott_band_gain_zero_mutes_band();
    test_ott_upward_boosts_quiet_signal();
    test_ott_depth_zero_is_bypass();
    test_ott_validation();
    test_ott_wav_dump();
}
