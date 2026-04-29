/*
 * test_drawn.c — Tests for the drawn-waveform generator (Phase 2).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Each preset gets a basic shape sanity check (peak amplitude, sign
 * pattern, key zero-crossings), plus a determinism check across two
 * fresh fills. The canvas-to-int16 conversion gets its own test, and
 * each preset gets a WAV dump for ear-testing the generator output
 * looped at a recognizable pitch.
 */

#include "test_main.h"

#include "lfe.h"
#include "util/wav.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CANVAS_LEN 256u

static void test_drawn_fill_preset_sine(void)
{
    LFE_TEST_HEADER("drawn preset sine");

    int8_t canvas[CANVAS_LEN];
    lfe_status rc = lfe_drawn_fill_preset(canvas, CANVAS_LEN,
                                          LFE_DRAWN_PRESET_SINE);
    LFE_TEST_ASSERT_EQ(rc, LFE_OK, "fill_preset(sine) returns OK");

    /* Sine starts at 0, peaks near +127 around quarter cycle, crosses
     * zero at half, troughs near -128 at three-quarters. */
    LFE_TEST_ASSERT(canvas[0] == 0, "sine[0] is 0");
    LFE_TEST_ASSERT(canvas[CANVAS_LEN / 4] >= 120,
                    "sine peak (~quarter) is near +127");
    LFE_TEST_ASSERT(canvas[CANVAS_LEN / 2] >= -8 && canvas[CANVAS_LEN / 2] <= 8,
                    "sine zero crossing (~half) near 0");
    LFE_TEST_ASSERT(canvas[3 * CANVAS_LEN / 4] <= -120,
                    "sine trough (~three-quarters) near -128");
}

static void test_drawn_fill_preset_saw(void)
{
    LFE_TEST_HEADER("drawn preset saw");

    int8_t canvas[CANVAS_LEN];
    lfe_drawn_fill_preset(canvas, CANVAS_LEN, LFE_DRAWN_PRESET_SAW);

    LFE_TEST_ASSERT(canvas[0] == -128,         "saw starts at -128");
    LFE_TEST_ASSERT(canvas[CANVAS_LEN - 1] >= 124,
                    "saw ends near +127");
    /* Monotonic increase. */
    int monotonic = 1;
    for (uint32_t i = 1; i < CANVAS_LEN; i++) {
        if (canvas[i] < canvas[i - 1]) { monotonic = 0; break; }
    }
    LFE_TEST_ASSERT(monotonic, "saw is monotonically non-decreasing");
}

static void test_drawn_fill_preset_square(void)
{
    LFE_TEST_HEADER("drawn preset square");

    int8_t canvas[CANVAS_LEN];
    lfe_drawn_fill_preset(canvas, CANVAS_LEN, LFE_DRAWN_PRESET_SQUARE);

    LFE_TEST_ASSERT(canvas[0]              == -128,
                    "square first half is -128");
    LFE_TEST_ASSERT(canvas[CANVAS_LEN / 2] ==  127,
                    "square second half is +127");
}

static void test_drawn_fill_preset_triangle(void)
{
    LFE_TEST_HEADER("drawn preset triangle");

    int8_t canvas[CANVAS_LEN];
    lfe_drawn_fill_preset(canvas, CANVAS_LEN, LFE_DRAWN_PRESET_TRIANGLE);

    /* Triangle starts low, peaks in the middle, ends low. */
    LFE_TEST_ASSERT(canvas[0]                  <= -120,
                    "triangle starts near -127");
    LFE_TEST_ASSERT(canvas[CANVAS_LEN / 2]     >= 120,
                    "triangle peak is near +127");
    LFE_TEST_ASSERT(canvas[CANVAS_LEN - 1]     <= -120,
                    "triangle ends near -127");
}

static void test_drawn_fill_preset_noise_deterministic(void)
{
    LFE_TEST_HEADER("drawn preset noise determinism");

    int8_t a[CANVAS_LEN], b[CANVAS_LEN];
    lfe_drawn_fill_preset(a, CANVAS_LEN, LFE_DRAWN_PRESET_NOISE);
    lfe_drawn_fill_preset(b, CANVAS_LEN, LFE_DRAWN_PRESET_NOISE);

    LFE_TEST_ASSERT(memcmp(a, b, CANVAS_LEN) == 0,
                    "noise preset is deterministic across two fills");

    /* Should not be all zeros. */
    int nonzero = 0;
    for (uint32_t i = 0; i < CANVAS_LEN; i++) if (a[i] != 0) { nonzero = 1; break; }
    LFE_TEST_ASSERT(nonzero, "noise preset produces non-zero data");
}

