/*
 * gen_fm4_presets.h — Preset bank for the 4-op FM generator.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Intended to be included by exactly one translation unit (gen_fm4.c).
 * Contains `static const lfe_fm4_params` literals — no externs, no
 * init code. Keeping the literals in a separate header lets callers
 * browse the preset bank without scrolling past the hot-path render
 * loop and makes adding/editing presets a pure data edit.
 *
 * See `reference_lfe_modulation_sdk.md` in memory for the shared SDK
 * types these literals use.
 */

#ifndef LFE_GEN_FM4_PRESETS_H
#define LFE_GEN_FM4_PRESETS_H

#include "lfe.h"
#include "util/fixed.h"

/* ------------------------------------------------------------------ */
/* Preset: classic 2-op DX electric piano                             */
/*                                                                     */
/* Uses only op 0 (carrier) and op 1 (modulator at 14x for the bell-  */
/* like attack). Ops 2 and 3 are silent (level 0, carrier_mix 0) and  */
/* don't contribute to the sound — they're available for the user to  */
/* extend the preset in their own edits.                               */
/* ------------------------------------------------------------------ */

#define FM4_EP_BASE_HZ_Q8 (220u << 8)   /* A3 */

static const lfe_fm4_params preset_ep = {
    .base_hz_q8 = FM4_EP_BASE_HZ_Q8,
    .ops = {
        /* Op 0 — carrier at 1.0x, medium-decay amp envelope. */
        { .freq_ratio_q8 = (1u << 8),     /* 1.00 */
          .detune_cents  = 0,
          .level         = Q15_ONE,
          .env = { .attack_ms = 2,   .decay_ms = 400,
                   .sustain_level = Q15_ONE * 3 / 4,
                   .release_ms = 300, .peak_level = Q15_ONE } },
        /* Op 1 — modulator at 14x, SHORT decay: this is where the
         * DX-style "ping" attack comes from. The modulator is loud
         * initially and fades quickly, so you get a bright bell
         * transient on top of a pitched sine carrier. */
        { .freq_ratio_q8 = (14u << 8),    /* 14.00 */
          .detune_cents  = 0,
          .level         = Q15_ONE,
          .env = { .attack_ms = 1,   .decay_ms = 150,
                   .sustain_level = 0,
                   .release_ms = 50,  .peak_level = Q15_ONE } },
        /* Ops 2 and 3 — silent in this preset. Level 0 + carrier_mix 0
         * means they contribute nothing to either the modulation math
         * (their prev_out stays near zero) or the audio output. */
        { .freq_ratio_q8 = (1u << 8), .detune_cents = 0, .level = 0,
          .env = { 0 } },
        { .freq_ratio_q8 = (1u << 8), .detune_cents = 0, .level = 0,
          .env = { 0 } },
    },
    /* Mod matrix: op 1 modulates op 0 at half-strength. All other
     * entries are zero (no other cross-links). Classic 2-op FM
     * topology. [src][dst]: mod_matrix[1][0] = how much op 1's
     * output modulates op 0's phase. */
    .mod_matrix = {
        { 0, 0, 0, 0 },         /* op 0 → {0,1,2,3}: no modulation */
        { Q15_HALF, 0, 0, 0 },  /* op 1 → op 0: bell modulator */
        { 0, 0, 0, 0 },
        { 0, 0, 0, 0 },
    },
    /* Carrier mix: only op 0 is audible. Op 1 is a pure modulator
     * despite its high level — level drives the mod matrix strength,
     * carrier_mix decides audibility. */
    .carrier_mix = { Q15_ONE, 0, 0, 0 },

    /* Release halfway through the nominal 1-second buffer, so the
     * user hears the attack + sustain + release shape cleanly. */
    .note_off_sample = 16000,
};

/* ------------------------------------------------------------------ */
/* Preset: 3-op inharmonic bell                                        */
/*                                                                     */
/* Carrier + two modulators. The primary modulator at 3.5x gives the  */
/* characteristic inharmonic ring (integer ratios sound harmonically  */
/* "clean" like a brass tone; 3.5x is specifically inharmonic and     */
/* produces the metallic spectrum). The secondary at 7x adds high    */
/* shimmer on the attack. Long carrier decay = long ring-out tail.    */
/* ------------------------------------------------------------------ */

