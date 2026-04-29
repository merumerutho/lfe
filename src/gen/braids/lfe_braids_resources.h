/*
 * lfe_braids_resources.h — LUTs and wavetables for the Braids port.
 *
 * Ported from braids/resources.h (MIT, (c) 2012 Emilie Gillet).
 *
 * Scope: only the tables referenced by the 18 shapes selected for the
 * maxtracker Braids port (see project_phase6_braids_plan memory). The
 * following upstream tables are intentionally dropped:
 *
 *   string_table, char_table, chr_characters      — UI / display only
 *   wt_waves, wt_map, wt_code, wt_table           — wavetable shapes we
 *                                                    don't carry (33 KB)
 *   lut_bell                                      — STRUCK_BELL dropped
 *   lut_granular_envelope, ..._rate               — GRANULAR_CLOUD dropped
 *   lut_fm_frequency_quantizer                    — FM shapes dropped
 *   lookup_table_table, *_signed_table, *_hr_table — id-indexed lookup,
 *                                                    unused by oscillator
 *                                                    code (synth code
 *                                                    dereferences LUTs by
 *                                                    name, not id)
 *
 * Array sizes are baked into the extern declarations so that callers
 * (including tests) can use `sizeof(lut_x)/sizeof(*lut_x)` to sanity-
 * check the port.
 */

#ifndef LFE_BRAIDS_RESOURCES_H
#define LFE_BRAIDS_RESOURCES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Size / index constants (reproduced from upstream) ----
 *
 * Declared above the `extern` array decls below so the array sizes
 * can reference them directly. The numeric values are locked by
 * upstream Braids resources.h and must not drift.
 */

#define LUT_RESONATOR_COEFFICIENT_SIZE 129
#define LUT_RESONATOR_SCALE_SIZE 129
#define LUT_SVF_CUTOFF_SIZE 257
#define LUT_SVF_DAMP_SIZE 257
#define LUT_SVF_SCALE_SIZE 257
#define LUT_BOWING_ENVELOPE_SIZE 752
#define LUT_BOWING_FRICTION_SIZE 257
#define LUT_BLOWING_ENVELOPE_SIZE 392
#define LUT_FLUTE_BODY_FILTER_SIZE 128
#define LUT_VCO_DETUNE_SIZE 257
#define LUT_ENV_EXPO_SIZE 257
#define LUT_BLOWING_JET_SIZE 257
#define LUT_OSCILLATOR_INCREMENTS_SIZE 97
#define LUT_OSCILLATOR_DELAYS_SIZE 97
#define LUT_ENV_PORTAMENTO_INCREMENTS_SIZE 128

#define WAV_FORMANT_SINE 0
#define WAV_FORMANT_SINE_SIZE 256
#define WAV_FORMANT_SQUARE 1
#define WAV_FORMANT_SQUARE_SIZE 256
#define WAV_SINE 2
#define WAV_SINE_SIZE 257
#define WAV_BANDLIMITED_COMB_0 3
#define WAV_BANDLIMITED_COMB_0_SIZE 257
#define WAV_BANDLIMITED_COMB_1 4
#define WAV_BANDLIMITED_COMB_2 5
#define WAV_BANDLIMITED_COMB_3 6
#define WAV_BANDLIMITED_COMB_4 7
#define WAV_BANDLIMITED_COMB_5 8
#define WAV_BANDLIMITED_COMB_6 9
#define WAV_BANDLIMITED_COMB_7 10
#define WAV_BANDLIMITED_COMB_8 11
#define WAV_BANDLIMITED_COMB_9 12
#define WAV_BANDLIMITED_COMB_10 13
#define WAV_BANDLIMITED_COMB_11 14
#define WAV_BANDLIMITED_COMB_12 15
#define WAV_BANDLIMITED_COMB_13 16
#define WAV_BANDLIMITED_COMB_14 17

#define WS_MODERATE_OVERDRIVE 0
#define WS_VIOLENT_OVERDRIVE 1
#define WS_SINE_FOLD 2
#define WS_TRI_FOLD 3

/* All four waveshaper tables share one size. */
#define BRAIDS_WS_SIZE 257

/* All bandlimited comb tables share the same size. */
#define BRAIDS_COMB_SIZE WAV_BANDLIMITED_COMB_0_SIZE

