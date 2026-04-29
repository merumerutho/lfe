/*
 * lfe_braids_macro.c — MacroOscillator dispatch.
 *
 * Ported from braids/macro_oscillator.cc. See lfe_braids_macro.h for
 * the public contract and scope.
 */

#include "lfe_braids_macro.h"

#include <string.h>

#include "lfe_braids_dsp.h"
#include "lfe_braids_resources.h"
#include "lfe_braids_param_interp.h"

/* ---- Init ---- */

void braids_macro_osc_init(braids_macro_osc_t *self)
{
    memset(self->sync, 0, sizeof(self->sync));
    memset(self->temp, 0, sizeof(self->temp));

    for (int i = 0; i < 3; i++) braids_analog_osc_init(&self->analog[i]);
    braids_digital_osc_init(&self->digital);

    self->parameter[0]            = 0;
    self->parameter[1]            = 0;
    self->previous_parameter[0]   = 0;
    self->previous_parameter[1]   = 0;
    self->pitch                   = 60 << 7;
    self->lp_state                = 0;
    self->shape                   = BRAIDS_MACRO_CSAW;
    self->previous_shape          = BRAIDS_MACRO_CSAW;
}

/* ---- Per-shape renderers (all process `size` samples at once; caller
 * has already blocked to BRAIDS_MACRO_BLOCK). The `self->temp` buffer
 * is sized to BRAIDS_MACRO_BLOCK, so each shape assumes size <=
 * BRAIDS_MACRO_BLOCK. ---- */

static void render_csaw(braids_macro_osc_t *self, int16_t *buffer, size_t size)
{
    braids_analog_osc_set_pitch(&self->analog[0], self->pitch);
    braids_analog_osc_set_shape(&self->analog[0], BRAIDS_OSC_SHAPE_CSAW);
    braids_analog_osc_set_parameter(&self->analog[0], self->parameter[0]);
    braids_analog_osc_set_aux_parameter(&self->analog[0], self->parameter[1]);
    braids_analog_osc_render(&self->analog[0], self->sync, buffer, NULL, size);

    /* Post-filter per upstream: DC shift + amplitude scale. */
    int16_t shift = (int16_t)(-(self->parameter[1] - 32767) >> 4);
    for (size_t i = 0; i < size; i++) {
        int32_t s = (int32_t)buffer[i] + shift;
        buffer[i] = (int16_t)((s * 13) >> 3);
    }
}

static void render_morph(braids_macro_osc_t *self, int16_t *buffer, size_t size)
{
    braids_analog_osc_set_pitch(&self->analog[0], self->pitch);
    braids_analog_osc_set_pitch(&self->analog[1], self->pitch);

    uint16_t balance;
    if (self->parameter[0] <= 10922) {
        braids_analog_osc_set_parameter(&self->analog[0], 0);
        braids_analog_osc_set_parameter(&self->analog[1], 0);
        braids_analog_osc_set_shape(&self->analog[0], BRAIDS_OSC_SHAPE_TRIANGLE);
        braids_analog_osc_set_shape(&self->analog[1], BRAIDS_OSC_SHAPE_SAW);
        balance = (uint16_t)(self->parameter[0] * 6);
    } else if (self->parameter[0] <= 21845) {
        braids_analog_osc_set_parameter(&self->analog[0], 0);
        braids_analog_osc_set_parameter(&self->analog[1], 0);
        braids_analog_osc_set_shape(&self->analog[0], BRAIDS_OSC_SHAPE_SQUARE);
        braids_analog_osc_set_shape(&self->analog[1], BRAIDS_OSC_SHAPE_SAW);
        balance = (uint16_t)(65535 - (self->parameter[0] - 10923) * 6);
    } else {
        braids_analog_osc_set_parameter(&self->analog[0],
                                        (int16_t)((self->parameter[0] - 21846) * 3));
        braids_analog_osc_set_parameter(&self->analog[1], 0);
        braids_analog_osc_set_shape(&self->analog[0], BRAIDS_OSC_SHAPE_SQUARE);
        braids_analog_osc_set_shape(&self->analog[1], BRAIDS_OSC_SHAPE_SINE);
        balance = 0;
    }

    int16_t *shape_1 = buffer;
    int16_t *shape_2 = self->temp;
    braids_analog_osc_render(&self->analog[0], self->sync, shape_1, NULL, size);
    braids_analog_osc_render(&self->analog[1], self->sync, shape_2, NULL, size);

    int32_t lp_cutoff = self->pitch - (self->parameter[1] >> 1) + 128 * 128;
    if (lp_cutoff < 0) lp_cutoff = 0;
    else if (lp_cutoff > 32767) lp_cutoff = 32767;
    int32_t f = braids_interp824_u16(lut_svf_cutoff, (uint32_t)lp_cutoff << 17);
    int32_t lp_state = self->lp_state;
    int32_t fuzz_amount = self->parameter[1] << 1;
    if (self->pitch > (80 << 7)) {
        fuzz_amount -= (self->pitch - (80 << 7)) << 4;
        if (fuzz_amount < 0) fuzz_amount = 0;
    }
    for (size_t i = 0; i < size; i++) {
        int16_t sample = braids_mix_s16(shape_1[i], shape_2[i], balance);
        int32_t shifted = sample;
        lp_state += (shifted - lp_state) * f >> 15;
        BRAIDS_CLIP(lp_state);
        shifted = lp_state + 32768;
        int16_t fuzzed = braids_interp88_s16(ws_violent_overdrive,
                                             (uint16_t)shifted);
        buffer[i] = braids_mix_s16(sample, fuzzed, (uint16_t)fuzz_amount);
    }
    self->lp_state = lp_state;
}

