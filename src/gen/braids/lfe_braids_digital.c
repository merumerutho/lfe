/*
 * lfe_braids_digital.c — digital oscillator DSP.
 *
 * Ported from braids/digital_oscillator.cc (MIT, (c) 2012 Emilie
 * Gillet). Sub-phase 6e-1 lands:
 *   - scaffolding (Init, Render dispatcher, helpers)
 *   - VOWEL_FOF
 *
 * Subsequent sub-phases (6e-2 .. 6e-5) fill in the remaining six
 * shapes. Their slots in fn_table are NULL until then; the Render
 * dispatcher guards against a NULL entry.
 */

#include "lfe_braids_digital.h"

#include <string.h>

#include "lfe_braids_dsp.h"
#include "lfe_braids_resources.h"
#include "lfe_braids_random.h"
#include "lfe_braids_param_interp.h"

/* ---- Upstream constants ---- */

#define BRAIDS_DOSC_HIGHEST_NOTE      (140 * 128)
#define BRAIDS_DOSC_PITCH_TABLE_START (128 * 128)
#define BRAIDS_DOSC_OCTAVE            (12 * 128)

/* ---- Init ---- */

/* Internal state-reset. Wipes every field EXCEPT `shape` /
 * `previous_shape` — the dispatcher relies on those being preserved
 * across its shape-transition re-init. See matching comment in
 * lfe_braids_analog.c. */
static void digital_osc_reset_state(braids_digital_osc_t *self)
{
    memset(&self->state, 0, sizeof(self->state));
    memset(&self->delay_lines, 0, sizeof(self->delay_lines));

    for (int i = 0; i < 4; i++)
        braids_excitation_init(&self->pulse[i]);
    for (int i = 0; i < 3; i++)
        braids_svf_init(&self->svf_unit[i]);

    self->phase                 = 0;
    self->phase_increment       = 0;
    self->delay                 = 0;
    self->parameter[0]          = 0;
    self->parameter[1]          = 0;
    self->previous_parameter[0] = 0;
    self->previous_parameter[1] = 0;
    self->smoothed_parameter    = 0;
    self->pitch                 = 60 << 7;
    self->active_voice          = 0;
    self->init                  = 1;
    self->strike                = 1;
}

void braids_digital_osc_init(braids_digital_osc_t *self)
{
    digital_osc_reset_state(self);
    self->shape          = BRAIDS_DOSC_SHAPE_VOWEL_FOF;
    self->previous_shape = BRAIDS_DOSC_SHAPE_VOWEL_FOF;
}

/* ---- Helpers ---- */

static uint32_t compute_phase_increment(int16_t midi_pitch)
{
    if (midi_pitch >= BRAIDS_DOSC_PITCH_TABLE_START)
        midi_pitch = BRAIDS_DOSC_PITCH_TABLE_START - 1;

    int32_t ref_pitch = midi_pitch - BRAIDS_DOSC_PITCH_TABLE_START;
    size_t  num_shifts = 0;
    while (ref_pitch < 0) {
        ref_pitch += BRAIDS_DOSC_OCTAVE;
        ++num_shifts;
    }

    uint32_t a = lut_oscillator_increments[ref_pitch >> 4];
    uint32_t b = lut_oscillator_increments[(ref_pitch >> 4) + 1];
    uint32_t pi = a + ((int32_t)(b - a) * (ref_pitch & 0xF) >> 4);
    pi >>= num_shifts;
    return pi;
}

static uint32_t compute_delay(int16_t midi_pitch)
{
    if (midi_pitch >= BRAIDS_DOSC_HIGHEST_NOTE - BRAIDS_DOSC_OCTAVE)
        midi_pitch = BRAIDS_DOSC_HIGHEST_NOTE - BRAIDS_DOSC_OCTAVE;

    int32_t ref_pitch = midi_pitch - BRAIDS_DOSC_PITCH_TABLE_START;
    size_t  num_shifts = 0;
    while (ref_pitch < 0) {
        ref_pitch += BRAIDS_DOSC_OCTAVE;
        ++num_shifts;
    }

    uint32_t a = lut_oscillator_delays[ref_pitch >> 4];
    uint32_t b = lut_oscillator_delays[(ref_pitch >> 4) + 1];
    uint32_t delay = a + ((int32_t)(b - a) * (ref_pitch & 0xF) >> 4);
    delay >>= 12 - num_shifts;
    return delay;
}

/* ---- Formant tables (VOWEL_FOF) ----
 *
 * Copied verbatim from upstream digital_oscillator.cc lines 534-618.
 * These were file-static there too; keeping them file-static here
 * preserves the "no public API exposure" invariant.
 */

#define BRAIDS_NF BRAIDS_NUM_FORMANTS

static const int16_t formant_f_data[BRAIDS_NF][BRAIDS_NF][BRAIDS_NF] = {
    /* bass */
    {
        { 9519, 10738, 12448, 12636, 12892 },  /* a */
        { 8620, 11720, 12591, 12932, 13158 },  /* e */
        { 7579, 11891, 12768, 13122, 13323 },  /* i */
        { 8620, 10013, 12591, 12768, 13010 },  /* o */
        { 8324,  9519, 12591, 12831, 13048 },  /* u */
    },
    /* tenor */
    {
        { 9696, 10821, 12810, 13010, 13263 },
        { 8620, 11827, 12768, 13228, 13477 },
        { 7908, 12038, 12932, 13263, 13452 },
        { 8620, 10156, 12768, 12932, 13085 },
        { 8324,  9519, 12852, 13010, 13296 },
    },
    /* countertenor */
    {
        { 9730, 10902, 12892, 13085, 13330 },
        { 8832, 11953, 12852, 13085, 13296 },
        { 7749, 12014, 13010, 13330, 13483 },
        { 8781, 10211, 12852, 13085, 13296 },
        { 8448,  9627, 12892, 13085, 13363 },
    },
    /* alto */
    {
        { 10156, 10960, 12932, 13427, 14195 },
        {  8620, 11692, 12852, 13296, 14195 },
        {  8324, 11827, 12852, 13550, 14195 },
        {  8881, 10156, 12956, 13427, 14195 },
        {  8160,  9860, 12708, 13427, 14195 },
    },
    /* soprano */
    {
        { 10156, 10960, 13010, 13667, 14195 },
        {  8324, 12187, 12932, 13489, 14195 },
        {  7749, 12337, 13048, 13667, 14195 },
        {  8881, 10156, 12956, 13609, 14195 },
        {  8160,  9860, 12852, 13609, 14195 },
    },
};

