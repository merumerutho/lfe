/*
 * lfe_braids_digital.h — ported Braids "digital" oscillator.
 *
 * Ported from braids/digital_oscillator.{h,cc} (MIT, (c) 2012 Emilie
 * Gillet). Only the 7 shapes reached by the 18 MACRO shapes in scope
 * are retained:
 *
 *   VOWEL_FOF, TWIN_PEAKS_NOISE, DIGITAL_MODULATION,
 *   PLUCKED, BOWED, BLOWN, FLUTED
 *
 * Port conventions mirror lfe_braids_analog.h:
 *   - C++ class + state union → C struct that owns the state union
 *   - `this->` → `self->`
 *   - Dispatch table is a static function-pointer array keyed by the
 *     (re-densified) `braids_digital_shape` enum
 *
 * The enum is densified (0..6) rather than matching upstream's sparse
 * numeric ids — callers from the MACRO layer will map explicitly, so
 * there's no wire-format dependency on the numbers.
 */

#ifndef LFE_BRAIDS_DIGITAL_H
#define LFE_BRAIDS_DIGITAL_H

#include <stdint.h>
#include <stddef.h>
#include "lfe_braids_excitation.h"
#include "lfe_braids_svf.h"

/* ---- Constants (upstream digital_oscillator.h) ---- */

#define BRAIDS_WG_BRIDGE_LENGTH   1024
#define BRAIDS_WG_NECK_LENGTH     4096
#define BRAIDS_WG_BORE_LENGTH     2048
#define BRAIDS_WG_JET_LENGTH      1024
#define BRAIDS_WG_FBORE_LENGTH    4096
#define BRAIDS_COMB_DELAY_LENGTH  8192

#define BRAIDS_NUM_FORMANTS       5
#define BRAIDS_NUM_PLUCK_VOICES   3
#define BRAIDS_KS_LENGTH          (1025 * 4)

/* ---- Shape enum (densified) ---- */

typedef enum {
    BRAIDS_DOSC_SHAPE_VOWEL_FOF = 0,
    BRAIDS_DOSC_SHAPE_TWIN_PEAKS_NOISE,
    BRAIDS_DOSC_SHAPE_DIGITAL_MODULATION,
    BRAIDS_DOSC_SHAPE_PLUCKED,
    BRAIDS_DOSC_SHAPE_BOWED,
    BRAIDS_DOSC_SHAPE_BLOWN,
    BRAIDS_DOSC_SHAPE_FLUTED,
    BRAIDS_DOSC_SHAPE_TRIPLE_RING_MOD,   /* routed from MACRO TRIPLE_RING_MOD */
    BRAIDS_DOSC_SHAPE_SAW_SWARM,          /* routed from MACRO SAW_SWARM     */
    BRAIDS_DOSC_SHAPE_COMB_FILTER,        /* routed from MACRO SAW_COMB      */

    BRAIDS_DOSC_SHAPE_COUNT
} braids_digital_shape;

/* ---- Per-shape state structs ----
 *
 * Each corresponds to one arm of the union in upstream's
 * DigitalOscillatorState. Only the shapes we've implemented have
 * real bodies; unimplemented shapes still declare their struct so
 * the union is complete from day one, but they may be filled in
 * during later sub-phases (6e-2 onwards).
 */

typedef struct {
    int32_t next_saw_sample;
    int16_t previous_sample;
    int32_t svf_lp[BRAIDS_NUM_FORMANTS];
    int32_t svf_bp[BRAIDS_NUM_FORMANTS];
} braids_fof_state;

typedef struct {
    int32_t bp;
    int32_t lp;
} braids_svf_state;

/* TWIN_PEAKS_NOISE uses two independent biquad resonators, each with
 * two memory taps. Upstream shares the layout with ParticleNoiseState
 * but we don't port PARTICLE_NOISE, so we keep only the [2][2] we
 * actually need. */
typedef struct {
    int32_t filter_state[2][2];
} braids_pno_state;

typedef struct {
    uint32_t symbol_phase;
    uint16_t symbol_count;
    int32_t  filter_state;
    uint8_t  data_byte;
} braids_dmd_state;

typedef struct {
    size_t   size;
    size_t   write_ptr;
    size_t   shift;
    size_t   mask;
    size_t   pluck_position;
    size_t   initialization_ptr;
    uint32_t phase;
    uint32_t phase_increment;
    uint32_t max_phase_increment;
    int16_t  previous_sample;
    uint8_t  polyphony_assigner;
} braids_pluck_state;

typedef struct {
    uint16_t delay_ptr;
    uint16_t excitation_ptr;
    int32_t  lp_state;
    int32_t  filter_state[2];
    int16_t  previous_sample;
} braids_phy_state;