static const lfe_fm4_params preset_bell = {
    .base_hz_q8 = 440u << 8,            /* A4 */
    .ops = {
        /* Op 0 — carrier, slow decay (long bell ring). */
        { .freq_ratio_q8 = (1u << 8),
          .detune_cents  = 0,
          .level         = Q15_ONE,
          .env = { .attack_ms = 2,    .decay_ms = 800,
                   .sustain_level = Q15_ONE / 4,
                   .release_ms = 600,  .peak_level = Q15_ONE } },
        /* Op 1 — primary modulator at 3.5x (inharmonic ring).
         * Q8.8 encoding: 3.5 × 256 = 896 = 0x380. */
        { .freq_ratio_q8 = 0x380u,
          .detune_cents  = 0,
          .level         = Q15_ONE,
          .env = { .attack_ms = 1,    .decay_ms = 500,
                   .sustain_level = Q15_ONE / 8,
                   .release_ms = 300,  .peak_level = Q15_ONE } },
        /* Op 2 — secondary shimmer modulator at 7x, short decay. */
        { .freq_ratio_q8 = (7u << 8),
          .detune_cents  = 0,
          .level         = Q15_ONE,
          .env = { .attack_ms = 1,    .decay_ms = 200,
                   .sustain_level = 0,
                   .release_ms = 100,  .peak_level = Q15_ONE } },
        /* Op 3 — unused. */
        { .freq_ratio_q8 = (1u << 8), .detune_cents = 0, .level = 0,
          .env = { 0 } },
    },
    .mod_matrix = {
        /* Only modulators (ops 1 and 2) feed op 0. Op 2 at half
         * strength so its shimmer is audible but not dominant.
         * [src][dst]: row is source, column is target. */
        { 0, 0, 0, 0 },
        { Q15_ONE,  0, 0, 0 },   /* op 1 → op 0 */
        { Q15_HALF, 0, 0, 0 },   /* op 2 → op 0 */
        { 0, 0, 0, 0 },
    },
    .carrier_mix    = { Q15_ONE, 0, 0, 0 },   /* only op 0 audible */
    .note_off_sample = 20000,                  /* ~625 ms */
};

/* ------------------------------------------------------------------ */
/* Preset: 2-op synth bass                                             */
/* ------------------------------------------------------------------ */

static const lfe_fm4_params preset_bass = {
    .base_hz_q8 = 55u << 8,             /* A1 — deep */
    .ops = {
        { .freq_ratio_q8 = (1u << 8),   /* carrier at fundamental */
          .detune_cents  = 0,
          .level         = Q15_ONE,
          .env = { .attack_ms = 1,    .decay_ms = 80,
                   .sustain_level = Q15_ONE * 3 / 4,
                   .release_ms = 120,  .peak_level = Q15_ONE } },
        { .freq_ratio_q8 = (2u << 8),   /* modulator at 2x */
          .detune_cents  = 0,
          .level         = Q15_ONE,
          .env = { .attack_ms = 1,    .decay_ms = 100,
                   .sustain_level = Q15_ONE / 4,
                   .release_ms = 80,   .peak_level = Q15_ONE } },
        { .freq_ratio_q8 = (1u << 8), .detune_cents = 0, .level = 0,
          .env = { 0 } },
        { .freq_ratio_q8 = (1u << 8), .detune_cents = 0, .level = 0,
          .env = { 0 } },
    },
    .mod_matrix = {
        { 0, 0, 0, 0 },
        { Q15_HALF, 0, 0, 0 },          /* op 1 → op 0 moderate */
        { 0, 0, 0, 0 },
        { 0, 0, 0, 0 },
    },
    .carrier_mix    = { Q15_ONE, 0, 0, 0 },
    .note_off_sample = 9600,             /* ~300 ms, short sustain */
};

/* ------------------------------------------------------------------ */
/* Preset: 2-op brass                                                  */
/*                                                                     */
/* Integer harmonic (1:2) so the spectrum is "clean" — brass-like     */
/* rather than inharmonic/bell-like. Slow attack on both ops so the   */
/* timbre opens up over ~50 ms instead of starting with a hard ping.  */
/* ------------------------------------------------------------------ */

