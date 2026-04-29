/*
 * gen_drum.c — Phase 3 drum / percussion generator.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Free-form percussion voice. A fixed signal graph (sine oscillator +
 * filtered LFSR noise, mixed, gained) is driven by N independent
 * envelopes whose outputs are routed to tunable targets via a slot
 * table. "Kick", "snare", "clap" etc. are just preset parameter
 * values — the generator code doesn't know or care what kind of
 * percussion it's building.
 *
 * Per-sample work (offline rendering, not real-time):
 *   - N env steps (~30 cycles total for N=3)
 *   - 1 sine wavetable lookup + 1 phase-inc recompute (~15 cycles)
 *   - 1 noise step (~5 cycles)
 *   - 1 SVF filter step (~20 cycles)
 *   - mix + amp + clip (~10 cycles)
 *
 * Filter cutoff updates run at a control rate of 1 / LFE_DRUM_CR
 * samples because lfe_filter_set_cutoff calls libm sin(). At 32 kHz
 * with LFE_DRUM_CR = 32 that's a cutoff update every ~1 ms, fast
 * enough that the steppy sweep is inaudible on percussion.
 */

#include "lfe.h"

#include "util/envelope.h"
#include "util/filter.h"
#include "util/fixed.h"
#include "util/lfo.h"
#include "util/noise.h"
#include "util/platform.h"
#include "util/wavetable.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declaration of the internal init check from lfe.c. */
extern bool lfe_is_initialized(void);

/* Filter cutoff recompute interval, in samples. See file header. */
#define LFE_DRUM_CR 32

/* ------------------------------------------------------------------ */
/* Public-to-internal conversions                                      */
/* ------------------------------------------------------------------ */

/*
 * Copy a public drum-env-params struct into the internal envelope
 * param shape. Duplicating the type in the public header lets lfe.h
 * stay free of any `util/envelope.h` include; this function is the
 * one place the two shapes meet.
 */
static void copy_env_params(lfe_env_params *dst,
                            const lfe_drum_env_params *src)
{
    dst->attack_ms     = src->attack_ms;
    dst->decay_ms      = src->decay_ms;
    dst->sustain_level = (q15_t)src->sustain_level;
    dst->release_ms    = src->release_ms;
    dst->peak_level    = (q15_t)src->peak_level;
}

static lfe_filter_mode map_filter_mode(lfe_drum_filter_mode m)
{
    switch (m) {
    case LFE_DRUM_FILTER_LP:    return LFE_FILTER_LP;
    case LFE_DRUM_FILTER_HP:    return LFE_FILTER_HP;
    case LFE_DRUM_FILTER_BP:    return LFE_FILTER_BP;
    case LFE_DRUM_FILTER_NOTCH: return LFE_FILTER_NOTCH;
    }
    return LFE_FILTER_LP;
}

/* Tone waveshape lookup. The phase accumulator is a uint32 covering
 * one full cycle; the four shapes interpret it differently. Sine
 * reuses the shared wavetable for band-limit-friendly interpolation;
 * the others are computed directly from phase bits (non-bandlimited,
 * but percussion uses these for their harmonic character anyway). */
static inline q15_t drum_tone_sample(lfe_phase_t phase, uint8_t wave)
{
    switch (wave) {
    case LFE_DRUM_WAVE_SINE:
        return lfe_wt_sine_lookup(phase);

    case LFE_DRUM_WAVE_TRIANGLE: {
        /* Same math as util/lfo.c's triangle — bipolar Q15 ramp. */
        uint32_t p = phase;
        int32_t mag;
        if (p & 0x80000000u) {
            int32_t f = (int32_t)((p & 0x7FFFFFFFu) >> 16);
            mag = (int32_t)Q15_ONE - f * 2;
        } else {
            int32_t f = (int32_t)(p >> 16);
            mag = -(int32_t)Q15_ONE + f * 2;
        }
        if (mag >  Q15_ONE) mag =  Q15_ONE;
        if (mag < -Q15_ONE) mag = -Q15_ONE;
        return (q15_t)mag;
    }

    case LFE_DRUM_WAVE_SQUARE:
        return (phase & 0x80000000u) ? (q15_t)(-Q15_ONE) : (q15_t)Q15_ONE;

    case LFE_DRUM_WAVE_SAW:
        /* Rising saw from -1 to +1 across the cycle. */
        return (q15_t)((int32_t)(phase >> 16) - 0x8000);

    default:
        return lfe_wt_sine_lookup(phase);
    }
}

