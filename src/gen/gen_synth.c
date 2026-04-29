/*
 * gen_synth.c — Phase 4 subtractive synth generator.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A two-oscillator + noise source feeding a state-variable filter and
 * an amplitude envelope — the classical "subtractive" voice. The
 * modulation system is the same mod-slot design as Phase 3 (drum
 * generator), extended with oscillator-level targets (OSC1_LEVEL,
 * OSC2_LEVEL, PULSE_WIDTH).
 *
 * Oscillators are intentionally *naive* — a phase accumulator plus a
 * cheap per-sample waveform computation, no band-limiting. Aliasing
 * above the filter cutoff is expected; the subtractive filter is what
 * normally tames it. A later PolyBLEP pass can improve the high-note
 * behavior without touching the public API.
 *
 * The filter runs on the full mix (oscillators + noise), not just the
 * noise path like gen_drum.c does. That's the conventional signal flow
 * for subtractive synthesis.
 *
 * Coding style is kept "intrinsic-friendly": Q15×Q15→Q31 patterns are
 * written out explicitly and saturation happens only at the final
 * output. Easy to retrofit with inline-asm wrappers later without
 * touching the algorithm.
 */

#include "lfe.h"

#include "util/envelope.h"
#include "util/filter.h"
#include "util/fixed.h"
#include "util/noise.h"
#include "util/platform.h"
#include "util/wavetable.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern bool lfe_is_initialized(void);

/* Filter cutoff recompute interval — same tradeoff as the drum
 * generator. ~1 ms at 32 kHz. */
#define LFE_SYNTH_CR 32

/* ------------------------------------------------------------------ */
/* Conversions between public and internal types                       */
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

/* ------------------------------------------------------------------ */
/* dB → Q15 linear LUT (Calvario gains)                                */
/*                                                                     */
/* 65 entries covering 0 dB to -64 dB in 1 dB steps. Each entry is     */
/* round(32767 * 10^(-i/20)) — the Q15 linear magnitude corresponding  */
/* to the given dB of attenuation. Verified key values:                */
/*                                                                     */
/*   0 dB → 32767 (unity)                                              */
/*  -6 dB → 16422 (half amplitude)                                     */
/* -20 dB → 3277  (one tenth)                                          */
/* -40 dB → 328   (one hundredth)                                      */
/* -60 dB → 33    (one thousandth)                                     */
/*                                                                     */
/* db_q88_to_q15_linear() below reads this with linear interpolation   */
/* between adjacent whole-dB entries so sub-dB envelope sweeps don't   */
/* click. For a static param the whole-dB quantization is already      */
/* well below audible gain-step perception (~3 dB).                    */
/* ------------------------------------------------------------------ */

static const uint16_t db_to_q15_lut[65] = {
    32767, 29204, 26028, 23196, 20674, 18426, 16422, 14636,
    13045, 11626, 10362,  9234,  8230,  7335,  6538,  5827,
     5193,  4629,  4125,  3677,  3277,  2920,  2603,  2320,
     2067,  1843,  1642,  1464,  1304,  1163,  1036,   923,
      823,   734,   654,   583,   519,   463,   413,   368,
      328,   292,   260,   232,   207,   184,   164,   146,
      130,   116,   104,    92,    82,    73,    65,    58,
       52,    46,    41,    37,    33,    29,    26,    23,
       21,
};

/* Map a Q8.8 signed dB value to a Q15 linear magnitude in [0, Q15_ONE].
 *
 * Input range: dB values below -64 are clamped to -64 dB; positive
 * values (boost) are clamped to 0 dB. The LUT only covers attenuation
 * because Calvario's bit-window interpretation requires the gain to
 * stay in [0, 1] — anything above 1.0 would overflow the operand's
 * bit-width meaningfully and break the XOR semantics.
 *
 * Interpolation is linear between adjacent whole-dB entries. Between,
 * say, -3 dB (23196) and -4 dB (20674), at -3.5 dB (frac = 128/256),
 * the result is round(23196 + (20674 - 23196) * 128 / 256) = 21935.
 * That's within 0.1% of the true geometric mean, which is more than
 * accurate enough for a gain knob. */
