/*
 * lfe_braids_analog.c — analog oscillator DSP.
 *
 * Ported from braids/analog_oscillator.cc (MIT, (c) 2012 Emilie
 * Gillet). Six shapes kept (SAW, VARIABLE_SAW, CSAW, SQUARE,
 * TRIANGLE, SINE); FOLD and BUZZ variants dropped.
 *
 * Port conventions:
 *   - `AnalogOscillator` class → `braids_analog_osc_t` struct
 *   - `this->member` → `self->member`
 *   - `static_cast<T>(x)` → `(T)(x)`
 *   - `bool` in upstream → plain `int` (stmlib.h already pulled
 *     `bool` via a pragma that we don't replicate)
 *   - `size_t` kept
 *   - Parameter / phase-increment interpolation macros moved to
 *     `lfe_braids_param_interp.h`.
 *   - BLEP helpers file-static (inline, but without the attribute
 *     since compile-time inlining is sufficient at O2).
 */

#include "lfe_braids_analog.h"

#include "lfe_braids_dsp.h"
#include "lfe_braids_resources.h"
#include "lfe_braids_param_interp.h"

/* Upstream analog_oscillator.cc static constants. */
#define BRAIDS_NUM_ZONES         15
#define BRAIDS_HIGHEST_NOTE      (128 * 128)
#define BRAIDS_PITCH_TABLE_START (128 * 128)
#define BRAIDS_OCTAVE            (12 * 128)

/* ---- Helpers ---- */

static inline int32_t this_blep_sample(uint32_t t)
{
    if (t > 65535) t = 65535;
    return (int32_t)((t * t) >> 18);
}

static inline int32_t next_blep_sample(uint32_t t)
{
    if (t > 65535) t = 65535;
    t = 65535 - t;
    return -(int32_t)((t * t) >> 18);
}

static uint32_t compute_phase_increment(int16_t midi_pitch)
{
    if (midi_pitch >= BRAIDS_HIGHEST_NOTE)
        midi_pitch = BRAIDS_HIGHEST_NOTE - 1;

    int32_t ref_pitch = midi_pitch - BRAIDS_PITCH_TABLE_START;
    size_t  num_shifts = 0;
    while (ref_pitch < 0) {
        ref_pitch += BRAIDS_OCTAVE;
        ++num_shifts;
    }

    uint32_t a = lut_oscillator_increments[ref_pitch >> 4];
    uint32_t b = lut_oscillator_increments[(ref_pitch >> 4) + 1];
    uint32_t pi = a + ((int32_t)(b - a) * (ref_pitch & 0xF) >> 4);
    pi >>= num_shifts;
    return pi;
}

/* ---- Init ---- */

/* Internal state-reset — used by both the public init and by the
 * dispatcher's shape-transition re-init. Does NOT touch `shape` /
 * `previous_shape`: the dispatcher wants those preserved (otherwise
 * the transition check fires every block and the phase accumulator
 * is wiped, producing quasi-saw output — the bug that had the MACRO
 * smoke tests failing). The public init fills them in afterward.
 */
static void analog_osc_reset_state(braids_analog_osc_t *self)
{
    self->phase                    = 0;
    self->phase_increment          = 1;
    self->previous_phase_increment = 1;
    self->high                     = 0;
    self->parameter                = 0;
    self->previous_parameter       = 0;
    self->aux_parameter            = 0;
    self->discontinuity_depth      = -16383;
    self->pitch                    = 60 << 7;
    self->next_sample              = 0;
}

void braids_analog_osc_init(braids_analog_osc_t *self)
{
    analog_osc_reset_state(self);
    self->shape          = BRAIDS_OSC_SHAPE_SAW;
    self->previous_shape = BRAIDS_OSC_SHAPE_SAW;
}

/* ---- Render functions ----
 *
 * Each follows the upstream structure closely. The naked `size--` loop
 * is kept rather than refactored so a side-by-side with
 * analog_oscillator.cc stays readable for later audits.
 */

