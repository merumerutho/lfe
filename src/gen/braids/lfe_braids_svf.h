/*
 * lfe_braids_svf.h — state-variable filter for modeling T-networks.
 *
 * Ported from braids/svf.h (MIT, (c) 2013 Emilie Gillet). Chamberlin
 * SVF driven by two LUT-interpolated coefficients (cutoff + damp). A
 * "punch" parameter adds a touch of saturation feedback from the LP
 * state — used by the struck-voice shapes to give the body of the
 * filter a percussive bite.
 *
 * Header-only: Process() sits in the per-sample inner loop of the
 * physical-model voices.
 *
 * ARM946E-S note: `lp_ += f * bp >> 15` and friends use int32 state,
 * not int64. Maxtracker's own SVF hit transient overshoot bugs with
 * int32 state (see feedback_svf_int64_state memory) — upstream Braids
 * mitigates with the CLIP after each accumulation, which is kept
 * verbatim here. If the filter ever blows up on percussive triggers
 * we should revisit widening to int64 to match maxtracker's convention.
 */

#ifndef LFE_BRAIDS_SVF_H
#define LFE_BRAIDS_SVF_H

#include <stdint.h>
#include "lfe_braids_dsp.h"
#include "lfe_braids_resources.h"

typedef enum {
    BRAIDS_SVF_MODE_LP = 0,
    BRAIDS_SVF_MODE_BP,
    BRAIDS_SVF_MODE_HP
} braids_svf_mode;

typedef struct {
    int dirty;           /* cutoff / resonance need re-interp */
    int16_t  frequency;  /* cutoff index into lut_svf_cutoff */
    int16_t  resonance;  /* damp index into lut_svf_damp     */
    int32_t  punch;      /* Q(~16) saturation feedback scale */
    int32_t  f;          /* cached LUT output for `frequency` */
    int32_t  damp;       /* cached LUT output for `resonance` */
    int32_t  lp;         /* low-pass state */
    int32_t  bp;         /* band-pass state */
    braids_svf_mode mode;
} braids_svf_t;

static inline void braids_svf_init(braids_svf_t *s)
{
    s->dirty     = 1;
    s->frequency = 33 << 7;
    s->resonance = 16384;
    s->punch     = 0;
    s->f         = 0;
    s->damp      = 0;
    s->lp        = 0;
    s->bp        = 0;
    s->mode      = BRAIDS_SVF_MODE_BP;
}

static inline void braids_svf_set_frequency(braids_svf_t *s, int16_t f)
{
    s->dirty |= (s->frequency != f);
    s->frequency = f;
}

static inline void braids_svf_set_resonance(braids_svf_t *s, int16_t r)
{
    s->resonance = r;
    s->dirty = 1;
}

static inline void braids_svf_set_punch(braids_svf_t *s, uint16_t punch)
{
    s->punch = (int32_t)(((uint32_t)punch * (uint32_t)punch) >> 24);
}

static inline void braids_svf_set_mode(braids_svf_t *s, braids_svf_mode m)
{
    s->mode = m;
}

static inline int32_t braids_svf_process(braids_svf_t *s, int32_t in)
{
    if (s->dirty) {
        s->f    = braids_interp824_u16(lut_svf_cutoff,
                                       (uint32_t)s->frequency << 17);
        s->damp = braids_interp824_u16(lut_svf_damp,
                                       (uint32_t)s->resonance << 17);
        s->dirty = 0;
    }
    int32_t f    = s->f;
    int32_t damp = s->damp;
    if (s->punch) {
        int32_t punch_signal = s->lp > 4096 ? s->lp : 2048;
        f    += ((punch_signal >> 4) * s->punch) >> 9;
        damp += ((punch_signal - 2048) >> 3);
    }
    int32_t notch = in - (s->bp * damp >> 15);
    s->lp += f * s->bp >> 15;
    BRAIDS_CLIP(s->lp);
    int32_t hp = notch - s->lp;
    s->bp += f * hp >> 15;
    BRAIDS_CLIP(s->bp);
    return s->mode == BRAIDS_SVF_MODE_BP ? s->bp
         : s->mode == BRAIDS_SVF_MODE_HP ? hp
                                         : s->lp;
}

#endif /* LFE_BRAIDS_SVF_H */
