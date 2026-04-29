/*
 * lfe_braids_param_interp.h — per-sample parameter & phase-increment
 * linear interpolation macros.
 *
 * Ported from braids/parameter_interpolation.h (MIT, (c) 2012 Emilie
 * Gillet). Kept as a header of macros (rather than inlined at call
 * sites, as the original plan suggested) because both analog and
 * digital oscillator .c files need the exact same ramp-through-block
 * pattern and repeating it by hand is worse.
 *
 * Usage pattern:
 *
 *   BRAIDS_BEGIN_INTERPOLATE_PARAMETER
 *   while (size--) {
 *       BRAIDS_INTERPOLATE_PARAMETER   // creates `int32_t parameter`
 *       ...
 *   }
 *   BRAIDS_END_INTERPOLATE_PARAMETER
 *
 * The macros reference struct fields by name (`self->parameter`,
 * `self->previous_parameter`, `size`, etc.) — so the caller must
 * follow that naming. `self` is the C port's analogue of C++'s
 * `this`.
 */

#ifndef LFE_BRAIDS_PARAM_INTERP_H
#define LFE_BRAIDS_PARAM_INTERP_H

/* ---- single parameter (scalar `parameter_` member) ---- */

#define BRAIDS_BEGIN_INTERPOLATE_PARAMETER                                \
    int32_t parameter_start      = self->previous_parameter;              \
    int32_t parameter_delta      = self->parameter - self->previous_parameter; \
    int32_t parameter_increment  = 32767 / (int32_t)size;                 \
    int32_t parameter_xfade      = 0;

#define BRAIDS_INTERPOLATE_PARAMETER                                      \
    parameter_xfade += parameter_increment;                               \
    int32_t parameter = parameter_start +                                 \
        (parameter_delta * parameter_xfade >> 15);

#define BRAIDS_END_INTERPOLATE_PARAMETER                                  \
    self->previous_parameter = self->parameter;

/* ---- phase-increment (smooths pitch jumps across a block) ---- */

#define BRAIDS_BEGIN_INTERPOLATE_PHASE_INCREMENT                          \
    uint32_t phase_increment = self->previous_phase_increment;            \
    uint32_t phase_increment_increment =                                  \
        self->previous_phase_increment < self->phase_increment            \
        ? (self->phase_increment - self->previous_phase_increment) / (uint32_t)size \
        : ~((self->previous_phase_increment - self->phase_increment) / (uint32_t)size);

#define BRAIDS_INTERPOLATE_PHASE_INCREMENT                                \
    phase_increment += phase_increment_increment;

#define BRAIDS_END_INTERPOLATE_PHASE_INCREMENT                            \
    self->previous_phase_increment = phase_increment;

#endif /* LFE_BRAIDS_PARAM_INTERP_H */