static const lfe_fm4_params preset_brass = {
    .base_hz_q8 = 220u << 8,            /* A3 */
    .ops = {
        { .freq_ratio_q8 = (1u << 8),
          .detune_cents  = 0,
          .level         = Q15_ONE,
          .env = { .attack_ms = 40,   .decay_ms = 100,
                   .sustain_level = Q15_ONE * 7 / 8,
                   .release_ms = 300,  .peak_level = Q15_ONE } },
        { .freq_ratio_q8 = (2u << 8),
          .detune_cents  = 0,
          .level         = Q15_ONE,
          .env = { .attack_ms = 60,   .decay_ms = 200,
                   .sustain_level = Q15_ONE * 3 / 4,
                   .release_ms = 300,  .peak_level = Q15_ONE } },
        { .freq_ratio_q8 = (1u << 8), .detune_cents = 0, .level = 0,
          .env = { 0 } },
        { .freq_ratio_q8 = (1u << 8), .detune_cents = 0, .level = 0,
          .env = { 0 } },
    },
    .mod_matrix = {
        { 0, 0, 0, 0 },
        /* Moderate-strong modulation for the characteristic brassy
         * brightness — slightly under unity so the modulator's slow
         * ramp-in gradually opens the timbre rather than slamming it. */
        { (Q15_ONE * 2) / 3, 0, 0, 0 },
        { 0, 0, 0, 0 },
        { 0, 0, 0, 0 },
    },
    .carrier_mix    = { Q15_ONE, 0, 0, 0 },
    .note_off_sample = 22000,            /* ~690 ms sustain */
};

/* ------------------------------------------------------------------ */
/* Preset: 2-op plucked string                                         */
/*                                                                     */
/* No note_off — like gen_synth's PLUCK preset, the amp envelope      */
/* decays to zero naturally. Fast attack, short decay, sustain = 0.   */
/* The modulator gives the string-like spectral richness.             */
/* ------------------------------------------------------------------ */

static const lfe_fm4_params preset_pluck = {
    .base_hz_q8 = 220u << 8,
    .ops = {
        { .freq_ratio_q8 = (1u << 8),
          .detune_cents  = 0,
          .level         = Q15_ONE,
          .env = { .attack_ms = 1,    .decay_ms = 500,
                   .sustain_level = 0,
                   .release_ms = 50,   .peak_level = Q15_ONE } },
        { .freq_ratio_q8 = (3u << 8),   /* harmonic 3x */
          .detune_cents  = 0,
          .level         = Q15_ONE,
          .env = { .attack_ms = 1,    .decay_ms = 250,
                   .sustain_level = 0,
                   .release_ms = 50,   .peak_level = Q15_ONE } },
        { .freq_ratio_q8 = (1u << 8), .detune_cents = 0, .level = 0,
          .env = { 0 } },
        { .freq_ratio_q8 = (1u << 8), .detune_cents = 0, .level = 0,
          .env = { 0 } },
    },
    .mod_matrix = {
        { 0, 0, 0, 0 },
        { Q15_HALF, 0, 0, 0 },
        { 0, 0, 0, 0 },
        { 0, 0, 0, 0 },
    },
    .carrier_mix    = { Q15_ONE, 0, 0, 0 },
    .note_off_sample = 0,                /* no explicit release */
};

/* ------------------------------------------------------------------ */
/* Preset: dubstep wobble bass                                         */
/*                                                                     */
/* Low fundamental + a 2-op FM pair (op0 carrier, op1 modulator).     */
/* LFO 0 is a triangle wobble at ~3 Hz targeting the op1→op0 matrix   */
/* cell — classic "wobble bass" where the FM depth sweeps around the  */
/* baseline. LFO 1 is a slow sine at 0.5 Hz on op0's carrier mix so   */
/* the amplitude also breathes slightly. Long-ish note so you hear    */
/* several LFO cycles within the rendered sample.                      */
/* ------------------------------------------------------------------ */

