/*
 * gen_fm4.c — Phase 4c 4-operator FM synth.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Four identical operators (pure sine), routed through a 4x4 phase-
 * modulation matrix and a 4-entry carrier-mix vector. See lfe.h for
 * the API contract and project_fm4op_plan.md for the design memo.
 *
 * Key simplification: EVERY cross-link in the matrix is a one-sample
 * delay. Feedback-on-the-diagonal, forward modulation of later ops,
 * lateral cross-links — all use prev_out[] from the last iteration.
 * This makes the matrix math well-defined regardless of topology
 * (no need for a topological sort or two passes) and introduces 31 µs
 * of delay per edge at 32 kHz, which is sonically undetectable.
 *
 * Offline rendering cost is trivial: ~4 sine lookups + 16 multiplies
 * + 16 accumulates per output sample.
 */

#include "lfe.h"

#include "util/envelope.h"
#include "util/fixed.h"
#include "util/lfo.h"
#include "util/platform.h"
#include "util/wavetable.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern bool lfe_is_initialized(void);

/* ------------------------------------------------------------------ */
/* Env param bridging                                                  */
/* ------------------------------------------------------------------ */

static void copy_env_params(lfe_env_params *dst,
                            const lfe_drum_env_params *src)
{
    dst->attack_ms     = src->attack_ms;
    dst->decay_ms      = src->decay_ms;
    dst->sustain_level = (q15_t)src->sustain_level;
    dst->release_ms    = src->release_ms;
    dst->peak_level    = (q15_t)src->peak_level;
}

/* ------------------------------------------------------------------ */
/* Preset bank                                                         */
/*                                                                     */
/* The preset literals live in gen_fm4_presets.h — this file includes */
/* them as `static const` so they stay internal to this TU (no extern,*/
/* single copy in rodata) while the bank is easy to browse on its own.*/
/* ------------------------------------------------------------------ */

#include "gen_fm4_presets.h"


lfe_status lfe_fm4_fill_preset(lfe_fm4_params *out, lfe_fm4_preset preset)
{
    if (!out) return LFE_ERR_NULL;

    const lfe_fm4_params *src = NULL;
    switch (preset) {
    case LFE_FM4_PRESET_EP:    src = &preset_ep;    break;
    case LFE_FM4_PRESET_BELL:  src = &preset_bell;  break;
    case LFE_FM4_PRESET_BASS:  src = &preset_bass;  break;
    case LFE_FM4_PRESET_BRASS: src = &preset_brass; break;
    case LFE_FM4_PRESET_PLUCK:  src = &preset_pluck;  break;
    case LFE_FM4_PRESET_WOBBLE: src = &preset_wobble; break;
    case LFE_FM4_PRESET_GROWL:  src = &preset_growl;  break;
    default: break;
    }
    if (!src) return LFE_ERR_BAD_PARAM;

    *out = *src;
    return LFE_OK;
}

/* ------------------------------------------------------------------ */
/* Hot path                                                            */
/* ------------------------------------------------------------------ */

/* How much left-shift to apply to the (prev_out * matrix_entry) Q30
 * product before adding it into the phase accumulator. At unity
 * matrix entry and full prev_out, this sets the peak phase offset
 * to 8 full cycles → modulation index ~8, which is bright bell /
 * moderate-to-extreme FM territory. The 2-osc synth's FM combine
 * uses shift-by-4 for a moderate-only index-4 ceiling; the dedicated
 * FM engine pushes one bit further per the plan. */
#define LFE_FM4_MOD_SHIFT 5

/* Helper: clamp a signed int32 to Q15 range. */
static inline int32_t clamp_q15(int32_t v)
{
    if (v >  Q15_ONE)     return  Q15_ONE;
    if (v < -Q15_ONE - 1) return -Q15_ONE - 1;
    return v;
}

/* Compute phase_inc for op i given an effective base pitch. Used both
 * once at init (no LFO pitch mod) and per-sample when an LFO targets
 * PITCH. */
static inline lfe_phase_t fm4_op_phase_inc(const lfe_fm4_params *p,
                                           int i,
                                           q24_8_t base_hz_q8,
                                           q24_8_t nyquist_q8,
                                           uint32_t sr)
{
    uint64_t ratio = (uint64_t)p->ops[i].freq_ratio_q8;
    if (ratio == 0) ratio = (1u << 8);
    uint64_t eff_q8 = ((uint64_t)base_hz_q8 * ratio) >> 8;
    if (eff_q8 >= (uint64_t)nyquist_q8)
        eff_q8 = (uint64_t)nyquist_q8 - 1;
    return lfe_freq_to_phase_inc((q24_8_t)eff_q8, sr);
}

