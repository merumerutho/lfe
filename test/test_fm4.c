/*
 * test_fm4.c — Tests for the 4-operator FM generator (Phase 4c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Phase α scope:
 *   - parameter validation (NULL, unknown preset)
 *   - lfe_fm4_fill_preset for the EP preset
 *   - the EP preset renders non-silent two-sided audio
 *   - determinism: same params → bit-identical output
 *   - the sound differs meaningfully from a single-sine baseline
 *     (proves the mod matrix actually reaches the hot loop)
 *   - WAV dump of the EP preset for ear-testing
 *
 * Subsequent phases will add more presets + per-op param tests.
 */

#include "test_main.h"

#include "lfe.h"
#include "util/fixed.h"
#include "util/wav.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FM4_RATE   32000u
#define FM4_LEN_MS 1000u
#define FM4_LEN    ((FM4_RATE * FM4_LEN_MS) / 1000u)

static int32_t abs_peak(const int16_t *buf, uint32_t n)
{
    int32_t peak = 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t v = buf[i] < 0 ? -(int32_t)buf[i] : (int32_t)buf[i];
        if (v > peak) peak = v;
    }
    return peak;
}

static void gen_and_check(const lfe_fm4_params *p,
                          int16_t *buf, uint32_t n,
                          const char *label)
{
    memset(buf, 0, n * sizeof(int16_t));
    lfe_buffer outbuf = { .data = buf, .length = n, .rate = LFE_RATE_32000 };
    lfe_status rc = lfe_gen_fm4(&outbuf, p);
    LFE_TEST_ASSERT(rc >= 0, label);
}

/* ------------------------------------------------------------------ */

static void test_fm4_param_validation(void)
{
    LFE_TEST_HEADER("fm4 param validation");

    int16_t buf[64];
    lfe_buffer outbuf = { .data = buf, .length = 64, .rate = LFE_RATE_32000 };
    lfe_fm4_params p;
    lfe_fm4_fill_preset(&p, LFE_FM4_PRESET_EP);

    LFE_TEST_ASSERT_EQ(lfe_gen_fm4(NULL, &p),      LFE_ERR_NULL,
                       "NULL out → LFE_ERR_NULL");
    LFE_TEST_ASSERT_EQ(lfe_gen_fm4(&outbuf, NULL), LFE_ERR_NULL,
                       "NULL params → LFE_ERR_NULL");

    lfe_buffer bad_data = { .data = NULL, .length = 64, .rate = LFE_RATE_32000 };
    LFE_TEST_ASSERT_EQ(lfe_gen_fm4(&bad_data, &p), LFE_ERR_NULL,
                       "NULL out->data → LFE_ERR_NULL");

    lfe_fm4_params dummy;
    LFE_TEST_ASSERT_EQ(lfe_fm4_fill_preset(&dummy, (lfe_fm4_preset)9999),
                       LFE_ERR_BAD_PARAM,
                       "unknown preset → LFE_ERR_BAD_PARAM");
    LFE_TEST_ASSERT_EQ(lfe_fm4_fill_preset(NULL, LFE_FM4_PRESET_EP),
                       LFE_ERR_NULL,
                       "fill_preset NULL → LFE_ERR_NULL");
}

static void test_fm4_fill_preset_ep(void)
{
    LFE_TEST_HEADER("fm4 fill_preset EP");

    lfe_fm4_params p;
    lfe_status rc = lfe_fm4_fill_preset(&p, LFE_FM4_PRESET_EP);
    LFE_TEST_ASSERT_EQ(rc, LFE_OK, "fill_preset OK");

    /* Sanity: the EP preset should have a non-zero base pitch, an
     * audible op 0, and a mod matrix entry connecting op 1 to op 0
     * (that's the defining feature of the 2-op EP topology). */
    LFE_TEST_ASSERT(p.base_hz_q8 > 0, "base_hz non-zero");
    LFE_TEST_ASSERT(p.carrier_mix[0] > 0, "op 0 is an audible carrier");
    LFE_TEST_ASSERT(p.mod_matrix[1][0] != 0, "op 1 modulates op 0");
}