/* Cubic soft-clip drive. `drive_q15` in [0, Q15_ONE]; 0 passes through
 * unchanged, Q15_ONE applies maximum boost (~2×) before the nonlinear
 * saturation. Output is always in [-Q15_ONE, +Q15_ONE]. */
static inline int32_t drum_apply_drive(int32_t sample, int32_t drive_q15)
{
    if (drive_q15 <= 0) return sample;
    int32_t gain = Q15_ONE + drive_q15;                /* 1..2× in Q15 */
    int32_t x    = ((int64_t)sample * gain) >> 15;
    if (x >  Q15_ONE) x =  Q15_ONE;
    if (x < -Q15_ONE) x = -Q15_ONE;
    /* y = 1.5x - 0.5x³ (tanh-like soft knee). */
    int32_t x_sq    = (x * x) >> 15;
    int32_t x_cubed = (x_sq * x) >> 15;
    return (3 * x - x_cubed) >> 1;
}

/* ------------------------------------------------------------------ */
/* Preset bank                                                         */
/*                                                                     */
/* The preset literals live in gen_drum_presets.h — this file includes*/
/* them as `static const` so they stay internal to this TU.            */
/* ------------------------------------------------------------------ */

#include "gen_drum_presets.h"


lfe_status lfe_drum_fill_preset(lfe_drum_params *out, lfe_drum_preset preset)
{
    if (!out) return LFE_ERR_NULL;

    const lfe_drum_params *src = NULL;
    switch (preset) {
    case LFE_DRUM_PRESET_KICK:       src = &preset_kick;       break;
    case LFE_DRUM_PRESET_SNARE:      src = &preset_snare;      break;
    case LFE_DRUM_PRESET_HAT_CLOSED: src = &preset_hat_closed; break;
    case LFE_DRUM_PRESET_HAT_OPEN:   src = &preset_hat_open;   break;
    case LFE_DRUM_PRESET_TOM:        src = &preset_tom;        break;
    case LFE_DRUM_PRESET_CLAP:       src = &preset_clap;       break;
    case LFE_DRUM_PRESET_KICK_808:   src = &preset_kick_808;   break;
    case LFE_DRUM_PRESET_COWBELL:    src = &preset_cowbell;    break;
    case LFE_DRUM_PRESET_COUNT:      break;  /* not a real preset */
    }
    if (!src) return LFE_ERR_BAD_PARAM;

    *out = *src;
    return LFE_OK;
}

/* ------------------------------------------------------------------ */
/* Hot path                                                            */
/* ------------------------------------------------------------------ */

