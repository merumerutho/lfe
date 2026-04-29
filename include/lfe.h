/*
 * lfe.h — LFE (Lightweight Fixed-point Engine), public API.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The LFE (Lightweight Fixed-point Engine) produces sample PCM data offline. It is
 * intentionally portable C — no libnds, no NDS hardware registers, no
 * dependency on the rest of maxtracker. Callers allocate buffers, fill
 * out parameter structs, and call the library's generator functions to
 * write PCM into the buffers.
 *
 * The library is consumed in two contexts:
 *   - From maxtracker on Nintendo DS (when MAXTRACKER_LFE is defined),
 *     linked as liblfe.a built with devkitARM.
 *   - From the host test harness, linked as liblfe.a built with the
 *     system gcc and exercised by the standalone test binary.
 *
 * Build it with `make -C lib/lfe` for the host build, or via the top-
 * level `make lfe-test` to also run the test suite.
 */

#ifndef LFE_H
#define LFE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Common types                                                        */
/* ------------------------------------------------------------------ */

/*
 * The library produces signed 16-bit mono samples in [-32768, 32767].
 * Quantization to 8-bit (or any other downconversion) is the caller's
 * responsibility — keeping it out of the library lets each caller
 * decide when to do it (e.g. only on save, or only for old hardware).
 */
typedef int16_t lfe_sample_t;

/* Q15 fixed-point constants for the public API. The internal library
 * has its own Q15_ONE in util/fixed.h — these are the counterparts
 * for editor/caller code that includes lfe.h but not the internal
 * headers. Callers use these when constructing params structs (e.g.
 * setting synth osc levels, FM depths, Calvario dB gains). */
#define LFE_Q15_ONE   ((int16_t)0x7FFF)   /* +1.0 in Q15 =  32767 */
#define LFE_Q15_HALF  ((int16_t)0x4000)   /* +0.5 in Q15 =  16384 */
#define LFE_Q15_ZERO  ((int16_t)0)        /*  0.0 in Q15 */

/* Supported output sample rates. Phase 0 only validates 32 kHz; the
 * lower rates will be honored once the generators stabilize. */
typedef enum {
    LFE_RATE_8000  = 8000,
    LFE_RATE_16000 = 16000,
    LFE_RATE_32000 = 32000,
} lfe_rate;

/*
 * Caller-owned output buffer.
 *
 * `data`   — caller allocates and owns; the library writes into it
 * `length` — number of samples (NOT bytes) in `data`
 * `rate`   — sample rate the buffer is intended to be played at
 */
typedef struct {
    lfe_sample_t *data;
    uint32_t      length;
    lfe_rate      rate;
} lfe_buffer;

/*
 * Return codes. Negative values are hard errors, zero is success,
 * positive values are warnings (the operation succeeded but something
 * worth noting happened, e.g. clipping during generation).
 */
typedef enum {
    LFE_OK              =  0,
    LFE_WARN_CLIPPED    =  1,

    LFE_ERR_NULL        = -1,
    LFE_ERR_BAD_PARAM   = -2,
    LFE_ERR_BUF_TOO_SMALL = -3,
    LFE_ERR_NOT_INIT    = -4,
} lfe_status;

/* ------------------------------------------------------------------ */
/* Shared modulation primitives (used by multiple engines)             */
/*                                                                     */
/* These are the SDK building blocks every engine with envelope- or    */
/* LFO-driven modulation reaches for. The internal implementation      */
/* (stepping, state machines) lives in util/envelope.h + util/lfo.h;   */
/* the structs here are the *config* shape the caller fills in.        */
/*                                                                     */
/* Engines that use these primitives:                                  */
/*   - Drum  (lfe_gen_drum): 3 envelope mod slots + 1 global LFO       */
/*   - Synth (lfe_gen_synth): envelope mod slots + filter env          */
/*   - FM4   (lfe_gen_fm4):  per-op ADSR + 2 global LFOs               */
/*                                                                     */
/* Braids does NOT use these — it's a port of a third-party engine     */
/* with its own internal modulation, exposed via a different API.      */
/* ------------------------------------------------------------------ */

/*
 * Classic ADSR envelope parameters. Percussion patches typically set
 * sustain_level = 0 and release_ms = 0 (one-shot fire-and-decay);
 * sustained sounds use non-zero sustain/release and trigger release
 * at `note_off_sample`.
 *
 * Historically spelled `lfe_drum_env_params` — that name is retained
 * as a typedef alias below for existing code, but new code should use
 * `lfe_adsr_params` directly.
 */
typedef struct {
    uint32_t attack_ms;
    uint32_t decay_ms;
    uint16_t sustain_level;   /* gain-class (Q15); UI: lfe_db_to_q15() */
    uint32_t release_ms;
    uint16_t peak_level;      /* gain-class (Q15); UI: lfe_db_to_q15() */
} lfe_adsr_params;

/* Legacy alias — same struct, original name. Prefer lfe_adsr_params
 * in new code. */
typedef lfe_adsr_params lfe_drum_env_params;

/*
 * LFO waveshape. The shared set every engine's LFO slot picks from.
 */
typedef enum {
    LFE_LFO_SHAPE_SINE = 0,
    LFE_LFO_SHAPE_TRIANGLE,
    LFE_LFO_SHAPE_SQUARE,
    LFE_LFO_SHAPE_SAW_UP,
    LFE_LFO_SHAPE_SAW_DOWN,
    LFE_LFO_SHAPE_COUNT
} lfe_lfo_shape;

/*
 * LFO oscillator config — the "shape / rate / depth" triplet that is
 * destination-agnostic. Each engine embeds one or more of these in
 * its params struct alongside engine-specific dest + target fields,
 * which is what lets the *shape* of an LFO stay uniform across
 * engines while the *routing* remains domain-specific.
 *
 * Depth semantics are destination-relative: Q15_ONE = "full-swing for
 * this destination" as documented by the owning engine (typically a
 * ±1.0 signed excursion for amp-class destinations, and a fixed ±Hz
 * range for pitch/filter destinations).
 */
typedef struct {
    uint8_t  shape;        /* lfe_lfo_shape */
    uint8_t  _pad0;
    uint16_t _pad1;
    uint32_t rate_hz_q8;   /* Q24.8 Hz; 0..32 Hz typical. 0 holds phase. */
    uint16_t depth;        /* Q15 [0, Q15_ONE]; 0 disables the slot. */
    uint16_t _pad2;
} lfe_lfo_params;

/* ------------------------------------------------------------------ */
/* Generator registry                                                  */
/* ------------------------------------------------------------------ */

/*
 * Central list of built-in generators. Used by host UIs (maxtracker's
 * Waveform Editor) to present a "pick a generator" menu without each
 * caller maintaining its own parallel name table. The numeric id is
 * stable — append only; never renumber, never remove.
 *
 * Each generator still has its own bespoke params struct and entry
 * point (lfe_gen_drum / lfe_gen_synth / …); this registry does NOT
 * unify dispatch. It's a label table.
 *
 * Columns: LFE_GEN(id, name, short_name, description)
 */