static void test_fm4_ep_non_silent(void)
{
    LFE_TEST_HEADER("fm4 EP renders audio");

    lfe_fm4_params p;
    lfe_fm4_fill_preset(&p, LFE_FM4_PRESET_EP);

    int16_t *buf = (int16_t *)calloc(FM4_LEN, sizeof(int16_t));
    if (!buf) return;

    gen_and_check(&p, buf, FM4_LEN, "EP gen OK");

    int32_t peak = abs_peak(buf, FM4_LEN);
    LFE_TEST_ASSERT(peak > 4000, "EP has substantial energy");

    /* Two-sided check — a degenerate stuck-at-rail case would fail. */
    bool has_positive = false, has_negative = false;
    for (uint32_t i = 0; i < FM4_LEN; i++) {
        if (buf[i] >  4000) has_positive = true;
        if (buf[i] < -4000) has_negative = true;
        if (has_positive && has_negative) break;
    }
    LFE_TEST_ASSERT(has_positive && has_negative,
                    "EP output varies on both sides of zero");

    free(buf);
}

static void test_fm4_determinism(void)
{
    LFE_TEST_HEADER("fm4 determinism");

    lfe_fm4_params p;
    lfe_fm4_fill_preset(&p, LFE_FM4_PRESET_EP);

    int16_t *a = (int16_t *)calloc(FM4_LEN, sizeof(int16_t));
    int16_t *b = (int16_t *)calloc(FM4_LEN, sizeof(int16_t));
    if (!a || !b) { free(a); free(b); return; }

    gen_and_check(&p, a, FM4_LEN, "first gen OK");
    gen_and_check(&p, b, FM4_LEN, "second gen OK");

    LFE_TEST_ASSERT(memcmp(a, b, FM4_LEN * sizeof(int16_t)) == 0,
                    "same params → byte-identical output");

    free(a); free(b);
}

static void test_fm4_matrix_affects_output(void)
{
    LFE_TEST_HEADER("fm4 mod matrix reaches the hot loop");

    /* Same preset twice, once with the mod matrix zeroed out. If the
     * matrix path is wired correctly, these two renders must differ —
     * with zero matrix, op 0 becomes a pure (modulated only by itself
     * which is zero) sine carrier, no bell character. */
    lfe_fm4_params p_full;
    lfe_fm4_fill_preset(&p_full, LFE_FM4_PRESET_EP);

    lfe_fm4_params p_nomod = p_full;
    memset(p_nomod.mod_matrix, 0, sizeof(p_nomod.mod_matrix));

    int16_t *a = (int16_t *)calloc(FM4_LEN, sizeof(int16_t));
    int16_t *b = (int16_t *)calloc(FM4_LEN, sizeof(int16_t));
    if (!a || !b) { free(a); free(b); return; }

    gen_and_check(&p_full,  a, FM4_LEN, "full-matrix EP OK");
    gen_and_check(&p_nomod, b, FM4_LEN, "zero-matrix EP OK");

    LFE_TEST_ASSERT(memcmp(a, b, FM4_LEN * sizeof(int16_t)) != 0,
                    "mod matrix changes output");

    /* Both should still be non-silent (zero matrix just removes the
     * bell character; op 0 is still audible as a pitched sine). */
    LFE_TEST_ASSERT(abs_peak(b, FM4_LEN) > 4000,
                    "zero-matrix still has op 0 carrier audio");

    free(a); free(b);
}

static void test_fm4_fill_preset_all(void)
{
    LFE_TEST_HEADER("fm4 fill_preset for all presets");

    static const lfe_fm4_preset presets[] = {
        LFE_FM4_PRESET_EP,
        LFE_FM4_PRESET_BELL,
        LFE_FM4_PRESET_BASS,
        LFE_FM4_PRESET_BRASS,
        LFE_FM4_PRESET_PLUCK,
        LFE_FM4_PRESET_WOBBLE,
        LFE_FM4_PRESET_GROWL,
    };

    for (size_t k = 0; k < sizeof(presets) / sizeof(presets[0]); k++) {
        lfe_fm4_params p;
        lfe_status rc = lfe_fm4_fill_preset(&p, presets[k]);
        LFE_TEST_ASSERT_EQ(rc, LFE_OK, "fill_preset OK");

        /* Minimum sanity: a valid preset must have a non-zero base
         * pitch, at least one audible carrier, and at least one mod-
         * matrix edge (otherwise it's not really "FM" — a pure sine
         * would be). */
        LFE_TEST_ASSERT(p.base_hz_q8 > 0, "base_hz non-zero");

        bool has_carrier = false;
        for (int i = 0; i < LFE_FM4_NUM_OPS; i++) {
            if (p.carrier_mix[i] != 0) { has_carrier = true; break; }
        }
        LFE_TEST_ASSERT(has_carrier, "preset has at least one carrier");

        bool has_mod_edge = false;
        for (int j = 0; j < LFE_FM4_NUM_OPS && !has_mod_edge; j++) {
            for (int i = 0; i < LFE_FM4_NUM_OPS && !has_mod_edge; i++) {
                if (p.mod_matrix[j][i] != 0) has_mod_edge = true;
            }
        }
        LFE_TEST_ASSERT(has_mod_edge, "preset has at least one mod edge");
    }
}