static const int16_t formant_a_data[BRAIDS_NF][BRAIDS_NF][BRAIDS_NF] = {
    {
        { 16384, 7318, 5813, 5813, 1638 },
        { 16384, 4115, 5813, 4115, 2062 },
        { 16384,  518, 2596, 1301,  652 },
        { 16384, 4617, 1460, 1638,  163 },
        { 16384, 1638,  411,  652,  259 },
    },
    {
        { 16384, 8211, 7318, 6522, 1301 },
        { 16384, 3269, 4115, 3269, 1638 },
        { 16384, 2913, 2062, 1638,  518 },
        { 16384, 5181, 4115, 4115,  821 },
        { 16384, 1638, 2314, 3269,  821 },
    },
    {
        { 16384, 8211, 1159, 1033,  206 },
        { 16384, 3269, 2062, 1638, 1638 },
        { 16384, 1033, 1033,  259,  259 },
        { 16384, 5181,  821, 1301,  326 },
        { 16384, 1638, 1159,  518,  326 },
    },
    {
        { 16384, 10337, 1638, 259, 16 },
        { 16384,  1033,  518, 291, 16 },
        { 16384,  1638,  518, 259, 16 },
        { 16384,  5813, 2596, 652, 29 },
        { 16384,  4115,  518, 163, 10 },
    },
    {
        { 16384, 8211,  411, 1638, 51 },
        { 16384, 1638, 2913,  163, 25 },
        { 16384, 4115,  821,  821, 103 },
        { 16384, 4617, 1301, 1301,  51 },
        { 16384, 2596,  291,  163,  16 },
    },
};

static int16_t interpolate_formant_parameter(
    const int16_t table[BRAIDS_NF][BRAIDS_NF][BRAIDS_NF],
    int16_t x, int16_t y, uint8_t formant)
{
    uint16_t x_index = (uint16_t)x >> 13;
    uint16_t x_mix   = (uint16_t)x << 3;
    uint16_t y_index = (uint16_t)y >> 13;
    uint16_t y_mix   = (uint16_t)y << 3;
    int16_t a = table[x_index    ][y_index    ][formant];
    int16_t b = table[x_index + 1][y_index    ][formant];
    int16_t c = table[x_index    ][y_index + 1][formant];
    int16_t d = table[x_index + 1][y_index + 1][formant];
    a = (int16_t)(a + ((b - a) * x_mix >> 16));
    c = (int16_t)(c + ((d - c) * x_mix >> 16));
    return (int16_t)(a + ((c - a) * y_mix >> 16));
}

/* ---- Render: VOWEL_FOF ---- */

static void render_vowel_fof(braids_digital_osc_t *self,
                             const uint8_t *sync, int16_t *buffer, size_t size)
{
    (void)sync;  /* VOWEL_FOF ignores hard-sync — matches upstream */

    int16_t amplitudes[BRAIDS_NF];
    int32_t svf_lp[BRAIDS_NF];
    int32_t svf_bp[BRAIDS_NF];
    int16_t svf_f[BRAIDS_NF];

    for (size_t i = 0; i < BRAIDS_NF; ++i) {
        int32_t frequency = interpolate_formant_parameter(
            formant_f_data, self->parameter[1], self->parameter[0], (uint8_t)i)
            + (12 << 7);
        svf_f[i] = braids_interp824_u16(lut_svf_cutoff,
                                        (uint32_t)frequency << 17);
        amplitudes[i] = interpolate_formant_parameter(
            formant_a_data, self->parameter[1], self->parameter[0], (uint8_t)i);
        if (self->init) {
            svf_lp[i] = 0;
            svf_bp[i] = 0;
        } else {
            svf_lp[i] = self->state.fof.svf_lp[i];
            svf_bp[i] = self->state.fof.svf_bp[i];
        }
    }

    if (self->init)
        self->init = 0;

    uint32_t phase            = self->phase;
    int32_t  previous_sample  = self->state.fof.previous_sample;
    int32_t  next_saw_sample  = self->state.fof.next_saw_sample;
    uint32_t increment        = self->phase_increment << 1;

    while (size) {
        int32_t this_saw_sample = next_saw_sample;
        next_saw_sample = 0;
        phase += increment;
        if (phase < increment) {
            uint32_t t = phase / (increment >> 16);
            if (t > 65535) t = 65535;
            this_saw_sample -= (int32_t)((t * t) >> 18);
            t = 65535 - t;
            next_saw_sample -= -(int32_t)((t * t) >> 18);
        }
        next_saw_sample += phase >> 17;
        int32_t in  = this_saw_sample;
        int32_t out = 0;
        for (int32_t i = 0; i < 5; ++i) {
            int32_t notch = in - (svf_bp[i] >> 6);
            svf_lp[i] += svf_f[i] * svf_bp[i] >> 15;
            BRAIDS_CLIP(svf_lp[i]);
            int32_t hp = notch - svf_lp[i];
            svf_bp[i] += svf_f[i] * hp >> 15;
            BRAIDS_CLIP(svf_bp[i]);
            out += svf_bp[i] * amplitudes[0] >> 17;
        }
        BRAIDS_CLIP(out);
        *buffer++ = (int16_t)((out + previous_sample) >> 1);
        *buffer++ = (int16_t)out;
        previous_sample = out;
        size -= 2;
    }
    self->phase = phase;
    self->state.fof.next_saw_sample = next_saw_sample;
    self->state.fof.previous_sample = (int16_t)previous_sample;
    for (size_t i = 0; i < BRAIDS_NF; ++i) {
        self->state.fof.svf_lp[i] = svf_lp[i];
        self->state.fof.svf_bp[i] = svf_bp[i];
    }
}