#define LFE_GEN_LIST(X) \
    X(TEST_TONE, "Test Tone", "TONE",  "Pure sine / square / saw reference tones.")             \
    X(DRAWN,     "Drawn",     "DRAW",  "User-drawn waveform with optional bandlimit / smooth.")  \
    X(DRUM,      "Drum",      "DRUM",  "Analog-drum preset bank (kick, snare, hat, clap …).")    \
    X(SYNTH,     "Synth",     "SYNTH", "Dual-osc subtractive synth with filter + ADSR.")         \
    X(FM4,       "FM 4-op",   "FM4",   "4-operator FM with free 4×4 phase-mod matrix.")  \
    X(BRAIDS,    "Braids",    "BRAIDS", "18 macro shapes ported from Mutable Instruments Braids.")

typedef enum {
#define X(id, name, short_name, desc) LFE_GEN_##id,
    LFE_GEN_LIST(X)
#undef X
    LFE_GEN_COUNT
} lfe_gen_id;

/* Registry row. `name` is the menu label; `short_name` is for tight
 * status lines; `description` is a one-liner for tooltips / help. */
typedef struct {
    lfe_gen_id  id;
    const char *name;
    const char *short_name;
    const char *description;
} lfe_gen_info;

/* Lookup by id. Returns NULL for out-of-range values. */
const lfe_gen_info *lfe_gen_lookup(lfe_gen_id id);

/* Convenience accessors that never crash on bad ids — they return "?"
 * (name/short_name) or "" (description) as a graceful placeholder. */
const char *lfe_gen_name(lfe_gen_id id);
const char *lfe_gen_short_name(lfe_gen_id id);
const char *lfe_gen_description(lfe_gen_id id);

/* ------------------------------------------------------------------ */
/* Library lifecycle                                                   */
/* ------------------------------------------------------------------ */

/*
 * Initialize the library. Builds wavetables and other one-time state.
 * Must be called once before any generator function. Idempotent — safe
 * to call multiple times.
 */
lfe_status lfe_init(void);

/*
 * Release any library state. Optional; the library has no destructive
 * resources, so calling shutdown is mostly a defensive habit.
 */
lfe_status lfe_shutdown(void);

/*
 * Library version string. The format is "MAJOR.MINOR.PATCH" and the
 * value is a static literal — no need to free.
 */
const char *lfe_version(void);

/* ------------------------------------------------------------------ */
/* Phase 0: Test tone                                                  */
/* ------------------------------------------------------------------ */

/*
 * Test tone parameters. The frequency is in Q24.8 fixed-point Hz so
 * the caller can specify sub-Hz precision (useful for tuning tests).
 * For a 440 Hz tone, pass freq_hz_q8 = 440 << 8 = 112640.
 *
 * Amplitude is Q15: 0x7FFF = full scale, 0x4000 = half, etc.
 */
typedef struct {
    uint32_t freq_hz_q8;
    uint16_t amplitude_q15;  /* gain-class (Q15); UI: lfe_db_to_q15() */
} lfe_test_tone_params;

/*
 * Generate a test tone (sine wave) into the provided buffer.
 *
 * Writes exactly `out->length` samples. Phase resets at the start of
 * the buffer, so calling this twice with the same params produces
 * byte-identical output (determinism).
 *
 * Returns:
 *   LFE_OK            on success
 *   LFE_ERR_NULL      if out, out->data, or p is NULL
 *   LFE_ERR_BAD_PARAM if frequency exceeds Nyquist for the given rate
 *   LFE_ERR_NOT_INIT  if lfe_init() has not been called
 */
lfe_status lfe_gen_test_tone(lfe_buffer *out,
                             const lfe_test_tone_params *p);

/* ------------------------------------------------------------------ */
/* Phase 2: Drawn waveform                                             */
/* ------------------------------------------------------------------ */

/*
 * The drawn-waveform generator turns a user-supplied "canvas" into a
 * sample. The canvas is a flat array of signed 8-bit values in the
 * range [-128, +127], one full cycle of the desired waveform. Output
 * is one int16 sample per canvas point, scaled by `<< 8`.
 *
 * Unlike the test-tone generator, the drawn waveform has no duration
 * parameter — it produces a fixed-length sample equal to the canvas
 * length. The intended use is to set the resulting sample as a
 * looping period; the playback rate determines the pitch when the
 * sample is triggered.
 *
 * Caller workflow:
 *   1. Allocate an int8 canvas (typically 256 points)
 *   2. Optionally fill it with a preset via lfe_drawn_fill_preset
 *   3. Modify points by hand (e.g. via touchscreen drawing in the UI)
 *   4. Allocate an int16 output buffer of the same length
 *   5. Call lfe_gen_drawn to convert the canvas to int16 PCM
 *   6. Commit the result to wherever the sample lives
 */

/* Built-in canvas presets — useful starting points for drawing. */
typedef enum {
    LFE_DRAWN_PRESET_SINE     = 0,
    LFE_DRAWN_PRESET_SAW,
    LFE_DRAWN_PRESET_SQUARE,
    LFE_DRAWN_PRESET_TRIANGLE,
    LFE_DRAWN_PRESET_NOISE,
} lfe_drawn_preset;

/*
 * Fill a canvas buffer with a preset waveform. The canvas is treated
 * as one full cycle: a sine preset is one period of sin, a saw goes
 * from -128 at index 0 to +127 at the last index, etc. The noise
 * preset is deterministic (uses a fixed-seed LFSR) so calling it
 * twice on identically-sized canvases produces identical output.
 *
 * Returns:
 *   LFE_OK            on success
 *   LFE_ERR_NULL      if canvas is NULL
 *   LFE_ERR_BAD_PARAM if length is 0 or preset is unknown
 */
lfe_status lfe_drawn_fill_preset(int8_t *canvas, uint32_t length,
                                 lfe_drawn_preset preset);

typedef struct {
    const int8_t *canvas;       /* signed 8-bit canvas, -128..127 */
    uint32_t      canvas_length; /* number of canvas points */
} lfe_drawn_params;

/*
 * Generate an int16 sample from a drawn canvas. The output buffer
 * length must equal canvas_length; each canvas point is scaled to
 * int16 by left-shifting 8 bits (so -128 → -32768, +127 → +32512).
 *
 * Returns:
 *   LFE_OK                 on success
 *   LFE_ERR_NULL           if any required pointer is NULL
 *   LFE_ERR_BAD_PARAM      if canvas_length is 0
 *   LFE_ERR_BUF_TOO_SMALL  if out->length != p->canvas_length
 */
lfe_status lfe_gen_drawn(lfe_buffer *out, const lfe_drawn_params *p);

/* ------------------------------------------------------------------ */
/* Phase 3: Drum / percussion generator                                */
/* ------------------------------------------------------------------ */

/*
 * The drum generator is free-form: it has a fixed signal graph (sine
 * oscillator + white-noise-through-filter, mixed and amplified) but
 * each of the tunable knobs on that graph can be driven by any of
 * LFE_DRUM_NUM_MODS independent envelopes. "Kick", "snare", "hat"
 * etc. are just preset parameter values — the generator itself does
 * not care what kind of drum is being built.
 *
 *
 *   ┌─ sine osc (tone_base + pitch mod) ─┐
 *   │                                    ├── mix ── master × amp mod ── out
 *   └─ LFSR noise ── SVF (cutoff mod) ───┘
 *
 * Modulation slots: each slot holds a simple ADSR-shaped envelope plus
 * a (target, depth) pair. The envelope runs once per generator call
 * (triggered at sample 0, no note-off), and its Q15 output is multiplied
 * by `depth` and summed into whichever parameter `target` names.
 *
 * A typical kick uses two slots: one AMP envelope for body, one PITCH
 * envelope (attack=0, decay=fast) to sweep the tone from a high thump
 * frequency down to the base.
 */