static const lfe_fm4_params preset_wobble = {
    .base_hz_q8 = 55u << 8,             /* A1 — deep bass */
    .ops = {
        { .freq_ratio_q8 = (1u << 8),
          .detune_cents  = 0,
          .level         = Q15_ONE,
          .env = { .attack_ms = 2,    .decay_ms = 10,
                   .sustain_level = Q15_ONE,
                   .release_ms = 150,  .peak_level = Q15_ONE } },
        { .freq_ratio_q8 = (2u << 8),   /* 2x modulator */
          .detune_cents  = 0,
          .level         = Q15_ONE,
          .env = { .attack_ms = 2,    .decay_ms = 10,
                   .sustain_level = Q15_ONE,
                   .release_ms = 100,  .peak_level = Q15_ONE } },
        { .freq_ratio_q8 = (1u << 8), .detune_cents = 0, .level = 0,
          .env = { 0 } },
        { .freq_ratio_q8 = (1u << 8), .detune_cents = 0, .level = 0,
          .env = { 0 } },
    },
    .mod_matrix = {
        { 0, 0, 0, 0 },
        { Q15_HALF, 0, 0, 0 },          /* baseline op1→op0 at 0.5 */
        { 0, 0, 0, 0 },
        { 0, 0, 0, 0 },
    },
    .carrier_mix    = { Q15_ONE, 0, 0, 0 },
    .note_off_sample = 0,                /* sustain — wobble plays out */
    .lfos = {
        /* LFO 0 — triangle @ 3 Hz, deep depth, modulating mod_matrix[1][0]:
         * target = (src=1 << 2) | (dst=0) = 0x04. */
        { .cfg = { .shape = LFE_LFO_SHAPE_TRIANGLE,
                   .rate_hz_q8 = 3u << 8,
                   .depth = Q15_HALF },
          .dest = LFE_FM4_LFO_DEST_MATRIX_CELL,
          .target = 0x04 },
        /* LFO 1 — sine @ 0.5 Hz, moderate depth, modulating carrier_mix[0]. */
        { .cfg = { .shape = LFE_LFO_SHAPE_SINE,
                   .rate_hz_q8 = (1u << 7),   /* 0.5 Hz = 0.5 << 8 = 0x80 */
                   .depth = Q15_ONE / 4 },
          .dest = LFE_FM4_LFO_DEST_CARRIER_MIX,
          .target = 0 },
    },
};

/* ------------------------------------------------------------------ */
/* Preset: growl lead                                                  */
/*                                                                     */
/* Aggressive square-wave LFO on the modulator→carrier edge produces  */
/* a gated on/off timbre character. A second slow triangle LFO on     */
/* pitch adds a subtle vibrato wobble on top. Higher fundamental than */
/* the wobble bass so it sits as a lead rather than a sub.             */
/* ------------------------------------------------------------------ */

static const lfe_fm4_params preset_growl = {
    .base_hz_q8 = 110u << 8,            /* A2 */
    .ops = {
        { .freq_ratio_q8 = (1u << 8),
          .detune_cents  = 0,
          .level         = Q15_ONE,
          .env = { .attack_ms = 5,    .decay_ms = 20,
                   .sustain_level = Q15_ONE,
                   .release_ms = 200,  .peak_level = Q15_ONE } },
        { .freq_ratio_q8 = (3u << 8),   /* 3x modulator (harmonic) */
          .detune_cents  = 0,
          .level         = Q15_ONE,
          .env = { .attack_ms = 2,    .decay_ms = 20,
                   .sustain_level = Q15_ONE,
                   .release_ms = 150,  .peak_level = Q15_ONE } },
        { .freq_ratio_q8 = (1u << 8), .detune_cents = 0, .level = 0,
          .env = { 0 } },
        { .freq_ratio_q8 = (1u << 8), .detune_cents = 0, .level = 0,
          .env = { 0 } },
    },
    .mod_matrix = {
        { 0, 0, 0, 0 },
        { Q15_ONE / 3, 0, 0, 0 },       /* modest baseline — LFO drives it */
        { 0, 0, 0, 0 },
        { 0, 0, 0, 0 },
    },
    .carrier_mix    = { Q15_ONE, 0, 0, 0 },
    .note_off_sample = 0,
    .lfos = {
        /* LFO 0 — square @ 5 Hz slamming the op1→op0 matrix cell:
         * gated growl rhythm. target = (src=1 << 2) | (dst=0) = 0x04. */
        { .cfg = { .shape = LFE_LFO_SHAPE_SQUARE,
                   .rate_hz_q8 = 5u << 8,
                   .depth = (Q15_ONE * 2) / 3 },
          .dest = LFE_FM4_LFO_DEST_MATRIX_CELL,
          .target = 0x04 },
        /* LFO 1 — triangle @ 4 Hz on pitch for subtle vibrato. Depth
         * is small because pitch mod is aggressive at full depth. */
        { .cfg = { .shape = LFE_LFO_SHAPE_TRIANGLE,
                   .rate_hz_q8 = 4u << 8,
                   .depth = Q15_ONE / 16 },
          .dest = LFE_FM4_LFO_DEST_PITCH,
          .target = 0 },
    },
};

#endif /* LFE_GEN_FM4_PRESETS_H */