static void test_fm4_wav_dump_all(void)
{
    LFE_TEST_HEADER("fm4 WAV dumps");

    static const struct {
        lfe_fm4_preset preset;
        const char    *path;
    } presets[] = {
        { LFE_FM4_PRESET_EP,     "test/output/fm4_ep.wav"     },
        { LFE_FM4_PRESET_BELL,   "test/output/fm4_bell.wav"   },
        { LFE_FM4_PRESET_BASS,   "test/output/fm4_bass.wav"   },
        { LFE_FM4_PRESET_BRASS,  "test/output/fm4_brass.wav"  },
        { LFE_FM4_PRESET_PLUCK,  "test/output/fm4_pluck.wav"  },
        { LFE_FM4_PRESET_WOBBLE, "test/output/fm4_wobble.wav" },
        { LFE_FM4_PRESET_GROWL,  "test/output/fm4_growl.wav"  },
    };

    for (size_t k = 0; k < sizeof(presets) / sizeof(presets[0]); k++) {
        lfe_fm4_params p;
        lfe_fm4_fill_preset(&p, presets[k].preset);

        int16_t *buf = (int16_t *)calloc(FM4_LEN, sizeof(int16_t));
        if (!buf) continue;

        lfe_buffer outbuf = {
            .data = buf, .length = FM4_LEN, .rate = LFE_RATE_32000,
        };
        lfe_status rc = lfe_gen_fm4(&outbuf, &p);
        LFE_TEST_ASSERT(rc >= 0, "preset gen succeeded");

        /* Sanity: each preset must produce non-silent audio. A stuck
         * preset (accidentally zeroed matrix / carrier_mix / levels)
         * would render as silence and slip through fill_preset_all's
         * "has a carrier + has a mod edge" shape checks. */
        LFE_TEST_ASSERT(abs_peak(buf, FM4_LEN) > 2000,
                        "preset renders substantial audio");

        int wav_rc = lfe_test_wav_write_mono16(presets[k].path, buf,
                                               FM4_LEN, FM4_RATE);
        LFE_TEST_ASSERT_EQ(wav_rc, 0, "preset WAV written");

        free(buf);
    }
}

/* ------------------------------------------------------------------ */
/* LFO-specific tests (Phase β: modulators)                            */
/* ------------------------------------------------------------------ */

/* With a matrix-cell LFO at nonzero depth, the render must differ
 * from the same preset with LFO disabled (depth=0). Proves the LFO
 * path actually reaches the modulation destination. */
static void test_fm4_lfo_reaches_output(void)
{
    LFE_TEST_HEADER("fm4 LFO modulation reaches output");

    lfe_fm4_params p_with;
    lfe_fm4_fill_preset(&p_with, LFE_FM4_PRESET_WOBBLE);

    lfe_fm4_params p_without = p_with;
    for (int k = 0; k < LFE_FM4_NUM_LFOS; k++) {
        p_without.lfos[k].dest      = LFE_FM4_LFO_DEST_OFF;
        p_without.lfos[k].cfg.depth = 0;
    }

    int16_t *a = (int16_t *)calloc(FM4_LEN, sizeof(int16_t));
    int16_t *b = (int16_t *)calloc(FM4_LEN, sizeof(int16_t));
    if (!a || !b) { free(a); free(b); return; }

    gen_and_check(&p_with,    a, FM4_LEN, "wobble with LFOs");
    gen_and_check(&p_without, b, FM4_LEN, "wobble LFOs disabled");

    LFE_TEST_ASSERT(memcmp(a, b, FM4_LEN * sizeof(int16_t)) != 0,
                    "LFO-on differs from LFO-off");
    /* Both still audible — disabling the LFO shouldn't silence the
     * underlying FM patch. */
    LFE_TEST_ASSERT(abs_peak(b, FM4_LEN) > 2000,
                    "wobble patch still audible with LFOs disabled");

    free(a); free(b);
}

/* Two distinct LFO rates should produce distinct outputs. Catches
 * regressions where the phase-increment is silently zeroed. */