/* TRIPLE_RING_MOD: single carrier phase (kept in `self->phase`) ring-
 * modulated by two independent sine modulators. Upstream reused the
 * vowel-synth state union for these; we allocate a dedicated arm. */
typedef struct {
    uint32_t modulator_phase[2];
} braids_trm_state;

/* COMB_FILTER: filters the incoming pitch across blocks to avoid
 * clicks when the caller retunes mid-note. */
typedef struct {
    int16_t filtered_pitch;
} braids_comb_state;

/* SAW_SWARM: 6 detuned saw phasors + a two-stage ladder LP + DC blocker. */
typedef struct {
    uint32_t phase[6];
    int32_t  filter_state[2][2];
    int32_t  dc_blocked;
    int32_t  lp;
    int32_t  bp;
} braids_saw_swarm_state;

/* ---- Main oscillator ---- */

typedef struct {
    uint32_t phase;
    uint32_t phase_increment;
    uint32_t delay;

    int16_t parameter[2];
    int16_t previous_parameter[2];
    int32_t smoothed_parameter;
    int16_t pitch;

    uint8_t active_voice;

    int init;
    int strike;

    braids_digital_shape shape;
    braids_digital_shape previous_shape;

    /* Per-shape state. Upstream used an anonymous union to share RAM;
     * keeping that semantic — only the shape currently active may
     * read/write its arm. */
    union {
        braids_fof_state        fof;
        braids_svf_state        svf;
        braids_pno_state        pno;
        braids_dmd_state        dmd;
        braids_pluck_state      plk[4];   /* 4 polyphonic voices */
        braids_phy_state        phy;
        braids_trm_state        trm;      /* TRIPLE_RING_MOD */
        braids_saw_swarm_state  saw;      /* SAW_SWARM       */
        braids_comb_state       cmb;      /* COMB_FILTER     */
        uint32_t                modulator_phase;
    } state;

    /* Support DSP modules owned by the oscillator (mirrors Braids). */
    braids_excitation_t pulse[4];
    braids_svf_t        svf_unit[3];

    /* Delay lines — one large union shared across shapes that need
     * bulk sample memory. Sizing matches upstream exactly; the total
     * is the max of the arms. */
    union {
        int16_t comb[BRAIDS_COMB_DELAY_LENGTH];           /* 16 KB — COMB_FILTER */
        int16_t ks[BRAIDS_KS_LENGTH];                     /*  8 KB — PLUCKED     */
        struct {
            int8_t bridge[BRAIDS_WG_BRIDGE_LENGTH];
            int8_t neck[BRAIDS_WG_NECK_LENGTH];
        } bowed;                                          /*  5 KB — BOWED       */
        int16_t bore[BRAIDS_WG_BORE_LENGTH];              /*  4 KB — BLOWN       */
        struct {
            int8_t jet[BRAIDS_WG_JET_LENGTH];
            int8_t bore[BRAIDS_WG_FBORE_LENGTH];
        } fluted;                                         /*  5 KB — FLUTED      */
    } delay_lines;
} braids_digital_osc_t;

void braids_digital_osc_init(braids_digital_osc_t *self);

static inline void braids_digital_osc_set_shape(braids_digital_osc_t *self,
                                                braids_digital_shape s)
{ self->shape = s; }

static inline void braids_digital_osc_set_pitch(braids_digital_osc_t *self,
                                                int16_t pitch)
{
    /* Smooth HF noise when the pitch CV is noisy — mirrors upstream. */
    if (self->pitch > (90 << 7) && pitch > (90 << 7))
        self->pitch = (int16_t)(((int32_t)self->pitch + pitch) >> 1);
    else
        self->pitch = pitch;
}

static inline void braids_digital_osc_set_parameters(braids_digital_osc_t *self,
                                                     int16_t p1, int16_t p2)
{
    self->parameter[0] = p1;
    self->parameter[1] = p2;
}

static inline uint32_t braids_digital_osc_phase_increment(const braids_digital_osc_t *self)
{ return self->phase_increment; }

static inline void braids_digital_osc_strike(braids_digital_osc_t *self)
{ self->strike = 1; }

/* Render `size` samples. `sync` is an optional hard-sync trigger
 * stream (NULL not supported — pass a zero-filled buffer for no-sync,
 * matches upstream semantics). */
void braids_digital_osc_render(braids_digital_osc_t *self,
                               const uint8_t *sync,
                               int16_t *buffer,
                               size_t size);

#endif /* LFE_BRAIDS_DIGITAL_H */