LFE_HOT
lfe_status lfe_gen_fm4(lfe_buffer *out, const lfe_fm4_params *p)
{
    if (!out || !out->data || !p) return LFE_ERR_NULL;
    if (!lfe_is_initialized())    return LFE_ERR_NOT_INIT;

    const uint32_t sr = (uint32_t)out->rate;
    if (sr == 0) return LFE_ERR_BAD_PARAM;

    const q24_8_t nyquist_q8 = (q24_8_t)((sr / 2u) << 8);
    if (p->base_hz_q8 >= nyquist_q8) return LFE_ERR_BAD_PARAM;

    /* ---- Init envelopes for each op ---- */
    lfe_env_state env[LFE_FM4_NUM_OPS];
    for (int i = 0; i < LFE_FM4_NUM_OPS; i++) {
        lfe_env_params ep;
        copy_env_params(&ep, &p->ops[i].env);
        lfe_env_init(&env[i], &ep, sr);
        lfe_env_trigger(&env[i]);
    }

    /* ---- Init LFOs ---- */
    lfe_lfo_state lfo_state[LFE_FM4_NUM_LFOS];
    bool lfo_active[LFE_FM4_NUM_LFOS];
    bool any_pitch_lfo = false;
    for (int k = 0; k < LFE_FM4_NUM_LFOS; k++) {
        lfo_active[k] = (p->lfos[k].dest != LFE_FM4_LFO_DEST_OFF) &&
                        (p->lfos[k].cfg.depth > 0);
        lfe_lfo_init(&lfo_state[k],
                     (lfe_lfo_shape)p->lfos[k].cfg.shape,
                     p->lfos[k].cfg.rate_hz_q8, sr);
        if (lfo_active[k] && p->lfos[k].dest == LFE_FM4_LFO_DEST_PITCH)
            any_pitch_lfo = true;
    }

    /* ---- Precompute baseline phase increments (no pitch mod) ---- */
    lfe_phase_t phase_inc[LFE_FM4_NUM_OPS];
    for (int i = 0; i < LFE_FM4_NUM_OPS; i++)
        phase_inc[i] = fm4_op_phase_inc(p, i, p->base_hz_q8, nyquist_q8, sr);

    /* ---- Per-op state ---- */
    lfe_phase_t phase[LFE_FM4_NUM_OPS] = { 0, 0, 0, 0 };
    q15_t       prev_out[LFE_FM4_NUM_OPS] = { 0, 0, 0, 0 };
    q15_t       curr_out[LFE_FM4_NUM_OPS];

    lfe_sample_t  *dst = out->data;
    const uint32_t n   = out->length;
    bool clipped       = false;

    const uint32_t note_off = (p->note_off_sample > 0 &&
                               p->note_off_sample <= n)
                              ? p->note_off_sample : 0;
    bool released = false;

    for (uint32_t s = 0; s < n; s++) {
        /* ---- Note-off: release all envelopes once ---- */
        if (note_off && !released && s >= note_off) {
            for (int i = 0; i < LFE_FM4_NUM_OPS; i++)
                lfe_env_release(&env[i]);
            released = true;
        }

        /* ---- Step envelopes (one per op) ---- */
        int32_t env_val[LFE_FM4_NUM_OPS];
        for (int i = 0; i < LFE_FM4_NUM_OPS; i++)
            env_val[i] = (int32_t)lfe_env_step(&env[i]);

        /* ---- Step LFOs + build effective (modulated) params ---- */
        /* Start from the baseline params, then apply each active LFO's
         * scaled output to its destination. Effective phase increments
         * are recomputed only when a pitch LFO is active. */
        int32_t eff_level[LFE_FM4_NUM_OPS];
        int32_t eff_carrier[LFE_FM4_NUM_OPS];
        int32_t eff_matrix[LFE_FM4_NUM_OPS][LFE_FM4_NUM_OPS];
        int32_t pitch_delta_q8 = 0;   /* signed delta to add to base_hz_q8 */
        for (int i = 0; i < LFE_FM4_NUM_OPS; i++) {
            eff_level[i]   = p->ops[i].level;
            eff_carrier[i] = p->carrier_mix[i];
            for (int j = 0; j < LFE_FM4_NUM_OPS; j++)
                eff_matrix[i][j] = p->mod_matrix[i][j];
        }
        for (int k = 0; k < LFE_FM4_NUM_LFOS; k++) {
            if (!lfo_active[k]) continue;
            int32_t raw = (int32_t)lfe_lfo_step(&lfo_state[k]);
            int32_t mod = (raw * (int32_t)p->lfos[k].cfg.depth) >> 15;

            const uint8_t t = p->lfos[k].target;
            switch (p->lfos[k].dest) {
            case LFE_FM4_LFO_DEST_OP_LEVEL: {
                int idx = t & 3;
                /* level is unsigned Q15 [0, Q15_ONE] — clamp the
                 * modulated result to that range. */
                int32_t v = eff_level[idx] + mod;
                if (v < 0)        v = 0;
                if (v > Q15_ONE)  v = Q15_ONE;
                eff_level[idx] = v;
                break;
            }
            case LFE_FM4_LFO_DEST_MATRIX_CELL: {
                int src = (t >> 2) & 3;
                int dstc = t & 3;
                eff_matrix[src][dstc] =
                    clamp_q15(eff_matrix[src][dstc] + mod);
                break;
            }
            case LFE_FM4_LFO_DEST_CARRIER_MIX: {
                int idx = t & 3;
                eff_carrier[idx] = clamp_q15(eff_carrier[idx] + mod);
                break;
            }
            case LFE_FM4_LFO_DEST_PITCH: {
                /* ±50% pitch swing at depth=Q15_ONE, mod=±Q15_ONE.
                 * delta = mod * base / (Q15_ONE * 2) ≈ mod * base >> 16. */
                int64_t delta = ((int64_t)mod * (int64_t)p->base_hz_q8) >> 16;
                pitch_delta_q8 += (int32_t)delta;
                break;
            }
            default: /* OFF / unknown */ break;
            }
        }

        /* If any LFO targets pitch this sample, recompute phase_inc. */
        if (any_pitch_lfo && pitch_delta_q8 != 0) {
            int64_t eff_base = (int64_t)p->base_hz_q8 + pitch_delta_q8;
            if (eff_base < 1) eff_base = 1;
            if (eff_base >= (int64_t)nyquist_q8)
                eff_base = (int64_t)nyquist_q8 - 1;
            for (int i = 0; i < LFE_FM4_NUM_OPS; i++)
                phase_inc[i] = fm4_op_phase_inc(p, i,
                                                (q24_8_t)eff_base,
                                                nyquist_q8, sr);
        }

        /* ---- Compute each op's sample for this frame ---- */
        /* Matrix-weighted modulation input from PREVIOUS sample's
         * outputs (one-sample delay on every edge). Uses eff_matrix so
         * LFO cell modulation is in effect this sample. */
        for (int i = 0; i < LFE_FM4_NUM_OPS; i++) {
            int64_t pmod = 0;
            for (int j = 0; j < LFE_FM4_NUM_OPS; j++) {
                pmod += (int64_t)prev_out[j] *
                        (int64_t)eff_matrix[j][i];
            }
            pmod <<= LFE_FM4_MOD_SHIFT;

            lfe_phase_t effective_phase =
                phase[i] + (lfe_phase_t)(int32_t)pmod;
            q15_t raw_sine = lfe_wt_sine_lookup(effective_phase);

            /* Scale by effective op level, then by envelope amplitude. */
            int32_t scaled =
                ((int32_t)raw_sine * eff_level[i]) >> 15;
            scaled = (scaled * env_val[i]) >> 15;

            curr_out[i] = (q15_t)scaled;
        }

        /* ---- Advance all phase accumulators ---- */
        for (int i = 0; i < LFE_FM4_NUM_OPS; i++)
            phase[i] += phase_inc[i];

        /* ---- Final mix via effective carrier_mix ---- */
        int32_t mix = 0;
        for (int i = 0; i < LFE_FM4_NUM_OPS; i++) {
            mix += ((int32_t)curr_out[i] * eff_carrier[i]) >> 15;
        }

        if (mix >  Q15_ONE)     { mix =  Q15_ONE;     clipped = true; }
        if (mix < -Q15_ONE - 1) { mix = -Q15_ONE - 1; clipped = true; }

        dst[s] = (lfe_sample_t)mix;

        /* ---- Shift current → previous for next iteration ---- */
        for (int i = 0; i < LFE_FM4_NUM_OPS; i++)
            prev_out[i] = curr_out[i];
    }

    return clipped ? LFE_WARN_CLIPPED : LFE_OK;
}