static void test_fm4_lfo_rate_affects_output(void)
{
    LFE_TEST_HEADER("fm4 LFO rate changes output");

    lfe_fm4_params p_slow;
    lfe_fm4_fill_preset(&p_slow, LFE_FM4_PRESET_WOBBLE);
    p_slow.lfos[0].cfg.rate_hz_q8 = 1u << 8;    /* 1 Hz */
    p_slow.lfos[1].dest = LFE_FM4_LFO_DEST_OFF; /* silence LFO 1 */

    lfe_fm4_params p_fast = p_slow;
    p_fast.lfos[0].cfg.rate_hz_q8 = 8u << 8;    /* 8 Hz */

    int16_t *a = (int16_t *)calloc(FM4_LEN, sizeof(int16_t));
    int16_t *b = (int16_t *)calloc(FM4_LEN, sizeof(int16_t));
    if (!a || !b) { free(a); free(b); return; }

    gen_and_check(&p_slow, a, FM4_LEN, "LFO @ 1 Hz");
    gen_and_check(&p_fast, b, FM4_LEN, "LFO @ 8 Hz");

    LFE_TEST_ASSERT(memcmp(a, b, FM4_LEN * sizeof(int16_t)) != 0,
                    "LFO rate changes output");

    free(a); free(b);
}

/* Pitch-destination LFO should perturb the output of an otherwise
 * stationary patch — same carrier, no matrix LFO, just pitch. */
static void test_fm4_lfo_pitch_dest(void)
{
    LFE_TEST_HEADER("fm4 LFO pitch destination");

    lfe_fm4_params p;
    lfe_fm4_fill_preset(&p, LFE_FM4_PRESET_BASS);
    /* Install a pitch LFO on slot 0; ensure slot 1 is off. */
    p.lfos[0].cfg.shape       = LFE_LFO_SHAPE_SINE;
    p.lfos[0].dest            = LFE_FM4_LFO_DEST_PITCH;
    p.lfos[0].target          = 0;
    p.lfos[0].cfg.rate_hz_q8  = 6u << 8;            /* 6 Hz vibrato */
    p.lfos[0].cfg.depth       = Q15_ONE / 8;
    p.lfos[1].dest            = LFE_FM4_LFO_DEST_OFF;

    lfe_fm4_params p_off = p;
    p_off.lfos[0].dest      = LFE_FM4_LFO_DEST_OFF;
    p_off.lfos[0].cfg.depth = 0;

    int16_t *a = (int16_t *)calloc(FM4_LEN, sizeof(int16_t));
    int16_t *b = (int16_t *)calloc(FM4_LEN, sizeof(int16_t));
    if (!a || !b) { free(a); free(b); return; }

    gen_and_check(&p,     a, FM4_LEN, "pitch LFO on");
    gen_and_check(&p_off, b, FM4_LEN, "pitch LFO off");

    LFE_TEST_ASSERT(memcmp(a, b, FM4_LEN * sizeof(int16_t)) != 0,
                    "pitch LFO produces audible modulation");
    LFE_TEST_ASSERT(abs_peak(a, FM4_LEN) > 2000,
                    "pitch-modulated output still audible");

    free(a); free(b);
}

/* Render of a WOBBLE preset must be deterministic including LFO
 * phase advancement — same params twice → byte-identical output. */
static void test_fm4_lfo_determinism(void)
{
    LFE_TEST_HEADER("fm4 LFO determinism");

    lfe_fm4_params p;
    lfe_fm4_fill_preset(&p, LFE_FM4_PRESET_WOBBLE);

    int16_t *a = (int16_t *)calloc(FM4_LEN, sizeof(int16_t));
    int16_t *b = (int16_t *)calloc(FM4_LEN, sizeof(int16_t));
    if (!a || !b) { free(a); free(b); return; }

    gen_and_check(&p, a, FM4_LEN, "wobble pass 1");
    gen_and_check(&p, b, FM4_LEN, "wobble pass 2");

    LFE_TEST_ASSERT(memcmp(a, b, FM4_LEN * sizeof(int16_t)) == 0,
                    "LFO-driven preset is deterministic");

    free(a); free(b);
}

/* ------------------------------------------------------------------ */

void lfe_test_fm4(void)
{
    test_fm4_param_validation();
    test_fm4_fill_preset_ep();
    test_fm4_ep_non_silent();
    test_fm4_determinism();
    test_fm4_matrix_affects_output();
    test_fm4_fill_preset_all();
    test_fm4_lfo_reaches_output();
    test_fm4_lfo_rate_affects_output();
    test_fm4_lfo_pitch_dest();
    test_fm4_lfo_determinism();
    test_fm4_wav_dump_all();
}