/* Number of modulation slots available per drum. Fixed at compile time
 * so lfe_drum_params has no hidden allocations. Three is enough to cover
 * every preset shape (amp + pitch + filter, or amp + two amp transients
 * for a clap). */
#define LFE_DRUM_NUM_MODS 3

/* Modulation targets. NONE disables a slot (its envelope is not even
 * stepped). All other targets accumulate: if two slots target AMP,
 * their scaled outputs add. */
typedef enum {
    LFE_DRUM_MOD_NONE        = 0,
    LFE_DRUM_MOD_AMP,           /* master amplitude (Q15) */
    LFE_DRUM_MOD_PITCH,         /* tone frequency (Hz shift) */
    LFE_DRUM_MOD_FILTER,        /* noise filter cutoff (Hz shift) */
    LFE_DRUM_MOD_TONE_LEVEL,    /* tone mix level (Q15) */
    LFE_DRUM_MOD_NOISE_LEVEL,   /* noise mix level (Q15) */
} lfe_drum_mod_target;

/* Filter modes, mirrored from the internal filter module so lfe.h
 * stays free of any `util/filter.h` include. The numeric values happen
 * to match but the generator maps them explicitly, so future drift is
 * safe. */
typedef enum {
    LFE_DRUM_FILTER_LP    = 0,
    LFE_DRUM_FILTER_HP,
    LFE_DRUM_FILTER_BP,
    LFE_DRUM_FILTER_NOTCH,
} lfe_drum_filter_mode;

/* Tone oscillator waveshape. Sine is the default (matches the pre-
 * waveshape behavior); triangle is softer, square/saw add brightness
 * and are great for analog-style tones. */
typedef enum {
    LFE_DRUM_WAVE_SINE     = 0,
    LFE_DRUM_WAVE_TRIANGLE,
    LFE_DRUM_WAVE_SQUARE,
    LFE_DRUM_WAVE_SAW,
    LFE_DRUM_WAVE_COUNT
} lfe_drum_wave;

/* Drum LFO destination. Modulation is additive with clamping.
 * Paired with a shared `lfe_lfo_params` in lfe_drum_lfo below. */
typedef enum {
    LFE_DRUM_LFO_DEST_OFF = 0,
    LFE_DRUM_LFO_DEST_TONE_HZ,      /* Hz shift on tone osc (±2 kHz at full depth) */
    LFE_DRUM_LFO_DEST_TONE_LEVEL,   /* Q15 tone mix level */
    LFE_DRUM_LFO_DEST_NOISE_LEVEL,  /* Q15 noise mix level */
    LFE_DRUM_LFO_DEST_FILTER_CUT,   /* Hz shift on filter cutoff (±4 kHz at full depth) */
    LFE_DRUM_LFO_DEST_MASTER,       /* Q15 final amp (post-mix) */
    LFE_DRUM_LFO_DEST_DRIVE,        /* Q15 pre-master drive amount */
    LFE_DRUM_LFO_DEST_COUNT
} lfe_drum_lfo_dest;

/* Drum LFO slot: shared oscillator config + drum-specific dest.
 * Destination semantics for the depth field are documented on the
 * `lfe_drum_lfo_dest` enum members above. */
typedef struct {
    lfe_lfo_params cfg;    /* shape / rate / depth — shared SDK type */
    uint8_t        dest;   /* lfe_drum_lfo_dest */
    uint8_t        _pad0;
    uint16_t       _pad1;
} lfe_drum_lfo;

/*
 * One modulation slot: an envelope, a target parameter, and a signed
 * depth. The depth's unit depends on the target:
 *
 *   AMP, TONE_LEVEL, NOISE_LEVEL → Q15 (signed, [-32767, +32767])
 *   PITCH, FILTER                → integer Hz, signed
 *
 * With target = NONE, the slot is ignored and its envelope is not run.
 */
typedef struct {
    lfe_drum_env_params env;
    lfe_drum_mod_target target;
    int32_t             depth;
} lfe_drum_mod;

/*
 * Drum parameters. All the static (non-modulated) values live in the
 * top-level fields; the mods[] array supplies the time-varying
 * modulation on top of them.
 *
 * tone_base_hz_q8 / filter_base_hz are the "resting" values of the
 * oscillator frequency and the filter cutoff. A PITCH or FILTER
 * modulation slot adds (env_level_q15 * depth) >> 15 to the base on
 * each sample — so for a kick drum you set tone_base = 60 Hz and give
 * slot 1 a PITCH mod with depth = +200 Hz, producing a sweep from
 * 260 Hz down to 60 Hz as the envelope decays.
 *
 * If no slot targets AMP, the generator uses `master_level` directly
 * (constant amplitude — rarely what you want for percussion, but
 * useful for tone-only tests).
 */
typedef struct {
    /* Tone oscillator */
    uint32_t tone_base_hz_q8;    /* Q24.8 Hz, use freq_hz << 8 */
    uint16_t tone_level;         /* gain-class (Q15); UI: lfe_db_to_q15() */
    uint8_t  tone_wave;          /* lfe_drum_wave — SINE is the default */
    uint8_t  _pad_tone;

    /* Noise source */
    uint16_t noise_level;        /* gain-class (Q15); UI: lfe_db_to_q15() */
    uint32_t noise_seed;         /* 0 → library default */

    /* Filter (applied to the noise path only) */
    lfe_drum_filter_mode filter_mode;
    uint32_t filter_base_hz;     /* integer Hz */
    uint16_t filter_q;           /* Q15 damping, Q15_ONE = no resonance */

    /* Overall gain — gain-class (Q15); UI: lfe_db_to_q15().
     * Multiplied by the AMP-target modulation sum. */
    uint16_t master_level;

    /* Pre-master soft-clip drive. Q15 in [0, Q15_ONE]; 0 = clean
     * (bypassed for free). Drive boosts the pre-master mix by up to
     * ~2× before a cubic soft-clip — what gives analog-style drum
     * machines their bite. */
    uint16_t drive;
    uint16_t _pad_drive;

    /* Modulation slots. Any unused slot should set target = NONE. */
    lfe_drum_mod mods[LFE_DRUM_NUM_MODS];

    /* Single global LFO. Complements the envelope-based mod slots
     * with cyclic modulation (wobble hats, pulsing toms, gated snares).
     * Set lfo.dest = LFE_DRUM_LFO_DEST_OFF or lfo.depth = 0 to
     * disable — the generator skips stepping in that case. */
    lfe_drum_lfo lfo;
} lfe_drum_params;

/*
 * Built-in preset shapes. These fill a lfe_drum_params with known-good
 * values and return it — the generator itself is agnostic. Callers are
 * free to take a preset as a starting point and tweak individual fields
 * before calling lfe_gen_drum.
 */
