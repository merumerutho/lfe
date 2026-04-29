/*
 * gen_drum_presets.h — Preset bank for the drum generator.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Intended to be included by exactly one translation unit (gen_drum.c).
 * Contains `static const lfe_drum_params` literals — no externs, no
 * init code. Keeping the literals in a separate header lets callers
 * browse the preset bank without scrolling past the hot-path render
 * loop.
 */

#ifndef LFE_GEN_DRUM_PRESETS_H
#define LFE_GEN_DRUM_PRESETS_H

#include "lfe.h"
#include "util/fixed.h"

/* ------------------------------------------------------------------ */
/* Presets                                                             */
/*                                                                     */
/* Each preset is a plain struct literal. None of them are tuned to    */
/* any particular "genre" — they're reasonable starting points that    */
/* produce recognizable percussion sounds when fed through lfe_gen_drum*/
/* at 32 kHz with a ~500 ms buffer. Users are expected to grab a       */
/* preset and then tweak individual fields.                            */
/* ------------------------------------------------------------------ */

/* Shorthand for an unused mod slot. */
#define LFE_DRUM_MOD_SLOT_UNUSED { \
    .env = { 0 }, .target = LFE_DRUM_MOD_NONE, .depth = 0 }

static const lfe_drum_params preset_kick = {
    .tone_base_hz_q8 = 60u << 8,
    .tone_level      = Q15_ONE,
    .noise_level     = 0,
    .noise_seed      = 0,
    .filter_mode     = LFE_DRUM_FILTER_LP,
    .filter_base_hz  = 400,
    .filter_q        = Q15_HALF,
    .master_level    = Q15_ONE,
    .mods = {
        /* Amp: snappy attack, ~180 ms body decay. */
        { .env = { .attack_ms = 2,  .decay_ms = 180, .sustain_level = 0,
                   .release_ms = 0, .peak_level = Q15_ONE },
          .target = LFE_DRUM_MOD_AMP,   .depth = Q15_ONE },
        /* Pitch: +200 Hz at t=0, falling to base over 40 ms (the thump). */
        { .env = { .attack_ms = 0,  .decay_ms = 40,  .sustain_level = 0,
                   .release_ms = 0, .peak_level = Q15_ONE },
          .target = LFE_DRUM_MOD_PITCH, .depth = 200 },
        LFE_DRUM_MOD_SLOT_UNUSED,
    },
};

static const lfe_drum_params preset_snare = {
    .tone_base_hz_q8 = 180u << 8,
    .tone_level      = Q15_ONE / 3,
    .noise_level     = (Q15_ONE * 2) / 3,
    .noise_seed      = 0,
    .filter_mode     = LFE_DRUM_FILTER_HP,
    .filter_base_hz  = 1500,
    .filter_q        = Q15_ONE,
    .master_level    = Q15_ONE,
    .mods = {
        { .env = { .attack_ms = 1,  .decay_ms = 140, .sustain_level = 0,
                   .release_ms = 0, .peak_level = Q15_ONE },
          .target = LFE_DRUM_MOD_AMP,    .depth = Q15_ONE },
        /* Filter opens briefly at the start (crack), then closes. */
        { .env = { .attack_ms = 0,  .decay_ms = 60,  .sustain_level = 0,
                   .release_ms = 0, .peak_level = Q15_ONE },
          .target = LFE_DRUM_MOD_FILTER, .depth = 2000 },
        LFE_DRUM_MOD_SLOT_UNUSED,
    },
};

static const lfe_drum_params preset_hat_closed = {
    .tone_base_hz_q8 = 0,
    .tone_level      = 0,
    .noise_level     = Q15_ONE,
    .noise_seed      = 0,
    .filter_mode     = LFE_DRUM_FILTER_HP,
    .filter_base_hz  = 6000,
    .filter_q        = Q15_HALF,
    .master_level    = Q15_ONE,
    .mods = {
        { .env = { .attack_ms = 0,  .decay_ms = 45,  .sustain_level = 0,
                   .release_ms = 0, .peak_level = Q15_ONE },
          .target = LFE_DRUM_MOD_AMP, .depth = Q15_ONE },
        LFE_DRUM_MOD_SLOT_UNUSED,
        LFE_DRUM_MOD_SLOT_UNUSED,
    },
};

static const lfe_drum_params preset_hat_open = {
    .tone_base_hz_q8 = 0,
    .tone_level      = 0,
    .noise_level     = Q15_ONE,
    .noise_seed      = 0,
    .filter_mode     = LFE_DRUM_FILTER_HP,
    .filter_base_hz  = 6000,
    .filter_q        = Q15_HALF,
    .master_level    = Q15_ONE,
    .mods = {
        { .env = { .attack_ms = 0,  .decay_ms = 280, .sustain_level = 0,
                   .release_ms = 0, .peak_level = Q15_ONE },
          .target = LFE_DRUM_MOD_AMP, .depth = Q15_ONE },
        LFE_DRUM_MOD_SLOT_UNUSED,
        LFE_DRUM_MOD_SLOT_UNUSED,
    },
};