static void render_saw(braids_analog_osc_t *self,
                       const uint8_t *sync_in, int16_t *buffer,
                       uint8_t *sync_out, size_t size)
{
    BRAIDS_BEGIN_INTERPOLATE_PHASE_INCREMENT
    int32_t next_sample = self->next_sample;
    while (size--) {
        int sync_reset = 0, self_reset = 0, transition_during_reset = 0;
        uint32_t reset_time = 0;

        BRAIDS_INTERPOLATE_PHASE_INCREMENT
        int32_t this_sample = next_sample;
        next_sample = 0;

        if (*sync_in) {
            reset_time = (uint32_t)(*sync_in - 1) << 9;
            uint32_t phase_at_reset = self->phase +
                (65535 - reset_time) * (phase_increment >> 16);
            sync_reset = 1;
            if (phase_at_reset < self->phase) transition_during_reset = 1;
            int32_t discontinuity = phase_at_reset >> 17;
            this_sample -= discontinuity * this_blep_sample(reset_time) >> 15;
            next_sample -= discontinuity * next_blep_sample(reset_time) >> 15;
        }
        sync_in++;

        self->phase += phase_increment;
        if (self->phase < phase_increment) self_reset = 1;

        if (sync_out) {
            *sync_out++ = self->phase < phase_increment
                ? self->phase / (phase_increment >> 7) + 1
                : 0;
        }

        if ((transition_during_reset || !sync_reset) && self_reset) {
            uint32_t t = self->phase / (phase_increment >> 16);
            this_sample -= this_blep_sample(t);
            next_sample -= next_blep_sample(t);
        }

        if (sync_reset) {
            self->phase = reset_time * (phase_increment >> 16);
            self->high = 0;
        }

        next_sample += self->phase >> 17;
        *buffer++ = (int16_t)((this_sample - 16384) << 1);
    }
    self->next_sample = next_sample;
    BRAIDS_END_INTERPOLATE_PHASE_INCREMENT
}

static void render_variable_saw(braids_analog_osc_t *self,
                                const uint8_t *sync_in, int16_t *buffer,
                                uint8_t *sync_out, size_t size)
{
    BRAIDS_BEGIN_INTERPOLATE_PHASE_INCREMENT
    int32_t next_sample = self->next_sample;
    if (self->parameter < 1024) self->parameter = 1024;
    while (size--) {
        int sync_reset = 0, self_reset = 0, transition_during_reset = 0;
        uint32_t reset_time = 0;

        BRAIDS_INTERPOLATE_PHASE_INCREMENT
        uint32_t pw = (uint32_t)self->parameter << 16;

        int32_t this_sample = next_sample;
        next_sample = 0;

        if (*sync_in) {
            reset_time = (uint32_t)(*sync_in - 1) << 9;
            uint32_t phase_at_reset = self->phase +
                (65535 - reset_time) * (phase_increment >> 16);
            sync_reset = 1;
            if (phase_at_reset < self->phase ||
                (!self->high && phase_at_reset >= pw))
                transition_during_reset = 1;
            int32_t before = (phase_at_reset >> 18) + ((phase_at_reset - pw) >> 18);
            int32_t after  = (0 >> 18) + ((0 - pw) >> 18);
            int32_t discontinuity = after - before;
            this_sample += discontinuity * this_blep_sample(reset_time) >> 15;
            next_sample += discontinuity * next_blep_sample(reset_time) >> 15;
        }
        sync_in++;

        self->phase += phase_increment;
        if (self->phase < phase_increment) self_reset = 1;

        if (sync_out) {
            *sync_out++ = self->phase < phase_increment
                ? self->phase / (phase_increment >> 7) + 1
                : 0;
        }

        while (transition_during_reset || !sync_reset) {
            if (!self->high) {
                if (self->phase < pw) break;
                uint32_t t = (self->phase - pw) / (phase_increment >> 16);
                this_sample -= this_blep_sample(t) >> 1;
                next_sample -= next_blep_sample(t) >> 1;
                self->high = 1;
            }
            if (self->high) {
                if (!self_reset) break;
                self_reset = 0;
                uint32_t t = self->phase / (phase_increment >> 16);
                this_sample -= this_blep_sample(t) >> 1;
                next_sample -= next_blep_sample(t) >> 1;
                self->high = 0;
            }
        }

        if (sync_reset) {
            self->phase = reset_time * (phase_increment >> 16);
            self->high = 0;
        }

        next_sample += self->phase >> 18;
        next_sample += (self->phase - pw) >> 18;
        *buffer++ = (int16_t)((this_sample - 16384) << 1);
    }
    self->next_sample = next_sample;
    BRAIDS_END_INTERPOLATE_PHASE_INCREMENT
}