static void render_saw_square(braids_macro_osc_t *self, int16_t *buffer, size_t size)
{
    braids_analog_osc_set_parameter(&self->analog[0], self->parameter[0]);
    braids_analog_osc_set_parameter(&self->analog[1], self->parameter[0]);
    braids_analog_osc_set_pitch(&self->analog[0], self->pitch);
    braids_analog_osc_set_pitch(&self->analog[1], self->pitch);
    braids_analog_osc_set_shape(&self->analog[0], BRAIDS_OSC_SHAPE_VARIABLE_SAW);
    braids_analog_osc_set_shape(&self->analog[1], BRAIDS_OSC_SHAPE_SQUARE);

    braids_analog_osc_render(&self->analog[0], self->sync, buffer, NULL, size);
    braids_analog_osc_render(&self->analog[1], self->sync, self->temp, NULL, size);

    /* Inline interpolation of parameter[0] across the block. */
    int32_t p_start = self->previous_parameter[0];
    int32_t p_delta = self->parameter[0] - self->previous_parameter[0];
    int32_t p_inc   = 32767 / (int32_t)size;
    int32_t p_xf    = 0;
    for (size_t i = 0; i < size; i++) {
        p_xf += p_inc;
        int32_t parameter_1 = p_start + (p_delta * p_xf >> 15);
        uint16_t balance = (uint16_t)(parameter_1 << 1);
        int16_t atten_sq = (int16_t)((int32_t)self->temp[i] * 148 >> 8);
        buffer[i] = braids_mix_s16(buffer[i], atten_sq, balance);
    }
    self->previous_parameter[0] = self->parameter[0];
}

static void render_sine_triangle(braids_macro_osc_t *self, int16_t *buffer, size_t size)
{
    int32_t atten_sine = 32767 - 6 * (self->pitch - (92 << 7));
    int32_t atten_tri  = 32767 - 7 * (self->pitch - (80 << 7));
    if (atten_tri  < 0)      atten_tri  = 0;
    if (atten_sine < 0)      atten_sine = 0;
    if (atten_tri  > 32767)  atten_tri  = 32767;
    if (atten_sine > 32767)  atten_sine = 32767;

    int32_t timbre = self->parameter[0];
    braids_analog_osc_set_parameter(&self->analog[0], (int16_t)(timbre * atten_sine >> 15));
    braids_analog_osc_set_parameter(&self->analog[1], (int16_t)(timbre * atten_tri  >> 15));
    braids_analog_osc_set_pitch(&self->analog[0], self->pitch);
    braids_analog_osc_set_pitch(&self->analog[1], self->pitch);
    braids_analog_osc_set_shape(&self->analog[0], BRAIDS_OSC_SHAPE_SINE_FOLD);
    braids_analog_osc_set_shape(&self->analog[1], BRAIDS_OSC_SHAPE_TRIANGLE_FOLD);

    braids_analog_osc_render(&self->analog[0], self->sync, buffer, NULL, size);
    braids_analog_osc_render(&self->analog[1], self->sync, self->temp, NULL, size);

    int32_t p_start = self->previous_parameter[1];
    int32_t p_delta = self->parameter[1] - self->previous_parameter[1];
    int32_t p_inc   = 32767 / (int32_t)size;
    int32_t p_xf    = 0;
    for (size_t i = 0; i < size; i++) {
        p_xf += p_inc;
        int32_t parameter_1 = p_start + (p_delta * p_xf >> 15);
        uint16_t balance = (uint16_t)(parameter_1 << 1);
        buffer[i] = braids_mix_s16(buffer[i], self->temp[i], balance);
    }
    self->previous_parameter[1] = self->parameter[1];
}