static const lfe_drum_params preset_tom = {
    .tone_base_hz_q8 = 120u << 8,
    .tone_level      = Q15_ONE,
    .noise_level     = Q15_ONE / 6,
    .noise_seed      = 0,
    .filter_mode     = LFE_DRUM_FILTER_LP,
    .filter_base_hz  = 500,
    .filter_q        = Q15_HALF,
    .master_level    = Q15_ONE,
    .mods = {
        { .env = { .attack_ms = 2,  .decay_ms = 220, .sustain_level = 0,
                   .release_ms = 0, .peak_level = Q15_ONE },
          .target = LFE_DRUM_MOD_AMP,   .depth = Q15_ONE },
        { .env = { .attack_ms = 0,  .decay_ms = 90,  .sustain_level = 0,
                   .release_ms = 0, .peak_level = Q15_ONE },
          .target = LFE_DRUM_MOD_PITCH, .depth = 80 },
        LFE_DRUM_MOD_SLOT_UNUSED,
    },
};

static const lfe_drum_params preset_clap = {
    .tone_base_hz_q8 = 0,
    .tone_level      = 0,
    .noise_level     = Q15_ONE,
    .noise_seed      = 0,
    .filter_mode     = LFE_DRUM_FILTER_BP,
    .filter_base_hz  = 1500,
    .filter_q        = Q15_HALF,
    .master_level    = Q15_ONE,
    .mods = {
        /* A clap is a short click followed by a longer body. Two AMP
         * modulations with different decay times give that shape
         * without needing a dedicated multi-stage envelope type. */
        { .env = { .attack_ms = 0,  .decay_ms = 10,  .sustain_level = 0,
                   .release_ms = 0, .peak_level = Q15_ONE },
          .target = LFE_DRUM_MOD_AMP, .depth = Q15_ONE },
        { .env = { .attack_ms = 5,  .decay_ms = 150, .sustain_level = 0,
                   .release_ms = 0, .peak_level = Q15_ONE },
          .target = LFE_DRUM_MOD_AMP, .depth = (Q15_ONE * 3) / 4 },
        LFE_DRUM_MOD_SLOT_UNUSED,
    },
};

/* ------------------------------------------------------------------ */
/* Preset: 808-style kick                                              */
/*                                                                     */
/* Triangle-tone instead of sine gives audibly more harmonics; heavy  */
/* drive warms and saturates the attack. Pitch sweep from 240 Hz down */
/* to 50 Hz over 60 ms produces the classic long thump.                */
/* ------------------------------------------------------------------ */

static const lfe_drum_params preset_kick_808 = {
    .tone_base_hz_q8 = 50u << 8,
    .tone_level      = Q15_ONE,
    .tone_wave       = LFE_DRUM_WAVE_TRIANGLE,
    .noise_level     = 0,
    .noise_seed      = 0,
    .filter_mode     = LFE_DRUM_FILTER_LP,
    .filter_base_hz  = 300,
    .filter_q        = Q15_HALF,
    .master_level    = Q15_ONE,
    .drive           = (Q15_ONE * 2) / 3,   /* moderate saturation */
    .mods = {
        { .env = { .attack_ms = 2,  .decay_ms = 280, .sustain_level = 0,
                   .release_ms = 0, .peak_level = Q15_ONE },
          .target = LFE_DRUM_MOD_AMP,   .depth = Q15_ONE },
        { .env = { .attack_ms = 0,  .decay_ms = 60,  .sustain_level = 0,
                   .release_ms = 0, .peak_level = Q15_ONE },
          .target = LFE_DRUM_MOD_PITCH, .depth = 190 },
        LFE_DRUM_MOD_SLOT_UNUSED,
    },
};

/* ------------------------------------------------------------------ */
/* Preset: cowbell                                                     */
/*                                                                     */
/* Square tone at a mid-high fundamental + LFO gating the tone level  */
/* creates the chopped bright timbre of an 808 cowbell-style voice.   */
/* Short decay so it sits crisp in a groove.                           */
/* ------------------------------------------------------------------ */

static const lfe_drum_params preset_cowbell = {
    .tone_base_hz_q8 = 560u << 8,
    .tone_level      = Q15_ONE / 2,
    .tone_wave       = LFE_DRUM_WAVE_SQUARE,
    .noise_level     = 0,
    .noise_seed      = 0,
    .filter_mode     = LFE_DRUM_FILTER_LP,
    .filter_base_hz  = 4000,
    .filter_q        = Q15_HALF,
    .master_level    = Q15_ONE,
    .drive           = Q15_ONE / 4,
    .mods = {
        { .env = { .attack_ms = 1,  .decay_ms = 160, .sustain_level = 0,
                   .release_ms = 0, .peak_level = Q15_ONE },
          .target = LFE_DRUM_MOD_AMP, .depth = Q15_ONE },
        LFE_DRUM_MOD_SLOT_UNUSED,
        LFE_DRUM_MOD_SLOT_UNUSED,
    },
    .lfo = {
        .cfg  = { .shape = LFE_LFO_SHAPE_TRIANGLE,
                  .rate_hz_q8 = 12u << 8,   /* 12 Hz chop */
                  .depth = Q15_ONE / 2 },
        .dest = LFE_DRUM_LFO_DEST_TONE_LEVEL,
    },
};

#endif /* LFE_GEN_DRUM_PRESETS_H */