typedef enum {
    LFE_DRUM_PRESET_KICK       = 0,
    LFE_DRUM_PRESET_SNARE,
    LFE_DRUM_PRESET_HAT_CLOSED,
    LFE_DRUM_PRESET_HAT_OPEN,
    LFE_DRUM_PRESET_TOM,
    LFE_DRUM_PRESET_CLAP,
    /* Triangle-tone kick with heavy drive — punchier, analog-808-ish. */
    LFE_DRUM_PRESET_KICK_808,
    /* Square tone with a slow LFO on tone level — cowbell-style gated
     * rhythm, great for filler tom hits. */
    LFE_DRUM_PRESET_COWBELL,
    LFE_DRUM_PRESET_COUNT
} lfe_drum_preset;

/*
 * Populate `out` with the named preset. Overwrites all fields
 * including the mods array.
 *
 * Returns:
 *   LFE_OK            on success
 *   LFE_ERR_NULL      if out is NULL
 *   LFE_ERR_BAD_PARAM if preset is unknown
 */
lfe_status lfe_drum_fill_preset(lfe_drum_params *out, lfe_drum_preset preset);

/*
 * Generate a drum hit into the output buffer. Writes exactly
 * out->length samples. The envelope(s) are triggered at sample 0 and
 * run freely for the duration of the buffer — if the buffer is shorter
 * than the envelope's release, the tail is truncated; if longer, the
 * tail beyond the envelope is silent (assuming an AMP-target mod).
 *
 * Returns:
 *   LFE_OK              on clean generation
 *   LFE_WARN_CLIPPED    if the final mix saturated at some sample
 *   LFE_ERR_NULL        if any required pointer is NULL
 *   LFE_ERR_BAD_PARAM   if the sample rate is 0 or tone exceeds Nyquist
 *   LFE_ERR_NOT_INIT    if lfe_init() has not been called
 */
lfe_status lfe_gen_drum(lfe_buffer *out, const lfe_drum_params *p);

/* ------------------------------------------------------------------ */
/* Phase 4: Subtractive synth                                          */
/* ------------------------------------------------------------------ */

/*
 * A classic two-oscillator + noise → filter → amp subtractive voice.
 * Reuses the free-form modulation slot design from Phase 3 (drum
 * generator) but with its own target set including oscillator-specific
 * knobs like pulse width.
 *
 *
 *   ┌─ osc1 (saw/sqr/tri/sine, detune) ─┐
 *   │                                   │
 *   ├─ osc2 (saw/sqr/tri/sine, detune) ─┼── mix ── SVF ── × amp ── out
 *   │                                   │
 *   └─ LFSR noise ──────────────────────┘
 *
 * Unlike the drum generator, the filter operates on the **full mix**
 * (oscillators + noise), which is the usual subtractive topology. The
 * oscillator shapes are naive / aliased; the filter is expected to
 * tame high-frequency aliasing in the typical cutoff-below-Nyquist
 * subtractive use case. A future pass may add PolyBLEP correction
 * without changing the public API.
 *
 * Also unlike the drum generator, the synth supports **note-off**: at
 * `note_off_sample` every active envelope transitions to its release
 * phase. Setting note_off_sample = 0 disables release entirely — the
 * voice runs open-ended until the buffer ends.
 *
 * The envelope-params and filter-mode types are reused from the Phase 3
 * drum section (`lfe_drum_env_params`, `lfe_drum_filter_mode`). The
 * shapes are generic — only the prefix is historical. A future unified
 * refactor may drop the `drum_` prefix; we retain it here to keep
 * Phase 3 call sites undisturbed.
 */

typedef enum {
    LFE_SYNTH_WAVE_SAW = 0,
    LFE_SYNTH_WAVE_SQUARE,
    LFE_SYNTH_WAVE_TRIANGLE,
    LFE_SYNTH_WAVE_SINE,
} lfe_synth_waveform;

typedef struct {
    lfe_synth_waveform wave;
    int32_t  detune_hz;    /* signed Hz shift from the common base pitch */
    uint16_t level;        /* gain-class (Q15); UI: lfe_db_to_q15() */
    uint16_t pulse_width;  /* Q15, only used by SQUARE; Q15_HALF = 50% */
} lfe_synth_osc;

/*
 * Oscillator combine modes. Replaces the bare `s1 * l1 + s2 * l2` mix
 * with a small dispatch that lets osc1 and osc2 interact in more
 * interesting ways. MIX is the default (value 0) so existing presets
 * — which don't set `combine` explicitly — keep their current sound.
 *
 * Each mode interprets `combine_param1` / `combine_param2` differently:
 *
 *   MIX         : both unused — classical additive mix.
 *   HARD_SYNC   : both unused in v1 — osc2 resets to phase 0 whenever
 *                 osc1 wraps. A future version may use param1 as a
 *                 slave-phase offset.
 *   FM          : param1 = phase-mod depth (Q15; 0..Q15_ONE maps to a
 *                 modulation index of roughly 0..4 cycles — "moderate
 *                 FM", enough for bell/EP tones but not full DX-style
 *                 scream). param2 unused.
 *   RING_MOD    : both unused — simple `(s1 * s2) >> 15`.
 *   CALVARIO    : param1 = gain1 in dB (Q8.8 signed, -64..0 useful
 *                 range), param2 = gain2 in dB same encoding. See the
 *                 project_synth_osc_combine_plan memo for the bit-
 *                 window interpretation and sonic behavior.
 */
typedef enum {
    LFE_SYNTH_COMBINE_MIX = 0,   /* additive — current behavior */
    LFE_SYNTH_COMBINE_HARD_SYNC,
    LFE_SYNTH_COMBINE_FM,        /* phase mod: osc1 modulates osc2 phase */
    LFE_SYNTH_COMBINE_RING_MOD,  /* (s1 * s2) >> 15 */
    LFE_SYNTH_COMBINE_CALVARIO,  /* weighted XOR with dB gains per operand */
} lfe_synth_osc_combine;

/*
 * Modulation targets. Same additive accumulation rule as the drum
 * generator: multiple slots targeting the same parameter sum their
 * (env_level × depth) contributions, and the result adds to the
 * static base value in the params struct.
 */
typedef enum {
    LFE_SYNTH_MOD_NONE = 0,
    LFE_SYNTH_MOD_AMP,              /* Q15 overall gain */
    LFE_SYNTH_MOD_PITCH,            /* integer Hz, applied to both oscillators */
    LFE_SYNTH_MOD_FILTER,           /* integer Hz cutoff shift */
    LFE_SYNTH_MOD_OSC1_LEVEL,       /* Q15 */
    LFE_SYNTH_MOD_OSC2_LEVEL,       /* Q15 */
    LFE_SYNTH_MOD_NOISE_LEVEL,      /* Q15 */
    LFE_SYNTH_MOD_PULSE_WIDTH,      /* Q15, applied to both oscs */
    LFE_SYNTH_MOD_COMBINE_PARAM1,   /* mode-specific (FM depth, Calvario gain1, ...) */
    LFE_SYNTH_MOD_COMBINE_PARAM2,   /* mode-specific (Calvario gain2, ...) */
} lfe_synth_mod_target;

/* Four mod slots — one more than the drum generator, since
 * subtractive voices commonly want amp, pitch, filter, and a spare
 * for PWM or a level sweep. */
#define LFE_SYNTH_NUM_MODS 4

typedef struct {
    lfe_drum_env_params  env;     /* reused ADSR shape from Phase 3 */
    lfe_synth_mod_target target;
    int32_t              depth;
} lfe_synth_mod;

