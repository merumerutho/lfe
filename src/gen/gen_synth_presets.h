/*
 * gen_synth_presets.h — Preset bank for the dual-osc subtractive synth.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Intended to be included by exactly one translation unit (gen_synth.c).
 * Contains `static const lfe_synth_params` literals — no externs, no
 * init code.
 */

#ifndef LFE_GEN_SYNTH_PRESETS_H
#define LFE_GEN_SYNTH_PRESETS_H

#include "lfe.h"
#include "util/fixed.h"

/* ------------------------------------------------------------------ */
/* Presets                                                             */
/*                                                                     */
/* Reasonable starting points, not finely tuned. Users grab one and    */
/* tweak. Every preset is ~1 second at 32 kHz buffer when played out.  */
/* ------------------------------------------------------------------ */

#define LFE_SYNTH_MOD_SLOT_UNUSED { \
    .env = { 0 }, .target = LFE_SYNTH_MOD_NONE, .depth = 0 }

/* Common A4 base: 220 Hz (one octave below, a more common synth pitch
 * than A4 itself). Individual presets can override. */
#define BASE_HZ_Q8 (220u << 8)

static const lfe_synth_params preset_lead = {
    .base_hz_q8 = BASE_HZ_Q8,
    .osc1 = { .wave = LFE_SYNTH_WAVE_SAW, .detune_hz = -3,
              .level = Q15_ONE, .pulse_width = Q15_HALF },
    .osc2 = { .wave = LFE_SYNTH_WAVE_SAW, .detune_hz = +3,
              .level = Q15_ONE, .pulse_width = Q15_HALF },
    .noise_level    = 0,
    .noise_seed     = 0,
    .filter_mode    = LFE_DRUM_FILTER_LP,
    .filter_base_hz = 2000,
    .filter_q       = Q15_HALF,
    .master_level   = Q15_ONE,
    .note_off_sample = 22400,   /* ~700 ms at 32 kHz — release phase */
    .mods = {
        /* Amp: snappy attack, sustained body, moderate release */
        { .env = { .attack_ms = 5,  .decay_ms = 100, .sustain_level = Q15_ONE * 3 / 4,
                   .release_ms = 200, .peak_level = Q15_ONE },
          .target = LFE_SYNTH_MOD_AMP, .depth = Q15_ONE },
        /* Filter env: bright sweep in, closes back down */
        { .env = { .attack_ms = 3,  .decay_ms = 250, .sustain_level = Q15_ONE / 3,
                   .release_ms = 200, .peak_level = Q15_ONE },
          .target = LFE_SYNTH_MOD_FILTER, .depth = 3500 },
        LFE_SYNTH_MOD_SLOT_UNUSED,
        LFE_SYNTH_MOD_SLOT_UNUSED,
    },
};

static const lfe_synth_params preset_pad = {
    .base_hz_q8 = BASE_HZ_Q8,
    .osc1 = { .wave = LFE_SYNTH_WAVE_SAW, .detune_hz = -5,
              .level = Q15_ONE, .pulse_width = Q15_HALF },
    .osc2 = { .wave = LFE_SYNTH_WAVE_SAW, .detune_hz = +7,
              .level = Q15_ONE, .pulse_width = Q15_HALF },
    .noise_level    = 0,
    .noise_seed     = 0,
    .filter_mode    = LFE_DRUM_FILTER_LP,
    .filter_base_hz = 1200,
    .filter_q       = Q15_ONE,
    .master_level   = Q15_ONE,
    .note_off_sample = 16000,   /* ~500 ms, long release stretches past end */
    .mods = {
        /* Amp: slow attack, full sustain, long release */
        { .env = { .attack_ms = 300, .decay_ms = 100, .sustain_level = Q15_ONE,
                   .release_ms = 800, .peak_level = Q15_ONE },
          .target = LFE_SYNTH_MOD_AMP, .depth = Q15_ONE },
        LFE_SYNTH_MOD_SLOT_UNUSED,
        LFE_SYNTH_MOD_SLOT_UNUSED,
        LFE_SYNTH_MOD_SLOT_UNUSED,
    },
};

static const lfe_synth_params preset_pluck = {
    .base_hz_q8 = BASE_HZ_Q8,
    .osc1 = { .wave = LFE_SYNTH_WAVE_SAW, .detune_hz = 0,
              .level = Q15_ONE, .pulse_width = Q15_HALF },
    .osc2 = { .wave = LFE_SYNTH_WAVE_TRIANGLE, .detune_hz = 0,
              .level = Q15_ONE / 2, .pulse_width = Q15_HALF },
    .noise_level    = 0,
    .noise_seed     = 0,
    .filter_mode    = LFE_DRUM_FILTER_LP,
    .filter_base_hz = 800,
    .filter_q       = Q15_HALF,
    .master_level   = Q15_ONE,
    .note_off_sample = 0,   /* no explicit release — amp env decays to silence */
    .mods = {
        /* Amp: fast attack, fast decay to zero (plucky) */
        { .env = { .attack_ms = 2, .decay_ms = 250, .sustain_level = 0,
                   .release_ms = 50, .peak_level = Q15_ONE },
          .target = LFE_SYNTH_MOD_AMP, .depth = Q15_ONE },
        /* Filter env: bright ping */
        { .env = { .attack_ms = 0, .decay_ms = 180, .sustain_level = 0,
                   .release_ms = 50, .peak_level = Q15_ONE },
          .target = LFE_SYNTH_MOD_FILTER, .depth = 4000 },
        LFE_SYNTH_MOD_SLOT_UNUSED,
        LFE_SYNTH_MOD_SLOT_UNUSED,
    },
};

static const lfe_synth_params preset_bass = {
    .base_hz_q8 = 55u << 8,  /* A1 — deep */
    .osc1 = { .wave = LFE_SYNTH_WAVE_SQUARE, .detune_hz = 0,
              .level = Q15_ONE, .pulse_width = Q15_HALF },
    .osc2 = { .wave = LFE_SYNTH_WAVE_SAW, .detune_hz = 0,
              .level = Q15_ONE / 2, .pulse_width = Q15_HALF },
    .noise_level    = 0,
    .noise_seed     = 0,
    .filter_mode    = LFE_DRUM_FILTER_LP,
    .filter_base_hz = 400,
    .filter_q       = Q15_HALF,
    .master_level   = Q15_ONE,
    .note_off_sample = 9600,   /* ~300 ms */
    .mods = {
        { .env = { .attack_ms = 1, .decay_ms = 50, .sustain_level = Q15_ONE * 3 / 4,
                   .release_ms = 80, .peak_level = Q15_ONE },
          .target = LFE_SYNTH_MOD_AMP, .depth = Q15_ONE },
        /* Filter env: mild pluck character */
        { .env = { .attack_ms = 0, .decay_ms = 80, .sustain_level = 0,
                   .release_ms = 0, .peak_level = Q15_ONE },
          .target = LFE_SYNTH_MOD_FILTER, .depth = 600 },
        LFE_SYNTH_MOD_SLOT_UNUSED,
        LFE_SYNTH_MOD_SLOT_UNUSED,
    },
};

#endif /* LFE_GEN_SYNTH_PRESETS_H */