LFE_INLINE q15_t db_q88_to_q15_linear(int32_t db_q88)
{
    /* Clamp to the LUT's attenuation range. 0 dB = index 0 = Q15_ONE. */
    if (db_q88 >= 0) return (q15_t)db_to_q15_lut[0];
    /* -64 dB in Q8.8 is -16384. Below that, clamp to the floor entry. */
    if (db_q88 <= -(64 << 8)) return (q15_t)db_to_q15_lut[64];

    int32_t neg = -db_q88;              /* 0 .. 16384  (Q8.8) */
    int32_t idx = neg >> 8;             /* whole dB   (0..63) */
    int32_t frac = neg & 0xFF;          /* fractional, 0..255 */

    int32_t a = (int32_t)db_to_q15_lut[idx];
    int32_t b = (int32_t)db_to_q15_lut[idx + 1];
    /* Lerp: a + (b - a) * frac / 256. (b - a) is negative since the
     * LUT is monotonically decreasing, so this walks toward `b`
     * (more attenuated) as frac increases. */
    int32_t out = a + (((b - a) * frac) >> 8);
    return (q15_t)out;
}

/* ------------------------------------------------------------------ */
/* Oscillator combine                                                  */
/*                                                                     */
/* The original gen_synth hot loop hardcoded a three-term additive mix */
/* of `s1*l1 + s2*l2 + noise*ln`. We now route the osc1/osc2 pair      */
/* through combine_samples() so future modes (HARD_SYNC, FM, RING_MOD, */
/* CALVARIO) can slot in with a single case added below. Noise stays  */
/* as a separate additive term after the combine — it's always just a */
/* static layer regardless of how osc1 and osc2 interact.              */
/*                                                                     */
/* HARD_SYNC is a special case: it doesn't modify the combine math     */
/* itself (the mixed output is still additive), it modifies the osc2   */
/* PHASE between samples by force-resetting it whenever osc1 wraps.    */
/* That phase surgery happens in the hot loop below, not here, because */
/* combine_samples operates on already-generated sample values and     */
/* doesn't own the phase accumulators. The HARD_SYNC case in this      */
/* switch therefore just does the normal additive mix — the sonic      */
/* difference comes entirely from the phase-reset that the caller      */
/* performs.                                                            */
/* ------------------------------------------------------------------ */

/* Returns an int32 in roughly [-2*Q15_ONE, 2*Q15_ONE] for MIX — the
 * pre_filter clamp at the bottom of the hot loop is still responsible
 * for saturating to Q15 before the filter sees the value, so behavior
 * matches the pre-extension code exactly when mode == MIX. Other modes
 * are free to clamp internally if they need to, but must also return
 * an int32 to let the caller do the final saturate. */