typedef struct {
    uint32_t base_hz_q8;          /* common fundamental pitch, Q24.8 Hz */

    lfe_synth_osc osc1;
    lfe_synth_osc osc2;

    uint16_t noise_level;         /* gain-class (Q15); UI: lfe_db_to_q15() */
    uint32_t noise_seed;          /* 0 → library default */

    lfe_drum_filter_mode filter_mode;
    uint32_t filter_base_hz;      /* integer Hz */
    uint16_t filter_q;            /* Q15 damping */

    uint16_t master_level;        /* gain-class (Q15); UI: lfe_db_to_q15() */

    /* Sample index at which the envelopes enter release. 0 disables
     * note-off; otherwise every active mod's envelope is released at
     * that exact sample index (clamped to the buffer length). */
    uint32_t note_off_sample;

    lfe_synth_mod mods[LFE_SYNTH_NUM_MODS];

    /* Oscillator combine mode — see lfe_synth_osc_combine. Fields are
     * added at the END of the struct so existing presets (which zero-
     * init via designated initializers) default to MIX with zero
     * combine params — identical behavior to the pre-extension path. */
    lfe_synth_osc_combine combine;
    int32_t               combine_param1;
    int32_t               combine_param2;
} lfe_synth_params;

typedef enum {
    LFE_SYNTH_PRESET_LEAD = 0,
    LFE_SYNTH_PRESET_PAD,
    LFE_SYNTH_PRESET_PLUCK,
    LFE_SYNTH_PRESET_BASS,
} lfe_synth_preset;

/*
 * Populate `out` with the named preset. Overwrites every field.
 *
 * Returns:
 *   LFE_OK            on success
 *   LFE_ERR_NULL      if out is NULL
 *   LFE_ERR_BAD_PARAM if preset is unknown
 */
lfe_status lfe_synth_fill_preset(lfe_synth_params *out,
                                 lfe_synth_preset preset);

/*
 * Generate a subtractive-synth note into the output buffer. Writes
 * exactly out->length samples. Envelopes are triggered at sample 0
 * and released at sample `note_off_sample` (if nonzero).
 *
 * Returns:
 *   LFE_OK              on clean generation
 *   LFE_WARN_CLIPPED    if the final mix saturated at some sample
 *   LFE_ERR_NULL        if any required pointer is NULL
 *   LFE_ERR_BAD_PARAM   if the sample rate is 0 or the base pitch exceeds Nyquist
 *   LFE_ERR_NOT_INIT    if lfe_init() has not been called
 */
lfe_status lfe_gen_synth(lfe_buffer *out, const lfe_synth_params *p);

/* ------------------------------------------------------------------ */
/* Phase 4c: 4-operator FM synth                                       */
/* ------------------------------------------------------------------ */
/*
 * Dedicated FM generator, sibling to gen_drum / gen_synth rather than
 * an extension of them. Four identical operators (pure sine), each
 * with its own frequency ratio + level + ADSR amplitude envelope,
 * routed through a 4x4 phase-modulation matrix plus a 4-entry carrier
 * mix vector. Each operator can modulate any other operator (or
 * itself on the diagonal) at any amount, independent of which
 * operators are audible at the output — the matrix and the carrier
 * vector are deliberately decoupled so "how strongly op k modulates"
 * and "how strongly op k is heard" are separate knobs.
 *
 * Topology simplification: every cross-link is a ONE-SAMPLE DELAY
 * (not just feedback on the diagonal). At 32 kHz that's 31 µs per
 * edge — sonically undetectable — and it completely sidesteps the
 * topological-ordering problem of chained per-sample FM. The matrix
 * math is always well-defined regardless of sparsity.
 *
 * Because LFE renders offline (one-shot samples, not real-time), the
 * per-sample cost (~4 sine lookups + 16 muls + 16 accumulates) is a
 * non-concern — trivial for a 64k-sample render.
 *
 * See project_fm4op_plan.md for the design memo.
 */

#define LFE_FM4_NUM_OPS 4
#define LFE_FM4_NUM_LFOS 2

/* Where an LFO's output goes. The `target` field's interpretation
 * depends on the destination:
 *
 *   OFF           target ignored (LFO disabled)
 *   OP_LEVEL      target = op index 0..3 — additively modulates
 *                 ops[target].level (clamped to [0, Q15_ONE])
 *   MATRIX_CELL   target = (src<<2) | dst, 0..15 — additively
 *                 modulates mod_matrix[src][dst] (clamped to
 *                 [-Q15_ONE, +Q15_ONE]); classic dubstep wobble hits
 *                 a single cross-link
 *   CARRIER_MIX   target = op index 0..3 — additively modulates
 *                 carrier_mix[target] (clamped to [-Q15_ONE, +Q15_ONE])
 *   PITCH         target ignored — modulates base_hz_q8 uniformly
 *                 across all ops; depth=Q15_ONE → ±50% pitch swing
 */
typedef enum {
    LFE_FM4_LFO_DEST_OFF = 0,
    LFE_FM4_LFO_DEST_OP_LEVEL,
    LFE_FM4_LFO_DEST_MATRIX_CELL,
    LFE_FM4_LFO_DEST_CARRIER_MIX,
    LFE_FM4_LFO_DEST_PITCH,
    LFE_FM4_LFO_DEST_COUNT
} lfe_fm4_lfo_dest;

/* FM4 LFO slot: shared oscillator config + FM4-specific dest/target.
 * The shape/rate/depth triplet is the shared `lfe_lfo_params` struct
 * so every engine's LFO UI and rendering can use the same code path;
 * dest+target is what's domain-specific (see lfe_fm4_lfo_dest). */
typedef struct {
    lfe_lfo_params cfg;    /* shape / rate / depth — shared SDK type */
    uint8_t        dest;   /* lfe_fm4_lfo_dest */
    uint8_t        target; /* destination-relative target byte */
    uint16_t       _pad;
} lfe_fm4_lfo;

/* Legacy shape aliases — old code that wrote LFE_FM4_LFO_SHAPE_* now
 * gets the shared LFE_LFO_SHAPE_* values. Prefer the shared spellings
 * in new code. */
typedef lfe_lfo_shape lfe_fm4_lfo_shape;
#define LFE_FM4_LFO_SHAPE_SINE      LFE_LFO_SHAPE_SINE
#define LFE_FM4_LFO_SHAPE_TRIANGLE  LFE_LFO_SHAPE_TRIANGLE
#define LFE_FM4_LFO_SHAPE_SQUARE    LFE_LFO_SHAPE_SQUARE
#define LFE_FM4_LFO_SHAPE_SAW_UP    LFE_LFO_SHAPE_SAW_UP
#define LFE_FM4_LFO_SHAPE_SAW_DOWN  LFE_LFO_SHAPE_SAW_DOWN
#define LFE_FM4_LFO_SHAPE_COUNT     LFE_LFO_SHAPE_COUNT

typedef struct {
    /* Q8.8 multiplier of params.base_hz_q8. 0x100 = 1.0x (same as
     * fundamental), 0x380 = 3.5x (classic bell inharmonic ratio). */
    uint32_t freq_ratio_q8;

    /* Fine detune offset in cents. Reserved for a future phase —
     * currently unused by the Phase α generator; set to 0. */
    int16_t  detune_cents;

    /* Op output gain. Used BOTH as the scale when this op is mixed
     * into the audio output (via carrier_mix[i]) AND as the scale
     * when this op's sample feeds other ops via the mod matrix.
     * Q15 in [0, Q15_ONE]. */
    uint16_t level;

    /* ADSR amplitude envelope. Reused from the drum section for
     * consistency with the other generators. */
    lfe_drum_env_params env;
} lfe_fm4_op;

