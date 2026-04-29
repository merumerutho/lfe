/*
 * lfe_braids_analog.h — ported Braids "analog" oscillator.
 *
 * Ported from braids/analog_oscillator.{h,cc} (MIT, (c) 2012 Emilie
 * Gillet). Only the six shapes reached by the 18 MACRO shapes in
 * scope are retained:
 *
 *   SAW, VARIABLE_SAW, CSAW, SQUARE, TRIANGLE, SINE
 *
 * Dropped: TRIANGLE_FOLD, SINE_FOLD, BUZZ (no MACRO shape routes to
 * them in our selection; saves ~150 LOC + the ws_sine_fold / ws_tri_fold
 * references).
 *
 * Public enum values retain their upstream ordinals even where we've
 * dropped a shape, so calls like `set_shape(OSC_SHAPE_SINE)` still
 * resolve to the same numeric id as in the C++ source — easier to
 * cross-reference against the upstream macro_oscillator.cc dispatch.
 */

#ifndef LFE_BRAIDS_ANALOG_H
#define LFE_BRAIDS_ANALOG_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    BRAIDS_OSC_SHAPE_SAW           = 0,
    BRAIDS_OSC_SHAPE_VARIABLE_SAW  = 1,
    BRAIDS_OSC_SHAPE_CSAW          = 2,
    BRAIDS_OSC_SHAPE_SQUARE        = 3,
    BRAIDS_OSC_SHAPE_TRIANGLE      = 4,
    BRAIDS_OSC_SHAPE_SINE          = 5,
    BRAIDS_OSC_SHAPE_TRIANGLE_FOLD = 6,  /* needed by MACRO SINE_TRIANGLE */
    BRAIDS_OSC_SHAPE_SINE_FOLD     = 7   /* needed by MACRO SINE_TRIANGLE */
    /* BUZZ=8 dropped */
} braids_analog_shape;

typedef struct {
    uint32_t phase;
    uint32_t phase_increment;
    uint32_t previous_phase_increment;
    int high;                     /* bool: square-wave half */

    int16_t parameter;
    int16_t previous_parameter;
    int16_t aux_parameter;
    int16_t discontinuity_depth;
    int16_t pitch;

    int32_t next_sample;

    braids_analog_shape shape;
    braids_analog_shape previous_shape;
} braids_analog_osc_t;

void braids_analog_osc_init(braids_analog_osc_t *self);

static inline void braids_analog_osc_set_shape(braids_analog_osc_t *self,
                                               braids_analog_shape shape)
{ self->shape = shape; }

static inline void braids_analog_osc_set_pitch(braids_analog_osc_t *self,
                                               int16_t pitch)
{ self->pitch = pitch; }

static inline void braids_analog_osc_set_parameter(braids_analog_osc_t *self,
                                                   int16_t parameter)
{ self->parameter = parameter; }

static inline void braids_analog_osc_set_aux_parameter(braids_analog_osc_t *self,
                                                       int16_t parameter)
{ self->aux_parameter = parameter; }

static inline uint32_t braids_analog_osc_phase_increment(const braids_analog_osc_t *self)
{ return self->phase_increment; }

static inline void braids_analog_osc_reset(braids_analog_osc_t *self)
{ self->phase = -self->phase_increment; }

/* Render `size` samples. `sync_in` is an optional hard-sync trigger
 * stream (NULL → no sync); `sync_out` is an optional emit of this
 * oscillator's own reset times (NULL → don't emit).
 * `buffer` receives `size` int16 samples. */
void braids_analog_osc_render(braids_analog_osc_t *self,
                              const uint8_t *sync_in,
                              int16_t *buffer,
                              uint8_t *sync_out,
                              size_t size);

#endif /* LFE_BRAIDS_ANALOG_H */