LFE_INLINE int32_t combine_samples(q15_t s1, int32_t l1,
                                   q15_t s2, int32_t l2,
                                   lfe_synth_osc_combine mode,
                                   int32_t param1, int32_t param2)
{
    (void)param1; (void)param2;
    switch (mode) {
    case LFE_SYNTH_COMBINE_MIX:
    case LFE_SYNTH_COMBINE_HARD_SYNC:
    case LFE_SYNTH_COMBINE_FM:
    default:
        /* MIX / HARD_SYNC / FM all do the same additive combine at the
         * sample level. HARD_SYNC's sonic difference comes from phase2
         * resets applied in the hot loop; FM's comes from the phase2
         * offset applied before osc_step(osc2) — also in the hot loop. */
        return (((int32_t)s1 * l1) >> 15) +
               (((int32_t)s2 * l2) >> 15);
    case LFE_SYNTH_COMBINE_RING_MOD: {
        /* Classic ring modulation: multiply the two level-scaled osc
         * samples. Output energy lands at sum and difference frequencies
         * only — nothing at the input freqs themselves, which is the
         * characteristic robotic / metallic sound.
         *
         * Per-osc level application is the same as MIX (level-scale
         * each source first, then combine), so zeroing either level
         * silences the output as users expect. Ranges: a and b each
         * stay in [-Q15_ONE, Q15_ONE), their product fits in int32
         * with room to spare, and the final >> 15 brings it back to
         * Q15 range. Both combine_params are unused — ring mod is a
         * pure one-line formula with no tunable depth. */
        int32_t a = ((int32_t)s1 * l1) >> 15;
        int32_t b = ((int32_t)s2 * l2) >> 15;
        return (a * b) >> 15;
    }
    case LFE_SYNTH_COMBINE_CALVARIO: {
        /* Weighted XOR with per-operand dB gains. combine_param1 and
         * combine_param2 are signed Q8.8 dB values for gain1 and gain2
         * respectively; the LUT lerp maps each to a Q15 linear factor
         * in [0, Q15_ONE]. Zero-init means 0 dB → unity gain → full
         * XOR bit-window, which is the loudest/most distorted region.
         *
         * Pipeline:
         *   1. Apply osc levels l1, l2 (same as MIX: OSC_LEVEL mod
         *      targets still work as amplitude control).
         *   2. Apply Calvario bit-window gains g1, g2.
         *   3. XOR the two scaled samples.
         *
         * The XOR is done on the uint16 reinterpretation of the int16
         * bit pattern to avoid the implementation-defined behavior of
         * bitwise XOR on signed integers in C. Result is cast back to
         * int16 — the XOR can legitimately flip the sign bit (two
         * operands with different signs producing an extreme negative
         * result), which is the sonic signature of the distortion
         * region. That's expected, not a bug.
         *
         * Gains from the LUT are always ≥ 0, so sign-bit inversion
         * from a negative gain (the concern the plan memo called out)
         * cannot happen here — the LUT can't emit negative values. */
        q15_t g1 = db_q88_to_q15_linear(param1);
        q15_t g2 = db_q88_to_q15_linear(param2);

        int32_t a_leveled = ((int32_t)s1 * l1) >> 15;
        int32_t b_leveled = ((int32_t)s2 * l2) >> 15;

        int32_t a_windowed = (a_leveled * (int32_t)g1) >> 15;
        int32_t b_windowed = (b_leveled * (int32_t)g2) >> 15;

        uint16_t ua = (uint16_t)(int16_t)a_windowed;
        uint16_t ub = (uint16_t)(int16_t)b_windowed;
        int16_t  xr = (int16_t)(ua ^ ub);

        return (int32_t)xr;
    }
    }
}

/* ------------------------------------------------------------------ */
/* Oscillator step                                                     */
/*                                                                     */
/* Each case is a few integer ops. No tables (except sine which uses   */
/* the existing lfe_wt_sine_lookup). All four shapes are "zero-cost"   */
/* in the sense that the waveform itself is computed from the phase    */
/* accumulator with no extra state.                                    */
/* ------------------------------------------------------------------ */