static void render_csaw(braids_analog_osc_t *self,
                        const uint8_t *sync_in, int16_t *buffer,
                        uint8_t *sync_out, size_t size)
{
    BRAIDS_BEGIN_INTERPOLATE_PHASE_INCREMENT
    int32_t next_sample = self->next_sample;
    while (size--) {
        int sync_reset = 0, self_reset = 0, transition_during_reset = 0;
        uint32_t reset_time = 0;
        BRAIDS_INTERPOLATE_PHASE_INCREMENT
        uint32_t pw = (uint32_t)self->parameter * 49152;
        if (pw < 8 * phase_increment) pw = 8 * phase_increment;

        int32_t this_sample = next_sample;
        next_sample = 0;

        if (*sync_in) {
            reset_time = (uint32_t)(*sync_in - 1) << 9;
            uint32_t phase_at_reset = self->phase +
                (65535 - reset_time) * (phase_increment >> 16);
            sync_reset = 1;
            if (phase_at_reset < self->phase ||
                (!self->high && phase_at_reset >= pw))
                transition_during_reset = 1;
            if (self->phase >= pw) {
                self->discontinuity_depth = -2048 + (self->aux_parameter >> 2);
                int32_t before = (phase_at_reset >> 18);
                int16_t after  = self->discontinuity_depth;
                int32_t discontinuity = after - before;
                this_sample += discontinuity * this_blep_sample(reset_time) >> 15;
                next_sample += discontinuity * next_blep_sample(reset_time) >> 15;
            }
        }
        sync_in++;

        self->phase += phase_increment;
        if (self->phase < phase_increment) self_reset = 1;
        if (sync_out) {
            *sync_out++ = self->phase < phase_increment
                ? self->phase / (phase_increment >> 7) + 1
                : 0;
        }

        while (transition_during_reset || !sync_reset) {
            if (!self->high) {
                if (self->phase < pw) break;
                uint32_t t = (self->phase - pw) / (phase_increment >> 16);
                int16_t before = self->discontinuity_depth;
                int16_t after  = self->phase >> 18;
                int16_t discontinuity = after - before;
                this_sample += discontinuity * this_blep_sample(t) >> 15;
                next_sample += discontinuity * next_blep_sample(t) >> 15;
                self->high = 1;
            }
            if (self->high) {
                if (!self_reset) break;
                self_reset = 0;
                self->discontinuity_depth = -2048 + (self->aux_parameter >> 2);
                uint32_t t = self->phase / (phase_increment >> 16);
                int16_t before = 16383;
                int16_t after  = self->discontinuity_depth;
                int16_t discontinuity = after - before;
                this_sample += discontinuity * this_blep_sample(t) >> 15;
                next_sample += discontinuity * next_blep_sample(t) >> 15;
                self->high = 0;
            }
        }

        if (sync_reset) {
            self->phase = reset_time * (phase_increment >> 16);
            self->high = 0;
        }

        next_sample += self->phase < pw
            ? self->discontinuity_depth
            : self->phase >> 18;
        *buffer++ = (int16_t)((this_sample - 8192) << 1);
    }
    self->next_sample = next_sample;
    BRAIDS_END_INTERPOLATE_PHASE_INCREMENT
}