typedef struct {
    /* Common fundamental pitch, Q24.8 Hz. Each op's actual phase
     * increment is (base_hz_q8 * ops[i].freq_ratio_q8) >> 8. */
    uint32_t base_hz_q8;

    /* The four operators. */
    lfe_fm4_op ops[LFE_FM4_NUM_OPS];

    /* Phase-modulation matrix. mod_matrix[src][dst] in Q15 signed:
     * op src's previous-sample output (scaled by its level) adds a
     * phase offset to op dst equal to
     *   (prev_out[src] * mod_matrix[src][dst]) scaled so that unity
     *   matrix entry at full prev_out gives modulation index ~8
     *   (more extreme than the 2-osc synth's index-4 ceiling — the
     *   dedicated FM engine is expected to push further).
     * Diagonal entries = self-feedback. Zero-init = additive mix. */
    int16_t mod_matrix[LFE_FM4_NUM_OPS][LFE_FM4_NUM_OPS];

    /* Carrier mix: how much each op contributes to the FINAL audio
     * output. Q15 signed. Decoupling this from ops[i].level means
     * you can have a loud modulator that drives the carrier strongly
     * but is itself silent (level high, carrier_mix zero) — the
     * classic DX7 "modulator is inaudible, only the result is heard"
     * configuration. */
    int16_t carrier_mix[LFE_FM4_NUM_OPS];

    /* Sample index at which all envelopes enter release. 0 disables
     * note-off. */
    uint32_t note_off_sample;

    /* Global LFOs. Two slots, each independently configurable. The
     * render loop steps both LFOs every sample and applies their
     * (depth-scaled) output additively to their chosen destination.
     * LFOs targeting the same destination simply sum — so two LFOs
     * both set to OP_LEVEL target=0 at half depth can emulate a
     * quarter-depth LFO + its complement, etc. */
    lfe_fm4_lfo lfos[LFE_FM4_NUM_LFOS];
} lfe_fm4_params;

typedef enum {
    /* Classic 2-op DX electric piano: one modulator pings a carrier
     * with a short-decay envelope, creating the bell-like attack. */
    LFE_FM4_PRESET_EP = 0,

    /* 3-op inharmonic tuned bell: carrier + primary modulator at 3.5x
     * (inharmonic ratio, hence "metallic") + secondary shimmer
     * modulator at 7x. Long decay for the ring-out tail. */
    LFE_FM4_PRESET_BELL,

    /* 2-op synth bass: low fundamental, carrier + 2x modulator. */
    LFE_FM4_PRESET_BASS,

    /* 2-op brass: slow attack, integer harmonic modulator for a
     * full, bright sustained tone. */
    LFE_FM4_PRESET_BRASS,

    /* 2-op plucked string: fast attack, fast natural decay via the
     * amp envelope (no note_off — like the PLUCK preset in gen_synth). */
    LFE_FM4_PRESET_PLUCK,

    /* Dubstep wobble bass: deep fundamental, triangle LFO at 3 Hz on
     * the op1→op0 matrix cell, sine LFO at 0.5 Hz on op0's carrier
     * mix for slow amplitude movement. */
    LFE_FM4_PRESET_WOBBLE,

    /* Aggressive growl lead: square LFO hard-shifting matrix cell for
     * a rhythmic gated timbre, triangle LFO on pitch for a small
     * vibrato wobble on top. */
    LFE_FM4_PRESET_GROWL,

    LFE_FM4_PRESET_COUNT
} lfe_fm4_preset;

/*
 * Populate `out` with the named preset. Overwrites every field.
 * Returns LFE_OK / LFE_ERR_NULL / LFE_ERR_BAD_PARAM.
 */
lfe_status lfe_fm4_fill_preset(lfe_fm4_params *out, lfe_fm4_preset preset);

/*
 * Generate a 4-op FM note into the output buffer. Writes exactly
 * out->length samples. All four envelopes are triggered at sample 0
 * and released at sample `note_off_sample` (if nonzero).
 *
 * Returns:
 *   LFE_OK             on clean generation
 *   LFE_WARN_CLIPPED   if the final mix saturated at some sample
 *   LFE_ERR_NULL       if any required pointer is NULL
 *   LFE_ERR_BAD_PARAM  if sample rate is 0 or base pitch exceeds Nyquist
 *   LFE_ERR_NOT_INIT   if lfe_init() has not been called
 */
lfe_status lfe_gen_fm4(lfe_buffer *out, const lfe_fm4_params *p);

/* ------------------------------------------------------------------ */
/* Phase 6: Braids — 18 macro shapes ported from Mutable Instruments   */
/* ------------------------------------------------------------------ */

/*
 * Braids is a digital eurorack module by Mutable Instruments. Its
 * macro-oscillator dispatches one "shape" knob across analog and
 * digital DSP primitives covering an unusually broad timbral range.
 * This generator ports 18 of those shapes — see project memory
 * `project_phase6_braids_plan` for the selection rationale.
 *
 * Every shape takes the same 2-parameter (timbre, color) interface
 * plus pitch. Per-shape interpretation:
 *   - CSAW           : timbre = pulse width, color = discontinuity depth
 *   - MORPH          : timbre = osc-pair morph, color = filter cutoff
 *   - SAW_SQUARE     : timbre = saw/square balance
 *   - SINE_TRIANGLE  : timbre = wavefold depth, color = sine/tri balance
 *   - TRIPLE_*       : timbre + color = detune amounts for voices 1, 2
 *   - TRIPLE_RING_MOD: timbre + color = modulator pitch offsets
 *   - SAW_SWARM      : timbre = detune spread, color = HP cutoff
 *   - SAW_COMB       : timbre = comb delay, color = comb feedback
 *   - VOWEL_FOF      : timbre = vowel (a..u), color = voice (bass..soprano)
 *   - PLUCKED        : timbre = sustain, color = pluck width
 *   - BOWED          : timbre = bow pressure, color = bow position
 *   - BLOWN          : timbre = breath, color = body brightness
 *   - FLUTED         : timbre = breath, color = jet length
 *   - TWIN_PEAKS_NOISE: timbre = Q, color = second-peak offset
 *   - DIGITAL_MODULATION: timbre = symbol rate, color = data driver
 *
 * Shape numbering is locked — append-only; do NOT renumber existing
 * entries or saved parameter sets break.
 */