/* ---- Triple oscillator ----
 *
 * Three analog oscs of the same base shape, voice 0 at the pitch and
 * voices 1-2 detuned by `intervals[]`-scaled amounts driven by
 * parameter[0] and parameter[1].
 */

#define BR_SEMI * 128
static const int16_t k_triple_intervals[65] = {
    -24 BR_SEMI, -24 BR_SEMI, -24 BR_SEMI + 4,
    -23 BR_SEMI, -22 BR_SEMI, -21 BR_SEMI, -20 BR_SEMI, -19 BR_SEMI, -18 BR_SEMI,
    -17 BR_SEMI - 4, -17 BR_SEMI,
    -16 BR_SEMI, -15 BR_SEMI, -14 BR_SEMI, -13 BR_SEMI,
    -12 BR_SEMI - 4, -12 BR_SEMI,
    -11 BR_SEMI, -10 BR_SEMI, -9 BR_SEMI, -8 BR_SEMI,
    -7 BR_SEMI - 4, -7 BR_SEMI,
    -6 BR_SEMI, -5 BR_SEMI, -4 BR_SEMI, -3 BR_SEMI, -2 BR_SEMI, -1 BR_SEMI,
    -24, -8, -4, 0, 4, 8, 24,
    1 BR_SEMI, 2 BR_SEMI, 3 BR_SEMI, 4 BR_SEMI, 5 BR_SEMI, 6 BR_SEMI,
    7 BR_SEMI, 7 BR_SEMI + 4,
    8 BR_SEMI, 9 BR_SEMI, 10 BR_SEMI, 11 BR_SEMI,
    12 BR_SEMI, 12 BR_SEMI + 4,
    13 BR_SEMI, 14 BR_SEMI, 15 BR_SEMI, 16 BR_SEMI,
    17 BR_SEMI, 17 BR_SEMI + 4,
    18 BR_SEMI, 19 BR_SEMI, 20 BR_SEMI, 21 BR_SEMI, 22 BR_SEMI, 23 BR_SEMI,
    24 BR_SEMI - 4, 24 BR_SEMI, 24 BR_SEMI
};
#undef BR_SEMI

static void render_triple_core(braids_macro_osc_t *self, int16_t *buffer,
                               size_t size, braids_analog_shape base_shape)
{
    braids_analog_osc_set_parameter(&self->analog[0], 0);
    braids_analog_osc_set_parameter(&self->analog[1], 0);
    braids_analog_osc_set_parameter(&self->analog[2], 0);

    braids_analog_osc_set_pitch(&self->analog[0], self->pitch);
    for (size_t v = 0; v < 2; ++v) {
        int16_t d1 = k_triple_intervals[(self->parameter[v] >> 9)];
        int16_t d2 = k_triple_intervals[((self->parameter[v] >> 8) + 1) >> 1];
        uint16_t xfade = (uint16_t)(self->parameter[v] << 8);
        int16_t detune = (int16_t)(d1 + ((d2 - d1) * xfade >> 16));
        braids_analog_osc_set_pitch(&self->analog[v + 1],
                                    (int16_t)(self->pitch + detune));
    }

    braids_analog_osc_set_shape(&self->analog[0], base_shape);
    braids_analog_osc_set_shape(&self->analog[1], base_shape);
    braids_analog_osc_set_shape(&self->analog[2], base_shape);

    memset(buffer, 0, size * sizeof(int16_t));
    for (int v = 0; v < 3; ++v) {
        braids_analog_osc_render(&self->analog[v], self->sync,
                                 self->temp, NULL, size);
        for (size_t i = 0; i < size; i++)
            buffer[i] = (int16_t)(buffer[i] + (self->temp[i] * 21 >> 6));
    }
}

/* ---- Digital delegate (for shapes whose render lives in the digital
 * oscillator). ---- */
static void render_digital_delegate(braids_macro_osc_t *self, int16_t *buffer,
                                    size_t size, braids_digital_shape ds)
{
    braids_digital_osc_set_parameters(&self->digital,
                                      self->parameter[0], self->parameter[1]);
    braids_digital_osc_set_pitch(&self->digital, self->pitch);
    braids_digital_osc_set_shape(&self->digital, ds);
    braids_digital_osc_render(&self->digital, self->sync, buffer, size);
}