/* ---- Render: TWIN_PEAKS_NOISE ----
 *
 * Two parallel resonant biquads driven by white noise. The Q factor
 * squares through `q_squared`; cutoff and scale come from the
 * resonator LUTs. Outputs are summed + makeup-gained + soft-clipped
 * through the moderate-overdrive waveshaper, then written twice per
 * loop iteration (2× downsampled inner loop, identical to upstream).
 */

static void render_twin_peaks_noise(braids_digital_osc_t *self,
                                    const uint8_t *sync,
                                    int16_t *buffer, size_t size)
{
    (void)sync;

    int32_t sample;
    int32_t y10, y20;
    int32_t y11 = self->state.pno.filter_state[0][0];
    int32_t y12 = self->state.pno.filter_state[0][1];
    int32_t y21 = self->state.pno.filter_state[1][0];
    int32_t y22 = self->state.pno.filter_state[1][1];

    uint32_t q         = 65240u + (uint32_t)(self->parameter[0] >> 7);
    int32_t  q_squared = (int32_t)((q * q) >> 17);

    int16_t p1 = self->pitch;
    BRAIDS_CONSTRAIN(p1, 0, 16383);
    int32_t c1 = braids_interp824_u16(lut_resonator_coefficient,
                                      (uint32_t)p1 << 17);
    int32_t s1 = braids_interp824_u16(lut_resonator_scale,
                                      (uint32_t)p1 << 17);

    int16_t p2 = (int16_t)(self->pitch + ((self->parameter[1] - 16384) >> 1));
    BRAIDS_CONSTRAIN(p2, 0, 16383);
    int32_t c2 = braids_interp824_u16(lut_resonator_coefficient,
                                      (uint32_t)p2 << 17);
    int32_t s2 = braids_interp824_u16(lut_resonator_scale,
                                      (uint32_t)p2 << 17);

    c1 = c1 * (int32_t)q >> 16;
    c2 = c2 * (int32_t)q >> 16;

    int32_t makeup_gain = 8191 - (self->parameter[0] >> 2);

    while (size) {
        sample = braids_random_sample() >> 1;

        if (sample > 0) {
            y10 =  (sample * s1 >> 16);
            y20 =  (sample * s2 >> 16);
        } else {
            y10 = -((-sample) * s1 >> 16);
            y20 = -((-sample) * s2 >> 16);
        }

        y10 += y11 * c1 >> 15;
        y10 -= y12 * q_squared >> 15;
        BRAIDS_CLIP(y10);
        y12 = y11;
        y11 = y10;

        y20 += y21 * c2 >> 15;
        y20 -= y22 * q_squared >> 15;
        BRAIDS_CLIP(y20);
        y22 = y21;
        y21 = y20;

        y10 += y20;
        y10 += (y10 * makeup_gain >> 13);
        BRAIDS_CLIP(y10);
        sample = y10;
        sample = braids_interp88_s16(ws_moderate_overdrive,
                                     (uint16_t)(sample + 32768));

        *buffer++ = (int16_t)sample;
        *buffer++ = (int16_t)sample;
        size -= 2;
    }

    self->state.pno.filter_state[0][0] = y11;
    self->state.pno.filter_state[0][1] = y12;
    self->state.pno.filter_state[1][0] = y21;
    self->state.pno.filter_state[1][1] = y22;
}

/* ---- Render: DIGITAL_MODULATION ----
 *
 * QPSK-like 4-symbol constellation sweeping through a preamble
 * pattern then a slow-varying "data" stream driven by parameter[1]
 * through a one-pole smoothing filter. Each emitted symbol is a
 * complex sinusoid (I + jQ) with amplitudes from the fixed
 * constellation table. Not a 2x-downsampled inner loop — this one
 * writes one sample per tick, so `size` may be odd.
 */

static const int32_t k_constellation_q[] = {  23100, -23100, -23100,  23100 };
static const int32_t k_constellation_i[] = {  23100,  23100, -23100, -23100 };

static void render_digital_modulation(braids_digital_osc_t *self,
                                      const uint8_t *sync,
                                      int16_t *buffer, size_t size)
{
    (void)sync;

    uint32_t phase                         = self->phase;
    uint32_t increment                     = self->phase_increment;
    uint32_t symbol_stream_phase           = self->state.dmd.symbol_phase;
    uint32_t symbol_stream_phase_increment =
        compute_phase_increment((int16_t)(self->pitch - 1536 +
                                          ((self->parameter[0] - 32767) >> 3)));
    uint8_t  data_byte                     = self->state.dmd.data_byte;

    if (self->strike) {
        self->state.dmd.symbol_count = 0;
        self->strike = 0;
    }

    while (size--) {
        phase += increment;
        symbol_stream_phase += symbol_stream_phase_increment;
        if (symbol_stream_phase < symbol_stream_phase_increment) {
            ++self->state.dmd.symbol_count;
            if (!(self->state.dmd.symbol_count & 3)) {
                if (self->state.dmd.symbol_count >= (64 + 4 * 256))
                    self->state.dmd.symbol_count = 0;
                if (self->state.dmd.symbol_count < 32) {
                    data_byte = 0x00;
                } else if (self->state.dmd.symbol_count < 48) {
                    data_byte = 0x99;
                } else if (self->state.dmd.symbol_count < 64) {
                    data_byte = 0xCC;
                } else {
                    self->state.dmd.filter_state =
                        (self->state.dmd.filter_state * 3 +
                         (int32_t)self->parameter[1]) >> 2;
                    data_byte = (uint8_t)(self->state.dmd.filter_state >> 7);
                }
            } else {
                data_byte >>= 2;
            }
        }
        int16_t ii = braids_interp824_s16(wav_sine, phase);
        int16_t qq = braids_interp824_s16(wav_sine, phase + (1u << 30));
        *buffer++ = (int16_t)(
            (k_constellation_q[data_byte & 3] * qq >> 15) +
            (k_constellation_i[data_byte & 3] * ii >> 15));
    }

    self->phase                  = phase;
    self->state.dmd.symbol_phase = symbol_stream_phase;
    self->state.dmd.data_byte    = data_byte;
}