typedef enum {
    LFE_BRAIDS_SHAPE_CSAW = 0,
    LFE_BRAIDS_SHAPE_MORPH,
    LFE_BRAIDS_SHAPE_SAW_SQUARE,
    LFE_BRAIDS_SHAPE_SINE_TRIANGLE,
    LFE_BRAIDS_SHAPE_TRIPLE_SAW,
    LFE_BRAIDS_SHAPE_TRIPLE_SQUARE,
    LFE_BRAIDS_SHAPE_TRIPLE_TRIANGLE,
    LFE_BRAIDS_SHAPE_TRIPLE_SINE,
    LFE_BRAIDS_SHAPE_TRIPLE_RING_MOD,
    LFE_BRAIDS_SHAPE_SAW_SWARM,
    LFE_BRAIDS_SHAPE_SAW_COMB,
    LFE_BRAIDS_SHAPE_VOWEL_FOF,
    LFE_BRAIDS_SHAPE_PLUCKED,
    LFE_BRAIDS_SHAPE_BOWED,
    LFE_BRAIDS_SHAPE_BLOWN,
    LFE_BRAIDS_SHAPE_FLUTED,
    LFE_BRAIDS_SHAPE_TWIN_PEAKS_NOISE,
    LFE_BRAIDS_SHAPE_DIGITAL_MODULATION,
    LFE_BRAIDS_SHAPE_COUNT
} lfe_braids_shape;

typedef struct {
    lfe_braids_shape shape;

    /* Pitch in Q24.8 Hz. The generator converts to Braids' internal
     * "midi_pitch" (semitones × 128 above an internal reference). */
    uint32_t pitch_hz_q8;

    /* Timbre + color knobs, both Q15. Per-shape semantics above. */
    uint16_t timbre;
    uint16_t color;

    /* RNG seed used by stochastic shapes (PLUCKED initial noise,
     * SAW_SWARM strike phases, TWIN_PEAKS_NOISE / FLUTED breath
     * noise). Zero means "use the library's running RNG state".
     * Setting an explicit seed makes a render deterministic. */
    uint32_t seed;
} lfe_braids_params;

/*
 * Render Braids into `out`. Internal rate is 96 kHz; the generator
 * box-averages down to `out->rate`. `out->length` is the target
 * sample count at `out->rate`.
 *
 * Returns:
 *   LFE_OK             on success
 *   LFE_WARN_CLIPPED   if any output sample saturated
 *   LFE_ERR_NULL       if any required pointer is NULL
 *   LFE_ERR_BAD_PARAM  if shape is out of range, rate is 0, or rate > 96000
 *   LFE_ERR_NOT_INIT   if lfe_init() has not been called
 */
lfe_status lfe_gen_braids(lfe_buffer *out, const lfe_braids_params *p);

/* ------------------------------------------------------------------ */
/* Phase 5: FX — sample-range transformations                          */
/* ------------------------------------------------------------------ */

/*
 * Effects differ from generators in an important way: generators
 * *produce* audio into an empty buffer, while effects *transform* an
 * existing buffer over a specific sample range. Every FX entry point
 * takes a `lfe_fx_range` that identifies the slice of the buffer to
 * process; samples outside that range are guaranteed to be untouched
 * (byte-identical on exit).
 *
 * This matches an editor workflow: the user selects a portion of a
 * sample with the stylus and applies an effect to just that portion.
 *
 * All FX operate in-place. Stateful effects (filter, delay) start
 * with fresh state at range.start — the first few samples of a
 * filter-processed selection may click slightly; the editor UI is
 * expected to offer a crossfade or undo, not the library.
 *
 * Effects in this phase:
 *   - lfe_fx_distort      hard / soft / fold / bitcrush
 *   - lfe_fx_filter       SVF (LP/HP/BP/Notch) over selection
 *   - lfe_fx_delay        mono single-tap delay with feedback
 *   - lfe_fx_env_shaper   drawn volume envelope over selection
 *   - lfe_fx_normalize    DC-remove + peak-normalize over selection
 *   - lfe_fx_ott          3-band multiband up/down compressor
 *   - lfe_fx_reverse      reverse sample order within selection
 *   - lfe_fx_bitcrush    bit depth + sample rate reduction + TPDF dither
 *
 * Not in this phase (planned): ping-pong delay (needs a stereo buffer
 * type), biquad EQ, reverb.
 */

/*
 * Half-open selection range. `start` is inclusive, `end` is exclusive.
 * start == end is a no-op (returns LFE_OK without touching the buffer).
 * end > buf->length returns LFE_ERR_BAD_PARAM.
 */
typedef struct {
    uint32_t start;
    uint32_t end;
} lfe_fx_range;

/* -------- Distortion / saturation -------- */

typedef enum {
    LFE_FX_DIST_HARD = 0,    /* hard clip at ±threshold */
    LFE_FX_DIST_SOFT,        /* rational soft-clip, tanh-like */
    LFE_FX_DIST_FOLD,        /* wavefolder: reflect past threshold */
    LFE_FX_DIST_BITCRUSH,    /* quantize to N bits */
} lfe_fx_distortion_mode;

typedef struct {
    lfe_fx_distortion_mode mode;
    uint16_t drive;       /* gain-class (Q15); UI: lfe_db_to_q15() */
    uint16_t threshold;   /* Q15 clip point (used by HARD and FOLD) */
    uint16_t mix;         /* Q15 dry/wet: 0 = dry, Q15_ONE = fully wet */
    uint8_t  bit_depth;   /* 1..15: target bit depth for BITCRUSH */
} lfe_fx_distortion_params;

/*
 * Apply a distortion/saturation effect to [range.start, range.end).
 *
 * Returns:
 *   LFE_OK                 on success
 *   LFE_ERR_NULL           if buf, buf->data, range, or p is NULL
 *   LFE_ERR_BAD_PARAM      if range.end > buf->length, or if mode is unknown,
 *                          or if bit_depth is out of range for BITCRUSH
 */
lfe_status lfe_fx_distort(lfe_buffer *buf,
                          const lfe_fx_range *range,
                          const lfe_fx_distortion_params *p);

/* -------- Filter (SVF over selection) -------- */

typedef struct {
    lfe_drum_filter_mode mode;    /* LP/HP/BP/Notch */
    uint32_t cutoff_hz;
    uint16_t q;                   /* Q15 resonance knob: 0 = critically damped,
                                     Q15_ONE = near self-oscillation. Internally
                                     mapped exponentially to Q factor ∈ [0.5, 40]. */
    uint16_t mix;                 /* Q15 dry/wet */
} lfe_fx_filter_params;

lfe_status lfe_fx_filter(lfe_buffer *buf,
                         const lfe_fx_range *range,
                         const lfe_fx_filter_params *p);

/* -------- Delay (mono, single-tap, with feedback) -------- */

typedef struct {
    uint32_t delay_ms;        /* delay time in milliseconds */
    uint16_t feedback;        /* gain-class (Q15); UI: lfe_db_to_q15(); 0 = single echo */
    uint16_t mix;             /* Q15 dry/wet */

    /*
     * Caller-owned scratch space used as the delay line. Must be at
     * least (delay_ms * buf->rate / 1000) samples long. The library
     * never allocates — NDS builds have no heap. The effect zeroes
     * the scratch buffer before use, so caller can reuse it for
     * multiple delay passes without clearing it between calls.
     */
    lfe_sample_t *scratch;
    uint32_t      scratch_length;
} lfe_fx_delay_params;

lfe_status lfe_fx_delay(lfe_buffer *buf,
                        const lfe_fx_range *range,
                        const lfe_fx_delay_params *p);

/* -------- Envelope shaper (volume curve) -------- */