/* ---- Small LUTs ---- */
extern const uint16_t lut_resonator_coefficient[LUT_RESONATOR_COEFFICIENT_SIZE];
extern const uint16_t lut_resonator_scale[LUT_RESONATOR_SCALE_SIZE];
extern const uint16_t lut_svf_cutoff[LUT_SVF_CUTOFF_SIZE];
extern const uint16_t lut_svf_damp[LUT_SVF_DAMP_SIZE];
extern const uint16_t lut_svf_scale[LUT_SVF_SCALE_SIZE];
extern const uint16_t lut_bowing_envelope[LUT_BOWING_ENVELOPE_SIZE];
extern const uint16_t lut_bowing_friction[LUT_BOWING_FRICTION_SIZE];
extern const uint16_t lut_blowing_envelope[LUT_BLOWING_ENVELOPE_SIZE];
extern const uint16_t lut_flute_body_filter[LUT_FLUTE_BODY_FILTER_SIZE];
extern const uint16_t lut_vco_detune[LUT_VCO_DETUNE_SIZE];
extern const uint16_t lut_env_expo[LUT_ENV_EXPO_SIZE];
extern const int16_t  lut_blowing_jet[LUT_BLOWING_JET_SIZE];
extern const uint32_t lut_oscillator_increments[LUT_OSCILLATOR_INCREMENTS_SIZE];
extern const uint32_t lut_oscillator_delays[LUT_OSCILLATOR_DELAYS_SIZE];
extern const uint32_t lut_env_portamento_increments[LUT_ENV_PORTAMENTO_INCREMENTS_SIZE];

/* ---- Waveforms ---- */
extern const int16_t wav_formant_sine[WAV_FORMANT_SINE_SIZE];
extern const int16_t wav_formant_square[WAV_FORMANT_SQUARE_SIZE];
extern const int16_t wav_sine[WAV_SINE_SIZE];
extern const int16_t wav_bandlimited_comb_0[BRAIDS_COMB_SIZE];
extern const int16_t wav_bandlimited_comb_1[BRAIDS_COMB_SIZE];
extern const int16_t wav_bandlimited_comb_2[BRAIDS_COMB_SIZE];
extern const int16_t wav_bandlimited_comb_3[BRAIDS_COMB_SIZE];
extern const int16_t wav_bandlimited_comb_4[BRAIDS_COMB_SIZE];
extern const int16_t wav_bandlimited_comb_5[BRAIDS_COMB_SIZE];
extern const int16_t wav_bandlimited_comb_6[BRAIDS_COMB_SIZE];
extern const int16_t wav_bandlimited_comb_7[BRAIDS_COMB_SIZE];
extern const int16_t wav_bandlimited_comb_8[BRAIDS_COMB_SIZE];
extern const int16_t wav_bandlimited_comb_9[BRAIDS_COMB_SIZE];
extern const int16_t wav_bandlimited_comb_10[BRAIDS_COMB_SIZE];
extern const int16_t wav_bandlimited_comb_11[BRAIDS_COMB_SIZE];
extern const int16_t wav_bandlimited_comb_12[BRAIDS_COMB_SIZE];
extern const int16_t wav_bandlimited_comb_13[BRAIDS_COMB_SIZE];
extern const int16_t wav_bandlimited_comb_14[BRAIDS_COMB_SIZE];

/* ---- Waveshapers ---- */
extern const int16_t ws_moderate_overdrive[BRAIDS_WS_SIZE];
extern const int16_t ws_violent_overdrive[BRAIDS_WS_SIZE];
extern const int16_t ws_sine_fold[BRAIDS_WS_SIZE];
extern const int16_t ws_tri_fold[BRAIDS_WS_SIZE];

/* ---- Pointer tables actually used by oscillator code ----
 *
 * analog_oscillator.cc's saw-swarm path indexes waveform_table[] with
 * `WAV_BANDLIMITED_COMB_0 + n`. Entries beyond the comb range (sine,
 * formants) are unused but kept so indices match the upstream layout.
 *
 * Array bound kept for sizeof() completeness: upstream waveform_table
 * has 18 entries (formants + sine + 15 combs), waveshaper_table has 4.
 */
extern const int16_t *waveform_table[18];
extern const int16_t *waveshaper_table[4];

#ifdef __cplusplus
}
#endif

#endif /* LFE_BRAIDS_RESOURCES_H */