/* ---- SAW_COMB: analog SAW into buffer, then digital COMB_FILTER in-place. ---- */
static void render_saw_comb(braids_macro_osc_t *self, int16_t *buffer, size_t size)
{
    braids_analog_osc_set_parameter(&self->analog[0], 0);
    braids_analog_osc_set_pitch(&self->analog[0], self->pitch);
    braids_analog_osc_set_shape(&self->analog[0], BRAIDS_OSC_SHAPE_SAW);
    braids_analog_osc_render(&self->analog[0], self->sync, buffer, NULL, size);

    braids_digital_osc_set_parameters(&self->digital,
                                      self->parameter[0], self->parameter[1]);
    braids_digital_osc_set_pitch(&self->digital, self->pitch);
    braids_digital_osc_set_shape(&self->digital, BRAIDS_DOSC_SHAPE_COMB_FILTER);
    braids_digital_osc_render(&self->digital, self->sync, buffer, size);
}

/* ---- Block dispatcher ---- */

static void render_block(braids_macro_osc_t *self, int16_t *buffer, size_t n)
{
    switch (self->shape) {
    case BRAIDS_MACRO_CSAW:            render_csaw(self, buffer, n); break;
    case BRAIDS_MACRO_MORPH:           render_morph(self, buffer, n); break;
    case BRAIDS_MACRO_SAW_SQUARE:      render_saw_square(self, buffer, n); break;
    case BRAIDS_MACRO_SINE_TRIANGLE:   render_sine_triangle(self, buffer, n); break;
    case BRAIDS_MACRO_TRIPLE_SAW:      render_triple_core(self, buffer, n, BRAIDS_OSC_SHAPE_SAW); break;
    case BRAIDS_MACRO_TRIPLE_SQUARE:   render_triple_core(self, buffer, n, BRAIDS_OSC_SHAPE_SQUARE); break;
    case BRAIDS_MACRO_TRIPLE_TRIANGLE: render_triple_core(self, buffer, n, BRAIDS_OSC_SHAPE_TRIANGLE); break;
    case BRAIDS_MACRO_TRIPLE_SINE:     render_triple_core(self, buffer, n, BRAIDS_OSC_SHAPE_SINE); break;
    case BRAIDS_MACRO_TRIPLE_RING_MOD: render_digital_delegate(self, buffer, n, BRAIDS_DOSC_SHAPE_TRIPLE_RING_MOD); break;
    case BRAIDS_MACRO_SAW_SWARM:       render_digital_delegate(self, buffer, n, BRAIDS_DOSC_SHAPE_SAW_SWARM); break;
    case BRAIDS_MACRO_SAW_COMB:        render_saw_comb(self, buffer, n); break;
    case BRAIDS_MACRO_VOWEL_FOF:       render_digital_delegate(self, buffer, n, BRAIDS_DOSC_SHAPE_VOWEL_FOF); break;
    case BRAIDS_MACRO_PLUCKED:         render_digital_delegate(self, buffer, n, BRAIDS_DOSC_SHAPE_PLUCKED); break;
    case BRAIDS_MACRO_BOWED:           render_digital_delegate(self, buffer, n, BRAIDS_DOSC_SHAPE_BOWED); break;
    case BRAIDS_MACRO_BLOWN:           render_digital_delegate(self, buffer, n, BRAIDS_DOSC_SHAPE_BLOWN); break;
    case BRAIDS_MACRO_FLUTED:          render_digital_delegate(self, buffer, n, BRAIDS_DOSC_SHAPE_FLUTED); break;
    case BRAIDS_MACRO_TWIN_PEAKS_NOISE:       render_digital_delegate(self, buffer, n, BRAIDS_DOSC_SHAPE_TWIN_PEAKS_NOISE); break;
    case BRAIDS_MACRO_DIGITAL_MODULATION:     render_digital_delegate(self, buffer, n, BRAIDS_DOSC_SHAPE_DIGITAL_MODULATION); break;
    default: memset(buffer, 0, n * sizeof(int16_t)); break;
    }
}

void braids_macro_osc_render(braids_macro_osc_t *self,
                             int16_t *buffer, size_t size)
{
    if (self->shape != self->previous_shape) {
        /* Don't re-init the analog osc state here — individual render fns
         * overwrite shape+parameter at the start of each block, and
         * keeping phase continuity avoids clicks on shape switches that
         * share a timbre corridor. */
        self->previous_shape = self->shape;
    }

    /* Block the request into BRAIDS_MACRO_BLOCK-sized chunks so the
     * temp buffer fits on stack. */
    size_t done = 0;
    while (done < size) {
        size_t n = size - done;
        if (n > BRAIDS_MACRO_BLOCK) n = BRAIDS_MACRO_BLOCK;
        render_block(self, buffer + done, n);
        done += n;
    }
}