LFE_INLINE q15_t osc_step(lfe_synth_waveform wave,
                          lfe_phase_t phase,
                          uint16_t    pulse_width_q15)
{
    switch (wave) {
    case LFE_SYNTH_WAVE_SAW: {
        /* Naive saw: map phase>>16 (0..65535) to (-32768..+32767) by
         * subtracting the bias. No band-limiting. */
        int32_t v = (int32_t)(phase >> 16) - 0x8000;
        return (q15_t)v;
    }
    case LFE_SYNTH_WAVE_SQUARE: {
        /* Threshold = pw_q15 × 2 (converts Q15 fraction to a 16-bit
         * comparison against phase_hi). At pw_q15 = Q15_HALF = 16384
         * the threshold lands exactly on 32768 — a 50% duty cycle. */
        uint32_t thr     = (uint32_t)pulse_width_q15 << 1;
        uint32_t phase_h = phase >> 16;
        return phase_h < thr ? (q15_t)Q15_ONE : Q15_NEG_ONE;
    }
    case LFE_SYNTH_WAVE_TRIANGLE: {
        /* Build a triangle from the saw: abs, double, bias. The math
         * may briefly hit +32768, which overflows int16; we cap. */
        int32_t s     = (int32_t)(phase >> 16) - 0x8000;
        int32_t abs_s = s < 0 ? -s : s;
        int32_t tri   = (abs_s << 1) - 0x8000;
        if (tri > Q15_ONE) tri = Q15_ONE;
        return (q15_t)tri;
    }
    case LFE_SYNTH_WAVE_SINE:
        return lfe_wt_sine_lookup(phase);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Preset bank                                                         */
/*                                                                     */
/* The preset literals live in gen_synth_presets.h — this file includes*/
/* them as `static const` so they stay internal to this TU.            */
/* ------------------------------------------------------------------ */

#include "gen_synth_presets.h"


lfe_status lfe_synth_fill_preset(lfe_synth_params *out,
                                 lfe_synth_preset preset)
{
    if (!out) return LFE_ERR_NULL;

    const lfe_synth_params *src = NULL;
    switch (preset) {
    case LFE_SYNTH_PRESET_LEAD:  src = &preset_lead;  break;
    case LFE_SYNTH_PRESET_PAD:   src = &preset_pad;   break;
    case LFE_SYNTH_PRESET_PLUCK: src = &preset_pluck; break;
    case LFE_SYNTH_PRESET_BASS:  src = &preset_bass;  break;
    }
    if (!src) return LFE_ERR_BAD_PARAM;

    *out = *src;
    return LFE_OK;
}

/* ------------------------------------------------------------------ */
/* Hot path                                                            */
/* ------------------------------------------------------------------ */

LFE_HOT
lfe_status lfe_gen_synth(lfe_buffer *out, const lfe_synth_params *p)
{
    if (!out || !out->data || !p) return LFE_ERR_NULL;
    if (!lfe_is_initialized())    return LFE_ERR_NOT_INIT;

    const uint32_t sr = (uint32_t)out->rate;
    if (sr == 0) return LFE_ERR_BAD_PARAM;

    const q24_8_t nyquist_q8 = (q24_8_t)((sr / 2u) << 8);
    if (p->base_hz_q8 >= nyquist_q8) return LFE_ERR_BAD_PARAM;

    /* ------------------------------------------------------------- */
    /* Init envelopes per active mod slot                             */
    /* ------------------------------------------------------------- */

    lfe_env_state env_state[LFE_SYNTH_NUM_MODS];
    bool          env_active[LFE_SYNTH_NUM_MODS];
    bool          any_amp_mod = false;

    for (int j = 0; j < LFE_SYNTH_NUM_MODS; j++) {
        if (p->mods[j].target == LFE_SYNTH_MOD_NONE) {
            env_active[j] = false;
            continue;
        }
        env_active[j] = true;
        lfe_env_params ep;
        copy_env_params(&ep, &p->mods[j].env);
        lfe_env_init(&env_state[j], &ep, sr);
        lfe_env_trigger(&env_state[j]);
        if (p->mods[j].target == LFE_SYNTH_MOD_AMP) any_amp_mod = true;
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
    int32_t last_filter_hz = (int32_t)p->filter_base_hz;

    /* ------------------------------------------------------------- */
    /* Main loop                                                      */
    /* ------------------------------------------------------------- */

    lfe_phase_t phase1 = 0;
    lfe_phase_t phase2 = 0;
    lfe_sample_t *dst  = out->data;
    const uint32_t n   = out->length;
    bool clipped       = false;

    /* Clamp note-off to the buffer length so the comparison below
     * doesn't fire spuriously on wraparound. */
    const uint32_t note_off = (p->note_off_sample > 0 && p->note_off_sample <= n)
                                ? p->note_off_sample : 0;
    bool released = false;

    for (uint32_t i = 0; i < n; i++) {
        /* -------- Note-off: release every active envelope once -------- */
        if (note_off && !released && i >= note_off) {
            for (int j = 0; j < LFE_SYNTH_NUM_MODS; j++) {
                if (env_active[j]) lfe_env_release(&env_state[j]);
            }
            released = true;
        }

        /* -------- Step envelopes, accumulate modulations -------- */
        int32_t amp_mod_q15     = any_amp_mod ? 0 : (int32_t)p->master_level;
        int32_t pitch_mod_hz    = 0;
        int32_t filter_mod_hz   = 0;
        int32_t osc1_level_mod  = 0;
        int32_t osc2_level_mod  = 0;
        int32_t noise_level_mod = 0;
        int32_t pw_mod_q15      = 0;
        int32_t combine_p1_mod  = 0;
        int32_t combine_p2_mod  = 0;

        for (int j = 0; j < LFE_SYNTH_NUM_MODS; j++) {
            if (!env_active[j]) continue;
            int64_t level = (int64_t)lfe_env_step(&env_state[j]);
            int64_t depth = (int64_t)p->mods[j].depth;
            int32_t scaled = (int32_t)((level * depth) >> 15);

            switch (p->mods[j].target) {
            case LFE_SYNTH_MOD_AMP:
                amp_mod_q15 += (scaled * (int32_t)p->master_level) >> 15;
                break;
            case LFE_SYNTH_MOD_PITCH:
                pitch_mod_hz += scaled;
                break;
            case LFE_SYNTH_MOD_FILTER:
                filter_mod_hz += scaled;
                break;
            case LFE_SYNTH_MOD_OSC1_LEVEL:
                osc1_level_mod += scaled;
                break;
            case LFE_SYNTH_MOD_OSC2_LEVEL:
                osc2_level_mod += scaled;
                break;
            case LFE_SYNTH_MOD_NOISE_LEVEL:
                noise_level_mod += scaled;
                break;
            case LFE_SYNTH_MOD_PULSE_WIDTH:
                pw_mod_q15 += scaled;
                break;
            case LFE_SYNTH_MOD_COMBINE_PARAM1:
                combine_p1_mod += scaled;
                break;
            case LFE_SYNTH_MOD_COMBINE_PARAM2:
                combine_p2_mod += scaled;
                break;
            case LFE_SYNTH_MOD_NONE:
                break;
            }
        }

        /* -------- Compute effective oscillator frequencies -------- */
        int64_t base_q8 = (int64_t)p->base_hz_q8 + ((int64_t)pitch_mod_hz << 8);
        if (base_q8 < 0) base_q8 = 0;

        int64_t osc1_q8 = base_q8 + ((int64_t)p->osc1.detune_hz << 8);
        int64_t osc2_q8 = base_q8 + ((int64_t)p->osc2.detune_hz << 8);
        if (osc1_q8 < 0) osc1_q8 = 0;
        if (osc2_q8 < 0) osc2_q8 = 0;
        if (osc1_q8 >= (int64_t)nyquist_q8) osc1_q8 = (int64_t)nyquist_q8 - 1;
        if (osc2_q8 >= (int64_t)nyquist_q8) osc2_q8 = (int64_t)nyquist_q8 - 1;

        /* -------- Compute modulated pulse widths -------- */
        int32_t pw1 = (int32_t)p->osc1.pulse_width + pw_mod_q15;
        int32_t pw2 = (int32_t)p->osc2.pulse_width + pw_mod_q15;
        if (pw1 < 0)       pw1 = 0;
        if (pw1 > Q15_ONE) pw1 = Q15_ONE;
        if (pw2 < 0)       pw2 = 0;
        if (pw2 > Q15_ONE) pw2 = Q15_ONE;

        /* -------- Per-sample combine params (base + mod) -------- */
        int32_t cp1 = p->combine_param1 + combine_p1_mod;
        int32_t cp2 = p->combine_param2 + combine_p2_mod;

        /* -------- Generate oscillator samples -------- */
        q15_t s1 = osc_step(p->osc1.wave, phase1, (uint16_t)pw1);

        /* FM: osc1 is the modulator, osc2 is the carrier. Apply a phase
         * offset to phase2 BEFORE running osc_step so s2 is the carrier
         * sampled at the modulated phase. Depth (combine_param1) is Q15
         * in [0, Q15_ONE]; at full depth the peak phase offset is 4 full
         * cycles (modulation index ~4, moderate DX7 bell/EP territory).
         *
         * Math: s1 × depth is a Q15×Q15 → Q30 product in [-2^30, 2^30].
         * Left-shifting by 4 scales that to ±2^34, which interpreted as
         * a signed phase offset (modulo 2^32) means ±4 full cycles when
         * depth = Q15_ONE. Using int64 intermediate avoids the 2^31
         * overflow the naive int32 path would hit at full swing. */
        lfe_phase_t phase2_for_step = phase2;
        if (p->combine == LFE_SYNTH_COMBINE_FM) {
            int32_t fm_depth = cp1;
            if (fm_depth < 0)       fm_depth = 0;
            if (fm_depth > Q15_ONE) fm_depth = Q15_ONE;
            int64_t pmod = (int64_t)s1 * (int64_t)fm_depth;
            pmod <<= 4;
            phase2_for_step += (lfe_phase_t)(int32_t)pmod;
        }
        q15_t s2 = osc_step(p->osc2.wave, phase2_for_step, (uint16_t)pw2);

        /* Advance phases. Cache prev_phase1 first so HARD_SYNC below
         * can detect a wrap without an extra branch on high frequencies.
         * (For freq < sr/2 the increment is at most UINT32_MAX/2, so at
         * most one wrap per sample — a single less-than is sufficient.) */
        lfe_phase_t prev_phase1 = phase1;
        phase1 += lfe_freq_to_phase_inc((q24_8_t)osc1_q8, sr);
        phase2 += lfe_freq_to_phase_inc((q24_8_t)osc2_q8, sr);

        /* HARD_SYNC: when osc1 (the master) wraps past UINT32_MAX,
         * snap osc2 (the slave) back to phase 0. Unsigned wrap detect:
         * the new phase is smaller than the old one. osc_step on the
         * NEXT sample will see phase2 == 0 and produce the reset
         * waveform, which is the source of the classic sync saw
         * character. Slave offset is always 0 for v1 per the plan. */
        if (p->combine == LFE_SYNTH_COMBINE_HARD_SYNC &&
            phase1 < prev_phase1) {
            phase2 = 0;
        }

        /* -------- Noise -------- */
        q15_t noise_raw = lfe_noise_step(&noise);

        /* -------- Mix oscillators via combine, then add noise -------- */
        int32_t l1 = (int32_t)p->osc1.level  + osc1_level_mod;
        int32_t l2 = (int32_t)p->osc2.level  + osc2_level_mod;
        int32_t ln = (int32_t)p->noise_level + noise_level_mod;
        if (l1 < 0) l1 = 0; if (l1 > Q15_ONE) l1 = Q15_ONE;
        if (l2 < 0) l2 = 0; if (l2 > Q15_ONE) l2 = Q15_ONE;
        if (ln < 0) ln = 0; if (ln > Q15_ONE) ln = Q15_ONE;

        /* cp1 / cp2 are already computed above — FM needs them before
         * osc_step(osc2). They carry through unchanged to combine_samples. */
        int32_t mixed = combine_samples(s1, l1, s2, l2,
                                        p->combine, cp1, cp2);

        /* Noise is a separate additive layer after the osc combine —
         * always a plain mix regardless of combine mode. The final
         * pre_filter clamp below is what actually saturates to Q15
         * before the filter, so MIX-mode behavior is bit-identical to
         * the pre-extension code. */
        int32_t pre_filter = mixed +
                             (((int32_t)noise_raw * ln) >> 15);

        /* -------- Filter the full mix -------- */
        if ((i % LFE_SYNTH_CR) == 0) {
            int32_t cut = (int32_t)p->filter_base_hz + filter_mod_hz;
            if (cut < 20) cut = 20;
            if (cut != last_filter_hz) {
                lfe_filter_set_cutoff(&flt, (uint32_t)cut, sr);
                last_filter_hz = cut;
            }
        }

        /* SVF expects Q15 input; clamp before stepping to keep the
         * filter state from blowing up on the rare mix overshoot. */
        if (pre_filter > Q15_ONE)     pre_filter = Q15_ONE;
        if (pre_filter < -Q15_ONE - 1) pre_filter = -Q15_ONE - 1;
        q15_t filtered = lfe_filter_step(&flt, (q15_t)pre_filter);

        /* -------- Amp -------- */
        if (amp_mod_q15 < 0)       amp_mod_q15 = 0;
        if (amp_mod_q15 > Q15_ONE) amp_mod_q15 = Q15_ONE;
        int32_t final_val = ((int32_t)filtered * amp_mod_q15) >> 15;

        if (final_val >  Q15_ONE)     { final_val =  Q15_ONE;     clipped = true; }
        if (final_val < -Q15_ONE - 1) { final_val = -Q15_ONE - 1; clipped = true; }

        dst[i] = (lfe_sample_t)final_val;
    }

    return clipped ? LFE_WARN_CLIPPED : LFE_OK;
}