/* ---- Render: PLUCKED ----
 *
 * Karplus-Strong with 3 polyphonic voices rotating on each strike.
 * Each voice owns its own 1025-sample `int16_t` delay line (one
 * segment of `delay_lines.ks`). The inner loop updates an adaptive
 * number of delay taps per sample — the number depends on the
 * desired pitch vs. the native delay-line stride — and advances the
 * read pointer through Interpolate1022.
 *
 * Upstream writes `previous_sample` into `plk[0]` unconditionally,
 * regardless of which voice is active. That quirk is preserved.
 *
 * Inner loop is 2×-downsampled (writes two samples per tick and
 * decrements `size -= 2`) — caller must pass an even size.
 */

static void render_plucked(braids_digital_osc_t *self,
                           const uint8_t *sync,
                           int16_t *buffer, size_t size)
{
    (void)sync;

    /* Upstream mutates `phase_increment_` here (doubles it). Replicate
     * by shifting the struct field in place — later dispatches will
     * recompute it from pitch. */
    self->phase_increment <<= 1;

    if (self->strike) {
        ++self->active_voice;
        if (self->active_voice >= BRAIDS_NUM_PLUCK_VOICES)
            self->active_voice = 0;

        braids_pluck_state *p = &self->state.plk[self->active_voice];
        int32_t increment = (int32_t)self->phase_increment;
        p->shift = 0;
        while (increment > (2 << 22)) {
            increment >>= 1;
            ++p->shift;
        }
        p->size = (size_t)(1024 >> p->shift);
        p->mask = p->size - 1;
        p->write_ptr = 0;
        p->max_phase_increment = self->phase_increment << 1;
        p->phase_increment = self->phase_increment;
        int32_t width = self->parameter[1];
        width = (3 * width) >> 1;
        p->initialization_ptr = (size_t)(((uint64_t)p->size * (uint64_t)(8192 + width)) >> 16);
        self->strike = 0;
    }

    braids_pluck_state *current_string = &self->state.plk[self->active_voice];

    /* Cap each active voice's running phase_increment to its original
     * value doubled, so pitch bends above "original * 2" are clamped. */
    current_string->phase_increment =
        self->phase_increment < current_string->max_phase_increment
            ? self->phase_increment
            : current_string->max_phase_increment;

    uint32_t update_probability = self->parameter[0] < 16384
        ? 65535u
        : (uint32_t)(131072 - (self->parameter[0] >> 3) * 31);
    int16_t loss = (int16_t)(4096 - (self->phase_increment >> 14));
    if (loss < 256) loss = 256;
    if (self->parameter[0] < 16384)
        loss = (int16_t)(loss * (16384 - self->parameter[0]) >> 14);
    else
        loss = 0;

    int16_t previous_sample = self->state.plk[0].previous_sample;

    while (size) {
        int32_t sample = 0;
        for (size_t i = 0; i < BRAIDS_NUM_PLUCK_VOICES; ++i) {
            braids_pluck_state *p = &self->state.plk[i];
            int16_t *dl = self->delay_lines.ks + i * 1025;

            if (p->initialization_ptr) {
                --p->initialization_ptr;
                int32_t excitation_sample = (dl[p->initialization_ptr] +
                                             3 * braids_random_sample()) >> 2;
                dl[p->initialization_ptr] = (int16_t)excitation_sample;
                sample += excitation_sample;
            } else {
                p->phase += p->phase_increment;
                size_t read_ptr  = ((p->phase >> (22 + p->shift)) + 2) & p->mask;
                size_t write_ptr = p->write_ptr;
                while (write_ptr != read_ptr) {
                    size_t next = (write_ptr + 1) & p->mask;
                    int32_t a = dl[write_ptr];
                    int32_t b = dl[next];
                    uint32_t probability = braids_random_word();
                    if ((probability & 0xFFFF) <= update_probability) {
                        int32_t sum = a + b;
                        sum = sum < 0 ? -(-sum >> 1) : (sum >> 1);
                        if (loss)
                            sum = sum * (32768 - loss) >> 15;
                        dl[write_ptr] = (int16_t)sum;
                    }
                    if (write_ptr == 0)
                        dl[p->size] = dl[0];
                    write_ptr = next;
                }
                p->write_ptr = write_ptr;
                sample += braids_interp1022(dl, p->phase >> p->shift);
            }
        }
        BRAIDS_CLIP(sample);
        *buffer++ = (int16_t)((previous_sample + sample) >> 1);
        *buffer++ = (int16_t)sample;
        previous_sample = (int16_t)sample;
        size -= 2;
    }
    self->state.plk[0].previous_sample = previous_sample;
}

/* ---- Render: BOWED (waveguide) ----
 *
 * Two delay lines (bridge + neck) form a closed string loop. A bow-
 * velocity envelope drives a friction curve (lut_bowing_friction)
 * which modulates energy injection each sample. Output taps the
 * bridge through a resonant biquad modeling the instrument body.
 *
 * Inner loop is 2× downsampled — caller must pass an even `size`.
 * Matches upstream digital_oscillator.cc::RenderBowed.
 */

#define BOWED_BRIDGE_LP_GAIN    14008
#define BOWED_BRIDGE_LP_POLE_1  18022
#define BOWED_BIQUAD_GAIN       6553
#define BOWED_BIQUAD_POLE_1     6948
#define BOWED_BIQUAD_POLE_2     (-2959)