static void test_drawn_gen_canvas_to_int16(void)
{
    LFE_TEST_HEADER("drawn canvas → int16");

    int8_t canvas[8] = { -128, -64, -1, 0, 1, 64, 100, 127 };
    int16_t out[8];

    lfe_buffer outbuf = {
        .data   = out,
        .length = 8,
        .rate   = LFE_RATE_32000,
    };
    lfe_drawn_params p = {
        .canvas        = canvas,
        .canvas_length = 8,
    };

    lfe_status rc = lfe_gen_drawn(&outbuf, &p);
    LFE_TEST_ASSERT_EQ(rc, LFE_OK, "lfe_gen_drawn returns OK");

    /* canvas[i] << 8 */
    LFE_TEST_ASSERT_EQ(out[0], -32768, "out[0] = -128 << 8");
    LFE_TEST_ASSERT_EQ(out[1], -16384, "out[1] =  -64 << 8");
    LFE_TEST_ASSERT_EQ(out[2],   -256, "out[2] =   -1 << 8");
    LFE_TEST_ASSERT_EQ(out[3],      0, "out[3] =    0 << 8");
    LFE_TEST_ASSERT_EQ(out[4],    256, "out[4] =    1 << 8");
    LFE_TEST_ASSERT_EQ(out[5],  16384, "out[5] =   64 << 8");
    LFE_TEST_ASSERT_EQ(out[6],  25600, "out[6] =  100 << 8");
    LFE_TEST_ASSERT_EQ(out[7],  32512, "out[7] =  127 << 8");
}

static void test_drawn_gen_param_validation(void)
{
    LFE_TEST_HEADER("drawn param validation");

    int8_t canvas[4]  = { 0, 0, 0, 0 };
    int16_t out[4]    = { 0, 0, 0, 0 };
    lfe_buffer outbuf = { .data = out, .length = 4, .rate = LFE_RATE_32000 };
    lfe_drawn_params p = { .canvas = canvas, .canvas_length = 4 };

    LFE_TEST_ASSERT_EQ(lfe_gen_drawn(NULL, &p), LFE_ERR_NULL,
                       "NULL out → LFE_ERR_NULL");
    LFE_TEST_ASSERT_EQ(lfe_gen_drawn(&outbuf, NULL), LFE_ERR_NULL,
                       "NULL params → LFE_ERR_NULL");

    /* Mismatched lengths. */
    lfe_drawn_params bad = { .canvas = canvas, .canvas_length = 8 };
    LFE_TEST_ASSERT_EQ(lfe_gen_drawn(&outbuf, &bad), LFE_ERR_BUF_TOO_SMALL,
                       "length mismatch → LFE_ERR_BUF_TOO_SMALL");

    /* Zero-length canvas. */
    lfe_drawn_params zero = { .canvas = canvas, .canvas_length = 0 };
    LFE_TEST_ASSERT_EQ(lfe_gen_drawn(&outbuf, &zero), LFE_ERR_BAD_PARAM,
                       "zero canvas length → LFE_ERR_BAD_PARAM");
}

static void test_drawn_wav_dump_each_preset(void)
{
    LFE_TEST_HEADER("drawn WAV dumps");

    /* Render each preset as a 1-second sample by repeating the 256-point
     * canvas. The repeat rate (32000 / 256 ≈ 125 Hz) is the fundamental
     * pitch of the looped sample. */
    static const struct {
        lfe_drawn_preset preset;
        const char      *name;
    } presets[] = {
        { LFE_DRAWN_PRESET_SINE,     "test/output/drawn_sine.wav"     },
        { LFE_DRAWN_PRESET_SAW,      "test/output/drawn_saw.wav"      },
        { LFE_DRAWN_PRESET_SQUARE,   "test/output/drawn_square.wav"   },
        { LFE_DRAWN_PRESET_TRIANGLE, "test/output/drawn_triangle.wav" },
        { LFE_DRAWN_PRESET_NOISE,    "test/output/drawn_noise.wav"    },
    };

    for (size_t k = 0; k < sizeof(presets) / sizeof(presets[0]); k++) {
        int8_t canvas[CANVAS_LEN];
        lfe_drawn_fill_preset(canvas, CANVAS_LEN, presets[k].preset);

        const uint32_t length = 32000u; /* 1 second at 32 kHz */
        int16_t *buf = (int16_t *)calloc(length, sizeof(int16_t));
        if (!buf) continue;

        /* Tile the canvas across the buffer (the canvas is one cycle). */
        for (uint32_t i = 0; i < length; i++) {
            buf[i] = (int16_t)((int32_t)canvas[i % CANVAS_LEN] << 8);
        }

        int rc = lfe_test_wav_write_mono16(presets[k].name, buf, length, 32000u);
        LFE_TEST_ASSERT_EQ(rc, 0, "preset WAV file written");

        free(buf);
    }
}

void lfe_test_drawn(void)
{
    test_drawn_fill_preset_sine();
    test_drawn_fill_preset_saw();
    test_drawn_fill_preset_square();
    test_drawn_fill_preset_triangle();
    test_drawn_fill_preset_noise_deterministic();
    test_drawn_gen_canvas_to_int16();
    test_drawn_gen_param_validation();
    test_drawn_wav_dump_each_preset();
}