/*
 * Apply a user-supplied gain envelope to the selection. The canvas
 * is a flat array of Q15 gain values (0 = silence, Q15_ONE = unity)
 * representing the full-length shape. The canvas is stretched to the
 * selection length via fixed-point linear interpolation: canvas_length
 * need not match the selection length.
 *
 * Typical uses:
 *   - fade-in: canvas = [0, Q15_ONE/2, Q15_ONE]
 *   - fade-out: canvas = [Q15_ONE, Q15_ONE/2, 0]
 *   - bell: a bell-shaped gain curve over the selection
 *
 * Presets fill the canvas with common shapes via lfe_fx_env_fill_preset.
 */

typedef enum {
    LFE_FX_ENV_PRESET_FADE_IN = 0,
    LFE_FX_ENV_PRESET_FADE_OUT,
    LFE_FX_ENV_PRESET_EXP_DECAY,
    LFE_FX_ENV_PRESET_EXP_ATTACK,
    LFE_FX_ENV_PRESET_TRIANGLE,
    LFE_FX_ENV_PRESET_BELL,
} lfe_fx_env_preset;

/*
 * Fill a Q15 gain canvas with a preset shape.
 *
 * Returns:
 *   LFE_OK            on success
 *   LFE_ERR_NULL      if canvas is NULL
 *   LFE_ERR_BAD_PARAM if length < 2 or preset is unknown
 */
lfe_status lfe_fx_env_fill_preset(uint16_t *canvas,
                                  uint32_t length,
                                  lfe_fx_env_preset preset);

typedef struct {
    const uint16_t *canvas;     /* per-point gain-class (Q15); UI: lfe_db_to_q15() */
    uint32_t        canvas_length;
    uint16_t        mix;        /* Q15 dry/wet (usually Q15_ONE = fully wet) */
} lfe_fx_env_shaper_params;

/*
 * Apply a gain envelope to [range.start, range.end). The canvas is
 * stretched (with linear interpolation) to cover the selection exactly.
 *
 * Returns:
 *   LFE_OK            on success
 *   LFE_ERR_NULL      if any pointer is NULL
 *   LFE_ERR_BAD_PARAM if canvas_length < 2 or range.end > buf->length
 */
/* -------- Normalize (DC-remove + peak-normalize) -------- */

/*
 * DC-remove and peak-normalize the selection in place.
 *
 *   1. Compute the mean of the samples in [range.start, range.end);
 *      subtract it from every sample (DC offset removal).
 *   2. Find the peak absolute value of the DC-removed selection.
 *   3. Scale every sample by (target_peak / peak) so the loudest
 *      sample reaches exactly `target_peak` in magnitude.
 *
 * Both operations always run — DC removal is not toggleable. If the
 * selection is silent (or constant DC) step 3 is skipped cleanly.
 *
 * `target_peak` is Q15. A value of 0 is interpreted as Q15_ONE
 * (full-scale) — a user-friendly default; callers that literally want
 * "multiply by zero" should mute the selection with the env shaper
 * instead.
 *
 * Returns:
 *   LFE_OK             success
 *   LFE_WARN_CLIPPED   one or more samples saturated at ±Q15_ONE after
 *                      scaling (possible at target_peak very near the
 *                      rail due to rounding)
 *   LFE_ERR_NULL       buf / buf->data / range is NULL
 *   LFE_ERR_BAD_PARAM  range.end > buf->length or range.end < range.start
 */
typedef struct {
    uint16_t target_peak;   /* Q15; 0 or Q15_ONE for full-scale */
} lfe_fx_normalize_params;

lfe_status lfe_fx_normalize(lfe_buffer *buf,
                            const lfe_fx_range *range,
                            const lfe_fx_normalize_params *p);

lfe_status lfe_fx_env_shaper(lfe_buffer *buf,
                             const lfe_fx_range *range,
                             const lfe_fx_env_shaper_params *p);

/* -------- OTT (3-band multiband compressor) -------- */

/*
 * "Over-the-top" multiband compressor, inspired by Xfer OTT. Splits
 * the selection into three bands with fixed Linkwitz-Riley 4th-order
 * crossovers at ~88 Hz and ~2500 Hz, applies independent upward and
 * downward compression to each band, re-sums, and writes back.
 *
 * Compression is done with fixed ratios (downward 3:1, upward 1:3) and
 * fixed thresholds (downward ≈ -24 dBFS, upward ≈ -40 dBFS). The per-
 * band `down_*` and `up_*` knobs scale the amount of gain reduction /
 * boost between 0 (bypass that direction) and Q15_ONE (full ratio).
 * The global `depth` further scales both directions simultaneously.
 *
 * Band gains (gain_low/mid/high) let the user re-balance the bands
 * post-compression — a cheap substitute for soloing / muting, used
 * heavily in OTT presets for bass boosts and presence lifts.
 *
 * Future improvement: add look-ahead (small pre-buffer so compression
 * can anticipate peaks). Offline rendering allows arbitrary look-ahead
 * for free in principle; skipping for v1 to keep the state simple.
 */
typedef struct {
    /* Global */
    uint16_t depth;       /* Q15 — scales up+down compression together */
    uint16_t time;        /* Q15 — scales attack and release times
                           *   0     → fastest  (atk ≈ 0.5 ms, rel ≈ 50 ms)
                           *   Q15_ONE → slowest (atk ≈ 5 ms,   rel ≈ 500 ms)
                           * Default ≈ half scale. */
    uint16_t in_gain;     /* gain-class (Q15); UI: lfe_db_to_q15() */
    uint16_t out_gain;    /* gain-class (Q15); UI: lfe_db_to_q15() */

    /* Per-band downward-compression depth (Q15). 0 = off. */
    uint16_t down_low;
    uint16_t down_mid;
    uint16_t down_high;

    /* Per-band upward-compression (expansion-to-threshold) depth (Q15). */
    uint16_t up_low;
    uint16_t up_mid;
    uint16_t up_high;

    /* Per-band makeup gain (gain-class Q15). UI: lfe_db_to_q15(). */
    uint16_t gain_low;
    uint16_t gain_mid;
    uint16_t gain_high;
} lfe_fx_ott_params;

lfe_status lfe_fx_ott(lfe_buffer *buf,
                      const lfe_fx_range *range,
                      const lfe_fx_ott_params *p);

/* -------- Reverse -------- */

/*
 * Reverse sample order within [range.start, range.end).
 * No parameters — pure in-place mirror.
 */
lfe_status lfe_fx_reverse(lfe_buffer *buf, const lfe_fx_range *range);

/* -------- Bitcrush (bit depth + sample rate reduction + dither) ---- */

typedef struct {
    uint8_t  bit_depth;   /* 1..15: target bit depth */
    uint8_t  rate_div;    /* 1..64: hold every Nth sample (1 = no reduction) */
    uint8_t  dither;      /* 0 = off, 1 = TPDF dither before quantization */
    uint16_t mix;         /* Q15 dry/wet: 0 = dry, Q15_ONE = fully wet */
} lfe_fx_bitcrush_params;

lfe_status lfe_fx_bitcrush(lfe_buffer *buf,
                           const lfe_fx_range *range,
                           const lfe_fx_bitcrush_params *p);

/* ------------------------------------------------------------------ */
/* Future phases extend the API below this line.                       */
/*                                                                     */
/*   Phase 5b: ping-pong delay, biquad EQ, reverb (deferred)           */
/*   Phase 6: Braids subset                                           */
/* ------------------------------------------------------------------ */

#ifdef __cplusplus
}
#endif

#endif /* LFE_H */