LFE_HOT
lfe_status lfe_gen_drum(lfe_buffer *out, const lfe_drum_params *p)
{
    if (!out || !out->data || !p)   return LFE_ERR_NULL;
    if (!lfe_is_initialized())      return LFE_ERR_NOT_INIT;

    const uint32_t sr = (uint32_t)out->rate;
    if (sr == 0) return LFE_ERR_BAD_PARAM;

    /* Reject above-Nyquist base tone. Pitch mod can push higher but
     * that's the caller's problem to keep sensible. */
    const q24_8_t nyquist_q8 = (q24_8_t)((sr / 2u) << 8);
    if (p->tone_base_hz_q8 >= nyquist_q8) return LFE_ERR_BAD_PARAM;

    /* ------------------------------------------------------------- */
    /* Init envelopes, one per active mod slot                        */
    /* ------------------------------------------------------------- */

    lfe_env_state env_state[LFE_DRUM_NUM_MODS];
    bool          env_active[LFE_DRUM_NUM_MODS];
    bool          any_amp_mod = false;

    for (int j = 0; j < LFE_DRUM_NUM_MODS; j++) {
        if (p->mods[j].target == LFE_DRUM_MOD_NONE) {
            env_active[j] = false;
            continue;
        }
        env_active[j] = true;
        lfe_env_params ep;
        copy_env_params(&ep, &p->mods[j].env);
        lfe_env_init(&env_state[j], &ep, sr);
        lfe_env_trigger(&env_state[j]);
        if (p->mods[j].target == LFE_DRUM_MOD_AMP) any_amp_mod = true;
    }

    /* ------------------------------------------------------------- */
    /* Init noise + filter                                            */
    /* ------------------------------------------------------------- */

    lfe_noise_state noise;
    lfe_noise_init(&noise, p->noise_seed);

    lfe_filter_state flt;
    lfe_filter_init(&flt, map_filter_mode(p->filter_mode));
    lfe_filter_set_q(&flt, (q15_t)p->filter_q);
    lfe_filter_set_cutoff(&flt, p->filter_base_hz, sr);

    /* Track the last filter cutoff we programmed, so the control-rate
     * update skips the sin() call when the cutoff hasn't moved. */
    int32_t last_filter_hz = (int32_t)p->filter_base_hz;

    /* ------------------------------------------------------------- */
    /* Init LFO (single slot)                                         */
    /* ------------------------------------------------------------- */

    lfe_lfo_state lfo;
    bool lfo_active = (p->lfo.dest != LFE_DRUM_LFO_DEST_OFF) &&
                      (p->lfo.cfg.depth > 0);
    lfe_lfo_init(&lfo,
                 (lfe_lfo_shape)p->lfo.cfg.shape,
                 p->lfo.cfg.rate_hz_q8, sr);

    /* ------------------------------------------------------------- */
    /* Main loop                                                      */
    /* ------------------------------------------------------------- */

    lfe_phase_t phase = 0;
    lfe_sample_t *dst = out->data;
    const uint32_t n  = out->length;
    bool clipped = false;

    for (uint32_t i = 0; i < n; i++) {
        /* Step every active envelope and sort their scaled outputs
         * into the right modulation accumulators. */
        int32_t amp_mod_q15      = any_amp_mod ? 0 : (int32_t)p->master_level;
        int32_t pitch_mod_hz     = 0;
        int32_t filter_mod_hz    = 0;
        int32_t tone_level_mod   = 0;
        int32_t noise_level_mod  = 0;
        int32_t drive_mod        = 0;

        for (int j = 0; j < LFE_DRUM_NUM_MODS; j++) {
            if (!env_active[j]) continue;
            int64_t level = (int64_t)lfe_env_step(&env_state[j]);
            int64_t depth = (int64_t)p->mods[j].depth;
            /* scaled = env_level (Q15) × depth, back-shifted out of Q15.
             * int64 intermediate: depth can legitimately be up to tens
             * of kilohertz for filter sweeps, and Q15 × 65000 would
             * overflow an int32 product. The extra precision on ARM9
             * is cheap for offline rendering. */
            int32_t scaled = (int32_t)((level * depth) >> 15);

            switch (p->mods[j].target) {
            case LFE_DRUM_MOD_AMP:
                /* AMP: the env-scaled depth is Q15; fold master in on
                 * top so the total contribution is env × depth × master
                 * at the envelope peak. */
                amp_mod_q15 += (scaled * (int32_t)p->master_level) >> 15;
                break;
            case LFE_DRUM_MOD_PITCH:       pitch_mod_hz    += scaled; break;
            case LFE_DRUM_MOD_FILTER:      filter_mod_hz   += scaled; break;
            case LFE_DRUM_MOD_TONE_LEVEL:  tone_level_mod  += scaled; break;
            case LFE_DRUM_MOD_NOISE_LEVEL: noise_level_mod += scaled; break;
            case LFE_DRUM_MOD_NONE:        break;
            }
        }

        /* -------- LFO step + dispatch to destination -------- */
        /* Step once per sample even when the destination is OFF? No —
         * phase matters only when the output is consumed, so skipping
         * saves a few cycles and matches FM4's "inactive" handling. */
        if (lfo_active) {
            int32_t raw = (int32_t)lfe_lfo_step(&lfo);
            int32_t mod = (raw * (int32_t)p->lfo.cfg.depth) >> 15;
            switch (p->lfo.dest) {
            case LFE_DRUM_LFO_DEST_TONE_HZ:
                /* ±2 kHz at full depth: scale mod (Q15 ±depth) by 2000. */
                pitch_mod_hz += (mod * 2000) >> 15;
                break;
            case LFE_DRUM_LFO_DEST_TONE_LEVEL:
                tone_level_mod += mod;
                break;
            case LFE_DRUM_LFO_DEST_NOISE_LEVEL:
                noise_level_mod += mod;
                break;
            case LFE_DRUM_LFO_DEST_FILTER_CUT:
                /* ±4 kHz at full depth. */
                filter_mod_hz += (mod * 4000) >> 15;
                break;
            case LFE_DRUM_LFO_DEST_MASTER:
                amp_mod_q15 += (mod * (int32_t)p->master_level) >> 15;
                break;
            case LFE_DRUM_LFO_DEST_DRIVE:
                drive_mod += mod;
                break;
            default: break;
            }
        }

        /* -------- Tone oscillator -------- */
        int64_t tone_hz_q8 = (int64_t)p->tone_base_hz_q8 +
                             ((int64_t)pitch_mod_hz << 8);
        if (tone_hz_q8 < 0)                    tone_hz_q8 = 0;
        if (tone_hz_q8 >= (int64_t)nyquist_q8) tone_hz_q8 = (int64_t)nyquist_q8 - 1;

        q15_t tone_sample = drum_tone_sample(phase, p->tone_wave);
        phase += lfe_freq_to_phase_inc((q24_8_t)tone_hz_q8, sr);

        /* -------- Noise → filter -------- */
        q15_t noise_raw = lfe_noise_step(&noise);

        if ((i % LFE_DRUM_CR) == 0) {
            int32_t cut = (int32_t)p->filter_base_hz + filter_mod_hz;
            if (cut < 20) cut = 20;
            if (cut != last_filter_hz) {
                lfe_filter_set_cutoff(&flt, (uint32_t)cut, sr);
                last_filter_hz = cut;
            }
        }
        q15_t noise_filtered = lfe_filter_step(&flt, noise_raw);

        /* -------- Mix -------- */
        int32_t tl = (int32_t)p->tone_level  + tone_level_mod;
        int32_t nl = (int32_t)p->noise_level + noise_level_mod;
        if (tl < 0) tl = 0; if (tl > Q15_ONE) tl = Q15_ONE;
        if (nl < 0) nl = 0; if (nl > Q15_ONE) nl = Q15_ONE;

        int32_t mixed = (((int32_t)tone_sample     * tl) +
                         ((int32_t)noise_filtered  * nl)) >> 15;

        /* -------- Drive (pre-master soft-clip) -------- */
        int32_t drive_eff = (int32_t)p->drive + drive_mod;
        if (drive_eff < 0)        drive_eff = 0;
        if (drive_eff > Q15_ONE)  drive_eff = Q15_ONE;
        mixed = drum_apply_drive(mixed, drive_eff);

        /* -------- Amp -------- */
        if (amp_mod_q15 < 0)       amp_mod_q15 = 0;
        if (amp_mod_q15 > Q15_ONE) amp_mod_q15 = Q15_ONE;
        int32_t final_val = (mixed * amp_mod_q15) >> 15;

        if (final_val >  Q15_ONE)     { final_val =  Q15_ONE;     clipped = true; }
        if (final_val < -Q15_ONE - 1) { final_val = -Q15_ONE - 1; clipped = true; }

        dst[i] = (lfe_sample_t)final_val;
    }

    return clipped ? LFE_WARN_CLIPPED : LFE_OK;
}