static void render_bowed(braids_digital_osc_t *self,
                         const uint8_t *sync,
                         int16_t *buffer, size_t size)
{
    (void)sync;

    int8_t *dl_b = self->delay_lines.bowed.bridge;
    int8_t *dl_n = self->delay_lines.bowed.neck;

    if (self->strike) {
        memset(dl_b, 0, sizeof(self->delay_lines.bowed.bridge));
        memset(dl_n, 0, sizeof(self->delay_lines.bowed.neck));
        memset(&self->state, 0, sizeof(self->state));
        self->strike = 0;
    }

    int16_t parameter_0 = (int16_t)(172 - (self->parameter[0] >> 8));
    int16_t parameter_1 = (int16_t)(  6 + (self->parameter[1] >> 9));

    uint16_t delay_ptr     = self->state.phy.delay_ptr;
    uint16_t excitation_ptr = self->state.phy.excitation_ptr;
    int32_t  lp_state      = self->state.phy.lp_state;
    int32_t  biquad_y0     = self->state.phy.filter_state[0];
    int32_t  biquad_y1     = self->state.phy.filter_state[1];

    uint32_t delay        = (self->delay >> 1) - (2 << 16);  /* 1-pole delay comp */
    uint32_t bridge_delay = (delay >> 8) * parameter_1;
    while ((delay - bridge_delay) > ((BRAIDS_WG_NECK_LENGTH   - 1) << 16) ||
            bridge_delay             > ((BRAIDS_WG_BRIDGE_LENGTH - 1) << 16)) {
        delay        >>= 1;
        bridge_delay >>= 1;
    }
    uint16_t bridge_delay_integral   = bridge_delay >> 16;
    uint16_t bridge_delay_fractional = bridge_delay & 0xFFFF;
    uint32_t neck_delay              = delay - bridge_delay;
    uint32_t neck_delay_integral     = neck_delay >> 16;
    uint16_t neck_delay_fractional   = neck_delay & 0xFFFF;
    int16_t previous_sample          = self->state.phy.previous_sample;

    while (size) {
        self->phase += self->phase_increment;

        uint16_t bridge_delay_ptr = delay_ptr + 2 * BRAIDS_WG_BRIDGE_LENGTH
                                  - bridge_delay_integral;
        uint16_t neck_delay_ptr   = delay_ptr + 2 * BRAIDS_WG_NECK_LENGTH
                                  - neck_delay_integral;
        int16_t bridge_dl_a = dl_b[bridge_delay_ptr       % BRAIDS_WG_BRIDGE_LENGTH];
        int16_t bridge_dl_b = dl_b[(bridge_delay_ptr - 1) % BRAIDS_WG_BRIDGE_LENGTH];
        int16_t nut_dl_a    = dl_n[neck_delay_ptr         % BRAIDS_WG_NECK_LENGTH];
        int16_t nut_dl_b    = dl_n[(neck_delay_ptr - 1)   % BRAIDS_WG_NECK_LENGTH];

        int32_t bridge_value = braids_mix_s16(bridge_dl_a, bridge_dl_b,
                                              bridge_delay_fractional) << 8;
        int32_t nut_value    = braids_mix_s16(nut_dl_a, nut_dl_b,
                                              neck_delay_fractional) << 8;

        lp_state = (bridge_value * BOWED_BRIDGE_LP_GAIN +
                    lp_state    * BOWED_BRIDGE_LP_POLE_1) >> 15;
        int32_t bridge_reflection = -lp_state;
        int32_t nut_reflection    = -nut_value;
        int32_t string_velocity   = bridge_reflection + nut_reflection;

        int32_t bow_velocity = lut_bowing_envelope[ excitation_ptr      >> 1];
        bow_velocity        += lut_bowing_envelope[(excitation_ptr + 1) >> 1];
        bow_velocity       >>= 1;

        int32_t velocity_delta = bow_velocity - string_velocity;
        int32_t friction = velocity_delta * parameter_0 >> 5;
        if (friction < 0) friction = -friction;
        if (friction >= (1 << 17)) friction = (1 << 17) - 1;
        friction = lut_bowing_friction[friction >> 9];
        int32_t new_velocity = friction * velocity_delta >> 15;

        dl_n[delay_ptr % BRAIDS_WG_NECK_LENGTH]   = (int8_t)((bridge_reflection + new_velocity) >> 8);
        dl_b[delay_ptr % BRAIDS_WG_BRIDGE_LENGTH] = (int8_t)((nut_reflection    + new_velocity) >> 8);
        ++delay_ptr;

        int32_t temp = bridge_value * BOWED_BIQUAD_GAIN >> 15;
        temp += biquad_y0 * BOWED_BIQUAD_POLE_1 >> 12;
        temp += biquad_y1 * BOWED_BIQUAD_POLE_2 >> 12;
        int32_t out = temp - biquad_y1;
        biquad_y1 = biquad_y0;
        biquad_y0 = temp;

        BRAIDS_CLIP(out);
        *buffer++ = (int16_t)((out + previous_sample) >> 1);
        *buffer++ = (int16_t)out;
        previous_sample = (int16_t)out;
        ++excitation_ptr;
        size -= 2;
    }
    if ((excitation_ptr >> 1) >= LUT_BOWING_ENVELOPE_SIZE - 32)
        excitation_ptr = (LUT_BOWING_ENVELOPE_SIZE - 32) << 1;

    self->state.phy.delay_ptr      = (uint16_t)(delay_ptr % BRAIDS_WG_NECK_LENGTH);
    self->state.phy.excitation_ptr = excitation_ptr;
    self->state.phy.lp_state       = lp_state;
    self->state.phy.filter_state[0] = biquad_y0;
    self->state.phy.filter_state[1] = biquad_y1;
    self->state.phy.previous_sample = previous_sample;
}

/* ---- Render: BLOWN (waveguide reed model) ----
 *
 * Single int16 bore delay line, noise-driven breath pressure shaped
 * by a reed non-linearity. One output sample per tick (size may be
 * odd). Matches upstream RenderBlown. */

#define BLOWN_BREATH_PRESSURE          26214
#define BLOWN_REFLECTION_COEFF        (-3891)
#define BLOWN_REED_SLOPE              (-1229)
#define BLOWN_REED_OFFSET              22938