static void render_square(braids_analog_osc_t *self,
                          const uint8_t *sync_in, int16_t *buffer,
                          uint8_t *sync_out, size_t size)
{
    BRAIDS_BEGIN_INTERPOLATE_PHASE_INCREMENT
    if (self->parameter > 32000) self->parameter = 32000;
    int32_t next_sample = self->next_sample;
    while (size--) {
        int sync_reset = 0, self_reset = 0, transition_during_reset = 0;
        uint32_t reset_time = 0;
        BRAIDS_INTERPOLATE_PHASE_INCREMENT
        uint32_t pw = (uint32_t)(32768 - self->parameter) << 16;

        int32_t this_sample = next_sample;
        next_sample = 0;

        if (*sync_in) {
            reset_time = (uint32_t)(*sync_in - 1) << 9;
            uint32_t phase_at_reset = self->phase +
                (65535 - reset_time) * (phase_increment >> 16);
            sync_reset = 1;
            if (phase_at_reset < self->phase ||
                (!self->high && phase_at_reset >= pw))
                transition_during_reset = 1;
            if (phase_at_reset >= pw) {
                this_sample -= this_blep_sample(reset_time);
                next_sample -= next_blep_sample(reset_time);
            }
        }
        sync_in++;

        self->phase += phase_increment;
        if (self->phase < phase_increment) self_reset = 1;
        if (sync_out) {
            *sync_out++ = self->phase < phase_increment
                ? self->phase / (phase_increment >> 7) + 1
                : 0;
        }

        while (transition_during_reset || !sync_reset) {
            if (!self->high) {
                if (self->phase < pw) break;
                uint32_t t = (self->phase - pw) / (phase_increment >> 16);
                this_sample += this_blep_sample(t);
                next_sample += next_blep_sample(t);
                self->high = 1;
            }
            if (self->high) {
                if (!self_reset) break;
                self_reset = 0;
                uint32_t t = self->phase / (phase_increment >> 16);
                this_sample -= this_blep_sample(t);
                next_sample -= next_blep_sample(t);
                self->high = 0;
            }
        }

        if (sync_reset) {
            self->phase = reset_time * (phase_increment >> 16);
            self->high = 0;
        }

        next_sample += self->phase < pw ? 0 : 32767;
        *buffer++ = (int16_t)((this_sample - 16384) << 1);
    }
    self->next_sample = next_sample;
    BRAIDS_END_INTERPOLATE_PHASE_INCREMENT
}

static void render_triangle(braids_analog_osc_t *self,
                            const uint8_t *sync_in, int16_t *buffer,
                            uint8_t *sync_out, size_t size)
{
    (void)sync_out;
    BRAIDS_BEGIN_INTERPOLATE_PHASE_INCREMENT
    uint32_t phase = self->phase;
    while (size--) {
        BRAIDS_INTERPOLATE_PHASE_INCREMENT
        int16_t  triangle;
        uint16_t phase_16;

        if (*sync_in++) phase = 0;

        /* 2x oversampled waveform */
        phase += phase_increment >> 1;
        phase_16 = phase >> 16;
        triangle = (phase_16 << 1) ^ (phase_16 & 0x8000 ? 0xffff : 0x0000);
        triangle += 32768;
        *buffer = triangle >> 1;

        phase += phase_increment >> 1;
        phase_16 = phase >> 16;
        triangle = (phase_16 << 1) ^ (phase_16 & 0x8000 ? 0xffff : 0x0000);
        triangle += 32768;
        *buffer++ += triangle >> 1;
    }
    self->phase = phase;
    BRAIDS_END_INTERPOLATE_PHASE_INCREMENT
}

static void render_sine(braids_analog_osc_t *self,
                        const uint8_t *sync_in, int16_t *buffer,
                        uint8_t *sync_out, size_t size)
{
    (void)sync_out;
    uint32_t phase = self->phase;
    BRAIDS_BEGIN_INTERPOLATE_PHASE_INCREMENT
    while (size--) {
        BRAIDS_INTERPOLATE_PHASE_INCREMENT
        phase += phase_increment;
        if (*sync_in++) phase = 0;
        *buffer++ = braids_interp824_s16(wav_sine, phase);
    }
    BRAIDS_END_INTERPOLATE_PHASE_INCREMENT
    self->phase = phase;
}

/* ---- Triangle fold + sine fold ----
 *
 * 2× oversampled wavefolders used by MACRO `RenderSineTriangle`.
 * `parameter` controls the fold depth via `gain = 2048 + param * 30720/Q15`,
 * so at param=0 the fold is near-identity.
 */

