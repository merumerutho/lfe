/*
 * lfe_braids_macro.h — MacroOscillator dispatch.
 *
 * Ported from braids/macro_oscillator.{h,cc} (MIT, © 2012 Emilie
 * Gillet). Routes the user-facing "shape" selection onto the underlying
 * analog and digital oscillator primitives, plus a few multi-oscillator
 * mix operations unique to the macro layer (MORPH, SAW_SQUARE,
 * SINE_TRIANGLE, TRIPLE_*, SAW_COMB).
 *
 * Scope: the 18 MACRO shapes in the port plan. `braids_macro_shape` is
 * densified (0..17) — it is NOT the upstream enum numbering.
 */

#ifndef LFE_BRAIDS_MACRO_H
#define LFE_BRAIDS_MACRO_H

#include <stdint.h>
#include <stddef.h>

#include "lfe_braids_analog.h"
#include "lfe_braids_digital.h"

/* Size of the internal block used by the MACRO renderer. Limits the
 * stack footprint of the temp / sync scratch. Must be even (several
 * shapes are 2×-downsampled). */
#define BRAIDS_MACRO_BLOCK 64

typedef enum {
    BRAIDS_MACRO_CSAW = 0,
    BRAIDS_MACRO_MORPH,
    BRAIDS_MACRO_SAW_SQUARE,
    BRAIDS_MACRO_SINE_TRIANGLE,
    BRAIDS_MACRO_TRIPLE_SAW,
    BRAIDS_MACRO_TRIPLE_SQUARE,
    BRAIDS_MACRO_TRIPLE_TRIANGLE,
    BRAIDS_MACRO_TRIPLE_SINE,
    BRAIDS_MACRO_TRIPLE_RING_MOD,
    BRAIDS_MACRO_SAW_SWARM,
    BRAIDS_MACRO_SAW_COMB,
    BRAIDS_MACRO_VOWEL_FOF,
    BRAIDS_MACRO_PLUCKED,
    BRAIDS_MACRO_BOWED,
    BRAIDS_MACRO_BLOWN,
    BRAIDS_MACRO_FLUTED,
    BRAIDS_MACRO_TWIN_PEAKS_NOISE,
    BRAIDS_MACRO_DIGITAL_MODULATION,

    BRAIDS_MACRO_COUNT
} braids_macro_shape;

typedef struct {
    int16_t parameter[2];           /* timbre (p[0]), color (p[1]) — Q15 */
    int16_t previous_parameter[2];
    int16_t pitch;                  /* Braids "midi_pitch" units: semitone*128 */
    int32_t lp_state;               /* MORPH's LP filter memory */

    braids_analog_osc_t   analog[3];
    braids_digital_osc_t  digital;

    braids_macro_shape shape;
    braids_macro_shape previous_shape;

    /* Scratch buffers used by the multi-oscillator mix paths. */
    uint8_t sync[BRAIDS_MACRO_BLOCK];
    int16_t temp[BRAIDS_MACRO_BLOCK];
} braids_macro_osc_t;

void braids_macro_osc_init(braids_macro_osc_t *self);

static inline void braids_macro_osc_set_shape(braids_macro_osc_t *self,
                                              braids_macro_shape shape)
{
    /* Changing shape re-strikes the digital oscillator — mirrors
     * upstream set_shape(). */
    if (shape != self->shape)
        braids_digital_osc_strike(&self->digital);
    self->shape = shape;
}

static inline void braids_macro_osc_set_pitch(braids_macro_osc_t *self,
                                              int16_t pitch)
{ self->pitch = pitch; }

static inline void braids_macro_osc_set_parameters(braids_macro_osc_t *self,
                                                   int16_t p1, int16_t p2)
{
    self->parameter[0] = p1;
    self->parameter[1] = p2;
}

static inline void braids_macro_osc_strike(braids_macro_osc_t *self)
{ braids_digital_osc_strike(&self->digital); }

/* Render `size` samples. `size` must be a multiple of 2 (some shapes
 * are 2×-downsampled) and a multiple of BRAIDS_MACRO_BLOCK is ideal
 * (the implementation pads with internal blocking so other multiples-
 * of-2 sizes work too, just less efficiently). */
void braids_macro_osc_render(braids_macro_osc_t *self,
                             int16_t *buffer, size_t size);

#endif /* LFE_BRAIDS_MACRO_H */