static void render_blown(braids_digital_osc_t *self,
                         const uint8_t *sync,
                         int16_t *buffer, size_t size)
{
    (void)sync;

    uint16_t delay_ptr = self->state.phy.delay_ptr;
    int32_t  lp_state  = self->state.phy.lp_state;

    int16_t *dl = self->delay_lines.bore;
    if (self->strike) {
        memset(dl, 0, sizeof(self->delay_lines.bore));
        self->strike = 0;
    }

    uint32_t delay = (self->delay >> 1) - (1 << 16);
    while (delay > ((BRAIDS_WG_BORE_LENGTH - 1) << 16))
        delay >>= 1;

    uint16_t bore_delay_integral   = delay >> 16;
    uint16_t bore_delay_fractional = delay & 0xFFFF;
    uint16_t parameter             = (uint16_t)(28000 - (self->parameter[0] >> 1));
    int16_t  filter_state          = (int16_t)self->state.phy.filter_state[0];

    int16_t normalized_pitch = (int16_t)((self->pitch - 8192 +
                                          (self->parameter[1] >> 1)) >> 7);
    if (normalized_pitch < 0) normalized_pitch = 0;
    else if (normalized_pitch > 127) normalized_pitch = 127;
    uint16_t filter_coefficient = lut_flute_body_filter[normalized_pitch];

    while (size--) {
        self->phase += self->phase_increment;

        int32_t breath_pressure = braids_random_sample() * parameter >> 15;
        breath_pressure = breath_pressure * BLOWN_BREATH_PRESSURE >> 15;
        breath_pressure += BLOWN_BREATH_PRESSURE;

        uint16_t bore_delay_ptr = delay_ptr + 2 * BRAIDS_WG_BORE_LENGTH
                                - bore_delay_integral;
        int16_t dl_a = dl[bore_delay_ptr       % BRAIDS_WG_BORE_LENGTH];
        int16_t dl_b = dl[(bore_delay_ptr - 1) % BRAIDS_WG_BORE_LENGTH];
        int32_t dl_value = braids_mix_s16(dl_a, dl_b, bore_delay_fractional);

        int32_t pressure_delta = (dl_value >> 1) + lp_state;
        lp_state = dl_value >> 1;

        pressure_delta = BLOWN_REFLECTION_COEFF * pressure_delta >> 12;
        pressure_delta -= breath_pressure;
        int32_t reed = (pressure_delta * BLOWN_REED_SLOPE >> 12) + BLOWN_REED_OFFSET;
        BRAIDS_CLIP(reed);
        int32_t out = pressure_delta * reed >> 15;
        out += breath_pressure;
        BRAIDS_CLIP(out);
        dl[delay_ptr++ % BRAIDS_WG_BORE_LENGTH] = (int16_t)out;

        filter_state = (int16_t)((filter_coefficient * out +
                                  (4096 - filter_coefficient) * filter_state) >> 12);
        *buffer++ = filter_state;
    }

    self->state.phy.filter_state[0] = filter_state;
    self->state.phy.delay_ptr       = (uint16_t)(delay_ptr % BRAIDS_WG_BORE_LENGTH);
    self->state.phy.lp_state        = lp_state;
}

/* ---- Render: FLUTED (jet + bore waveguide) ----
 *
 * Two int8 delay lines (jet + bore) with a blowing-envelope-driven
 * breath source and a `lut_blowing_jet` non-linearity. One sample
 * per tick. Matches upstream RenderFluted.
 */

#define FLUTED_DC_BLOCKING_POLE   4055  /* 0.99 * 4096, rounded */

static void render_fluted(braids_digital_osc_t *self,
                          const uint8_t *sync,
                          int16_t *buffer, size_t size)
{
    (void)sync;

    uint16_t delay_ptr       = self->state.phy.delay_ptr;
    uint16_t excitation_ptr  = self->state.phy.excitation_ptr;
    int32_t  lp_state        = self->state.phy.lp_state;
    int32_t  dc_blocking_x0  = self->state.phy.filter_state[0];
    int32_t  dc_blocking_y0  = self->state.phy.filter_state[1];

    int8_t *dl_b = self->delay_lines.fluted.bore;
    int8_t *dl_j = self->delay_lines.fluted.jet;

    if (self->strike) {
        excitation_ptr = 0;
        memset(dl_b, 0, sizeof(self->delay_lines.fluted.bore));
        memset(dl_j, 0, sizeof(self->delay_lines.fluted.jet));
        lp_state = 0;
        self->strike = 0;
    }

    uint32_t bore_delay = (self->delay << 1) - (2 << 16);
    uint32_t jet_delay  = (bore_delay >> 8) * (48 + (self->parameter[1] >> 10));
    bore_delay -= jet_delay;
    while (bore_delay > ((BRAIDS_WG_FBORE_LENGTH - 1) << 16) ||
           jet_delay  > ((BRAIDS_WG_JET_LENGTH   - 1) << 16)) {
        bore_delay >>= 1;
        jet_delay  >>= 1;
    }
    uint16_t bore_delay_integral   = bore_delay >> 16;
    uint16_t bore_delay_fractional = bore_delay & 0xFFFF;
    uint32_t jet_delay_integral    = jet_delay >> 16;
    uint16_t jet_delay_fractional  = jet_delay & 0xFFFF;

    uint16_t breath_intensity   = (uint16_t)(2100 - (self->parameter[0] >> 4));
    uint16_t filter_coefficient = lut_flute_body_filter[self->pitch >> 7];

    while (size--) {
        self->phase += self->phase_increment;

        uint16_t bore_delay_ptr = delay_ptr + 2 * BRAIDS_WG_FBORE_LENGTH
                                - bore_delay_integral;
        uint16_t jet_delay_ptr  = delay_ptr + 2 * BRAIDS_WG_JET_LENGTH
                                - jet_delay_integral;
        int16_t bore_dl_a = dl_b[bore_delay_ptr       % BRAIDS_WG_FBORE_LENGTH];
        int16_t bore_dl_b = dl_b[(bore_delay_ptr - 1) % BRAIDS_WG_FBORE_LENGTH];
        int16_t jet_dl_a  = dl_j[jet_delay_ptr        % BRAIDS_WG_JET_LENGTH];
        int16_t jet_dl_b  = dl_j[(jet_delay_ptr - 1)  % BRAIDS_WG_JET_LENGTH];
        int32_t bore_value = braids_mix_s16(bore_dl_a, bore_dl_b, bore_delay_fractional) << 9;
        int32_t jet_value  = braids_mix_s16(jet_dl_a,  jet_dl_b,  jet_delay_fractional ) << 9;

        int32_t breath_pressure = lut_blowing_envelope[excitation_ptr];
        breath_pressure <<= 1;
        int32_t random_pressure = braids_random_sample() * breath_intensity >> 12;
        random_pressure = random_pressure * breath_pressure >> 15;
        breath_pressure += random_pressure;

        lp_state = (-(int32_t)filter_coefficient * bore_value +
                    ((int32_t)(4096 - filter_coefficient)) * lp_state) >> 12;
        int32_t reflection = lp_state;
        dc_blocking_y0 = (FLUTED_DC_BLOCKING_POLE * dc_blocking_y0 >> 12);
        dc_blocking_y0 += reflection - dc_blocking_x0;
        dc_blocking_x0 = reflection;
        reflection = dc_blocking_y0;

        int32_t pressure_delta = breath_pressure - (reflection >> 1);
        dl_j[delay_ptr % BRAIDS_WG_JET_LENGTH] = (int8_t)(pressure_delta >> 9);

        int32_t jet_table_index = jet_value;
        if (jet_table_index < 0)      jet_table_index = 0;
        if (jet_table_index > 65535)  jet_table_index = 65535;
        pressure_delta = (int16_t)lut_blowing_jet[jet_table_index >> 8]
                       + (reflection >> 1);
        dl_b[delay_ptr % BRAIDS_WG_FBORE_LENGTH] = (int8_t)(pressure_delta >> 9);
        ++delay_ptr;

        int32_t out = bore_value >> 1;
        BRAIDS_CLIP(out);
        *buffer++ = (int16_t)out;
        if (size & 3) ++excitation_ptr;
    }
    if (excitation_ptr >= LUT_BLOWING_ENVELOPE_SIZE - 32)
        excitation_ptr = LUT_BLOWING_ENVELOPE_SIZE - 32;

    self->state.phy.delay_ptr      = delay_ptr;
    self->state.phy.excitation_ptr = excitation_ptr;
    self->state.phy.lp_state       = lp_state;
    self->state.phy.filter_state[0] = dc_blocking_x0;
    self->state.phy.filter_state[1] = dc_blocking_y0;
}