static void render_triangle_fold(braids_analog_osc_t *self,
                                 const uint8_t *sync_in, int16_t *buffer,
                                 uint8_t *sync_out, size_t size)
{
    (void)sync_out;
    uint32_t phase = self->phase;
    BRAIDS_BEGIN_INTERPOLATE_PHASE_INCREMENT
    BRAIDS_BEGIN_INTERPOLATE_PARAMETER

    while (size--) {
        BRAIDS_INTERPOLATE_PARAMETER
        BRAIDS_INTERPOLATE_PHASE_INCREMENT
        uint16_t phase_16;
        int16_t  tri;
        int16_t  gain = (int16_t)(2048 + (parameter * 30720 >> 15));

        if (*sync_in++) phase = 0;

        /* 2x oversampled WF. */
        phase += phase_increment >> 1;
        phase_16 = phase >> 16;
        tri = (phase_16 << 1) ^ (phase_16 & 0x8000 ? 0xFFFF : 0x0000);
        tri = (int16_t)(tri + 32768);
        tri = (int16_t)(tri * gain >> 15);
        tri = braids_interp88_s16(ws_tri_fold, (uint16_t)(tri + 32768));
        *buffer = tri >> 1;

        phase += phase_increment >> 1;
        phase_16 = phase >> 16;
        tri = (phase_16 << 1) ^ (phase_16 & 0x8000 ? 0xFFFF : 0x0000);
        tri = (int16_t)(tri + 32768);
        tri = (int16_t)(tri * gain >> 15);
        tri = braids_interp88_s16(ws_tri_fold, (uint16_t)(tri + 32768));
        *buffer++ += tri >> 1;
    }
    BRAIDS_END_INTERPOLATE_PARAMETER
    BRAIDS_END_INTERPOLATE_PHASE_INCREMENT
    self->phase = phase;
}

static void render_sine_fold(braids_analog_osc_t *self,
                             const uint8_t *sync_in, int16_t *buffer,
                             uint8_t *sync_out, size_t size)
{
    (void)sync_out;
    uint32_t phase = self->phase;
    BRAIDS_BEGIN_INTERPOLATE_PHASE_INCREMENT
    BRAIDS_BEGIN_INTERPOLATE_PARAMETER

    while (size--) {
        BRAIDS_INTERPOLATE_PARAMETER
        BRAIDS_INTERPOLATE_PHASE_INCREMENT
        int16_t sine;
        int16_t gain = (int16_t)(2048 + (parameter * 30720 >> 15));

        if (*sync_in++) phase = 0;

        /* 2x oversampled WF. */
        phase += phase_increment >> 1;
        sine = braids_interp824_s16(wav_sine, phase);
        sine = (int16_t)(sine * gain >> 15);
        sine = braids_interp88_s16(ws_sine_fold, (uint16_t)(sine + 32768));
        *buffer = sine >> 1;

        phase += phase_increment >> 1;
        sine = braids_interp824_s16(wav_sine, phase);
        sine = (int16_t)(sine * gain >> 15);
        sine = braids_interp88_s16(ws_sine_fold, (uint16_t)(sine + 32768));
        *buffer++ += sine >> 1;
    }
    BRAIDS_END_INTERPOLATE_PARAMETER
    BRAIDS_END_INTERPOLATE_PHASE_INCREMENT
    self->phase = phase;
}

/* ---- Dispatch table ---- */

typedef void (*render_fn)(braids_analog_osc_t *,
                          const uint8_t *, int16_t *, uint8_t *, size_t);

static const render_fn fn_table[] = {
    render_saw,             /* SAW            */
    render_variable_saw,    /* VARIABLE_SAW   */
    render_csaw,            /* CSAW           */
    render_square,          /* SQUARE         */
    render_triangle,        /* TRIANGLE       */
    render_sine,            /* SINE           */
    render_triangle_fold,   /* TRIANGLE_FOLD  */
    render_sine_fold,       /* SINE_FOLD      */
};

void braids_analog_osc_render(braids_analog_osc_t *self,
                              const uint8_t *sync_in,
                              int16_t *buffer,
                              uint8_t *sync_out,
                              size_t size)
{
    render_fn fn = fn_table[self->shape];

    if (self->shape != self->previous_shape) {
        /* State reset only — preserve caller's current `shape`. */
        analog_osc_reset_state(self);
        self->previous_shape = self->shape;
    }

    self->phase_increment = compute_phase_increment(self->pitch);

    if (self->pitch > BRAIDS_HIGHEST_NOTE) self->pitch = BRAIDS_HIGHEST_NOTE;
    else if (self->pitch < 0)              self->pitch = 0;

    fn(self, sync_in, buffer, sync_out, size);
}