/* ---- Render: TRIPLE_RING_MOD ----
 *
 * Carrier sine × two independent modulator sines. Matches upstream
 * RenderTripleRingMod. Upstream stored the two modulator phases in
 * the vowel-synth state union arm; we use a dedicated `trm` arm.
 */

static void render_triple_ring_mod(braids_digital_osc_t *self,
                                   const uint8_t *sync,
                                   int16_t *buffer, size_t size)
{
    uint32_t phase                       = self->phase + (1u << 30);
    uint32_t increment                   = self->phase_increment;
    uint32_t modulator_phase              = self->state.trm.modulator_phase[0];
    uint32_t modulator_phase_2            = self->state.trm.modulator_phase[1];
    uint32_t modulator_phase_increment    = compute_phase_increment(
        (int16_t)(self->pitch + ((self->parameter[0] - 16384) >> 2)));
    uint32_t modulator_phase_increment_2 = compute_phase_increment(
        (int16_t)(self->pitch + ((self->parameter[1] - 16384) >> 2)));

    while (size--) {
        phase += increment;
        if (*sync++) {
            phase = 0;
            modulator_phase = 0;
            modulator_phase_2 = 0;
        }
        modulator_phase   += modulator_phase_increment;
        modulator_phase_2 += modulator_phase_increment_2;
        int16_t result = braids_interp824_s16(wav_sine, phase);
        result = (int16_t)((result * braids_interp824_s16(wav_sine, modulator_phase)) >> 16);
        result = (int16_t)((result * braids_interp824_s16(wav_sine, modulator_phase_2)) >> 16);
        result = braids_interp88_s16(ws_moderate_overdrive,
                                     (uint16_t)(result + 32768));
        *buffer++ = result;
    }
    self->phase = phase - (1u << 30);
    self->state.trm.modulator_phase[0] = modulator_phase;
    self->state.trm.modulator_phase[1] = modulator_phase_2;
}

/* ---- Render: SAW_SWARM ----
 *
 * Six detuned saw phasors summed and filtered. `parameter[0]` = total
 * detune spread, `parameter[1]` = HP cutoff offset around pitch. The
 * seventh oscillator uses `self->phase` directly — matches upstream.
 */

static void render_saw_swarm(braids_digital_osc_t *self,
                             const uint8_t *sync,
                             int16_t *buffer, size_t size)
{
    int32_t detune = self->parameter[0] + 1024;
    detune = (detune * detune) >> 9;

    uint32_t increments[7];
    for (int16_t i = 0; i < 7; ++i) {
        int32_t saw_detune         = detune * (i - 3);
        int32_t detune_integral    = saw_detune >> 16;
        int32_t detune_fractional  = saw_detune & 0xFFFF;
        int32_t increment_a = (int32_t)compute_phase_increment(
            (int16_t)(self->pitch + detune_integral));
        int32_t increment_b = (int32_t)compute_phase_increment(
            (int16_t)(self->pitch + detune_integral + 1));
        increments[i] = (uint32_t)(increment_a +
            (((increment_b - increment_a) * detune_fractional) >> 16));
    }

    if (self->strike) {
        for (size_t i = 0; i < 6; ++i)
            self->state.saw.phase[i] = braids_random_word();
        self->strike = 0;
    }

    int32_t hp_cutoff = self->pitch;
    if (self->parameter[1] < 10922)
        hp_cutoff += ((self->parameter[1] - 10922) * 24) >> 5;
    else
        hp_cutoff += ((self->parameter[1] - 10922) * 12) >> 5;
    if (hp_cutoff < 0)         hp_cutoff = 0;
    else if (hp_cutoff > 32767) hp_cutoff = 32767;

    int32_t f    = braids_interp824_u16(lut_svf_cutoff,
                                        (uint32_t)hp_cutoff << 17);
    int32_t damp = lut_svf_damp[0];
    int32_t bp   = self->state.saw.bp;
    int32_t lp   = self->state.saw.lp;

    while (size--) {
        if (*sync++) {
            for (size_t i = 0; i < 6; ++i) self->state.saw.phase[i] = 0;
        }
        self->phase             += increments[0];
        self->state.saw.phase[0] += increments[1];
        self->state.saw.phase[1] += increments[2];
        self->state.saw.phase[2] += increments[3];
        self->state.saw.phase[3] += increments[4];
        self->state.saw.phase[4] += increments[5];
        self->state.saw.phase[5] += increments[6];

        int32_t sample = -28672;
        sample += self->phase             >> 19;
        sample += self->state.saw.phase[0] >> 19;
        sample += self->state.saw.phase[1] >> 19;
        sample += self->state.saw.phase[2] >> 19;
        sample += self->state.saw.phase[3] >> 19;
        sample += self->state.saw.phase[4] >> 19;
        sample += self->state.saw.phase[5] >> 19;
        sample = braids_interp88_s16(ws_moderate_overdrive,
                                     (uint16_t)(sample + 32768));

        int32_t notch = sample - (bp * damp >> 15);
        lp += f * bp >> 15;
        BRAIDS_CLIP(lp);
        int32_t hp = notch - lp;
        bp += f * hp >> 15;

        int32_t result = hp;
        BRAIDS_CLIP(result);
        *buffer++ = (int16_t)result;
    }
    self->state.saw.lp = lp;
    self->state.saw.bp = bp;
}

/* ---- Render: COMB_FILTER ----
 *
 * A resonant comb delay applied to the *incoming* buffer — caller
 * must fill buffer with dry signal before calling. `parameter[0]`
 * tunes the delay length relative to pitch; `parameter[1]` controls
 * feedback / resonance.
 *
 * Upstream stashes the previous filtered-pitch in state.ffm; we keep
 * it in our dedicated `cmb` arm for layout clarity.
 */

static void render_comb_filter(braids_digital_osc_t *self,
                               const uint8_t *sync,
                               int16_t *buffer, size_t size)
{
    (void)sync;

    int32_t pitch           = self->pitch + ((self->parameter[0] - 16384) >> 1);
    int32_t filtered_pitch  = self->state.cmb.filtered_pitch;
    filtered_pitch = (15 * filtered_pitch + pitch) >> 4;
    self->state.cmb.filtered_pitch = (int16_t)filtered_pitch;

    int16_t *dl = self->delay_lines.comb;
    uint32_t delay = compute_delay((int16_t)filtered_pitch);
    if (delay > ((uint32_t)BRAIDS_COMB_DELAY_LENGTH << 16))
        delay = (uint32_t)BRAIDS_COMB_DELAY_LENGTH << 16;

    uint32_t delay_integral   = delay >> 16;
    int32_t  delay_fractional = delay & 0xFFFF;

    int16_t resonance = (int16_t)((self->parameter[1] << 1) - 32768);
    resonance = braids_interp88_s16(ws_moderate_overdrive,
                                    (uint16_t)(resonance + 32768));

    uint32_t delay_ptr = self->phase % BRAIDS_COMB_DELAY_LENGTH;

    while (size--) {
        int32_t in = *buffer;
        uint32_t offset = delay_ptr + 2 * BRAIDS_COMB_DELAY_LENGTH - delay_integral;
        int32_t a = dl[offset       % BRAIDS_COMB_DELAY_LENGTH];
        int32_t b = dl[(offset - 1) % BRAIDS_COMB_DELAY_LENGTH];
        int32_t delayed_sample = a + (((b - a) * (delay_fractional >> 1)) >> 15);
        int32_t feedback = (delayed_sample * resonance >> 15) + (in >> 1);
        BRAIDS_CLIP(feedback);
        dl[delay_ptr] = (int16_t)feedback;
        int32_t out = (in + (delayed_sample << 1)) >> 1;
        BRAIDS_CLIP(out);
        *buffer++ = (int16_t)out;
        delay_ptr = (delay_ptr + 1) % BRAIDS_COMB_DELAY_LENGTH;
    }
    self->phase = delay_ptr;
}

/* ---- Dispatch ----
 *
 * Slots for not-yet-ported shapes remain NULL; the dispatcher guards
 * against that case so a call at this stage is a diagnosable no-op
 * rather than a crash.
 */

typedef void (*render_fn)(braids_digital_osc_t *,
                          const uint8_t *, int16_t *, size_t);

static const render_fn fn_table[BRAIDS_DOSC_SHAPE_COUNT] = {
    [BRAIDS_DOSC_SHAPE_VOWEL_FOF]          = render_vowel_fof,
    [BRAIDS_DOSC_SHAPE_TWIN_PEAKS_NOISE]   = render_twin_peaks_noise,
    [BRAIDS_DOSC_SHAPE_DIGITAL_MODULATION] = render_digital_modulation,
    [BRAIDS_DOSC_SHAPE_PLUCKED]            = render_plucked,
    [BRAIDS_DOSC_SHAPE_BOWED]              = render_bowed,
    [BRAIDS_DOSC_SHAPE_BLOWN]              = render_blown,
    [BRAIDS_DOSC_SHAPE_FLUTED]             = render_fluted,
    [BRAIDS_DOSC_SHAPE_TRIPLE_RING_MOD]    = render_triple_ring_mod,
    [BRAIDS_DOSC_SHAPE_SAW_SWARM]          = render_saw_swarm,
    [BRAIDS_DOSC_SHAPE_COMB_FILTER]        = render_comb_filter,
};

void braids_digital_osc_render(braids_digital_osc_t *self,
                               const uint8_t *sync,
                               int16_t *buffer,
                               size_t size)
{
    render_fn fn = (self->shape < BRAIDS_DOSC_SHAPE_COUNT)
        ? fn_table[self->shape] : NULL;
    if (!fn) {
        /* Not-yet-ported shape — emit silence so the caller's buffer
         * is well-defined. */
        memset(buffer, 0, size * sizeof(int16_t));
        return;
    }

    if (self->shape != self->previous_shape) {
        /* State reset only — preserve caller's current `shape`. */
        digital_osc_reset_state(self);
        self->previous_shape = self->shape;
        self->init = 1;
    }

    self->phase_increment = compute_phase_increment(self->pitch);
    self->delay           = compute_delay(self->pitch);

    if (self->pitch > BRAIDS_DOSC_HIGHEST_NOTE) self->pitch = BRAIDS_DOSC_HIGHEST_NOTE;
    else if (self->pitch < 0)                    self->pitch = 0;

    fn(self, sync, buffer, size);
}
