/*
 * biquad.c — Direct-form-I biquad implementation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * State precision is Q29 throughout — input, output, and the history
 * taps (x1, x2, y1, y2). This is crucial for narrow-band filters: the
 * feedback term a1*y[n-1] needs access to sub-Q15 precision of y, or
 * the truncation error compounds each recursion and the filter drifts
 * far from its intended response. LR4 at 88 Hz / 32 kHz (pole magnitude
 * ~0.988) would produce 3-4x systematic error in stopband magnitude
 * without this precision.
 *
 * Coefficients are Q29 too. Q29*Q29 = Q58, fits int64; sum of 5 Q58
 * terms fits int64 with headroom for the |coef*state| ≤ 2 typical case.
 */

#include "biquad.h"
#include "platform.h"

#include <math.h>

#define BQ_Q 29
#define BQ_SCALE ((double)(1 << BQ_Q))

/* Quantization: Q15 integer sample widened to Q29 = shift left by 14. */
#define BQ_Q15_TO_Q29 14

static int32_t d_to_qcoef(double v)
{
    double scaled = v * BQ_SCALE;
    if (scaled >  (double)INT32_MAX) return INT32_MAX;
    if (scaled < -(double)INT32_MAX) return INT32_MIN;
    return (int32_t)scaled;
}

/* Shape IDs for the internal init dispatcher. */
typedef enum {
    BQ_SHAPE_LP = 0,
    BQ_SHAPE_HP,
    BQ_SHAPE_BP,
    BQ_SHAPE_NOTCH,
} bq_shape;

static void biquad_compute_w0(uint32_t cutoff_hz, uint32_t sample_rate,
                              double *out_cos, double *out_sin)
{
    uint32_t nyquist = sample_rate / 2u;
    if (cutoff_hz >= nyquist) cutoff_hz = nyquist - 1u;
    if (cutoff_hz == 0)       cutoff_hz = 1u;

    double w0 = 2.0 * 3.14159265358979323846 *
                (double)cutoff_hz / (double)sample_rate;
    *out_cos = cos(w0);
    *out_sin = sin(w0);
}

static void biquad_set_with_q(lfe_biquad_state *bq,
                              uint32_t cutoff_hz,
                              uint32_t sample_rate,
                              double q,
                              bq_shape shape)
{
    if (!bq) return;
    if (sample_rate == 0) return;
    if (q < 0.5) q = 0.5;   /* below 0.5 gets sluggish without useful effect */

    double cos_w0, sin_w0;
    biquad_compute_w0(cutoff_hz, sample_rate, &cos_w0, &sin_w0);
    double alpha = sin_w0 / (2.0 * q);

    double a0_unnorm = 1.0 + alpha;
    double a1_unnorm = -2.0 * cos_w0;
    double a2_unnorm = 1.0 - alpha;

    double inv_a0 = 1.0 / a0_unnorm;
    bq->a1 = d_to_qcoef(a1_unnorm * inv_a0);
    bq->a2 = d_to_qcoef(a2_unnorm * inv_a0);

    /*
     * For LP and HP we derive the b coefs from the quantized a's so
     * DC (LP) or Nyquist (HP) gain is exact in Q29 integer arithmetic.
     * At low cutoffs with independent quantization, (b0+b1+b2) and
     * (1+a1+a2) are both ~5e-4 and their ratio gets several-percent
     * error — which compounds badly in cascaded stages (LR4). For BP
     * and Notch we use the standard RBJ formulas directly since their
     * natural scale doesn't suffer the same cancellation issue.
     */
    int64_t one_q29 = (int64_t)1 << BQ_Q;
    switch (shape) {
    case BQ_SHAPE_LP: {
        int64_t sum  = one_q29 + bq->a1 + bq->a2;
        int32_t b0_q = (int32_t)(sum / 4);
        bq->b0 =     b0_q;
        bq->b1 = 2 * b0_q;
        bq->b2 =     b0_q;
        break;
    }
    case BQ_SHAPE_HP: {
        int64_t diff = one_q29 - bq->a1 + bq->a2;
        int32_t b0_q = (int32_t)(diff / 4);
        bq->b0 =  b0_q;
        bq->b1 = -2 * b0_q;
        bq->b2 =  b0_q;
        break;
    }
    case BQ_SHAPE_BP: {
        /* Constant 0-dB peak gain: b0 = alpha/a0, b1 = 0, b2 = -b0. */
        double b0_norm = alpha * inv_a0;
        bq->b0 =  d_to_qcoef(b0_norm);
        bq->b1 =  0;
        bq->b2 = -bq->b0;
        break;
    }
    case BQ_SHAPE_NOTCH: {
        /* b0 = 1/a0, b1 = -2cos(w0)/a0, b2 = 1/a0. */
        double b0_norm = inv_a0;
        bq->b0 = d_to_qcoef(b0_norm);
        bq->b1 = d_to_qcoef(-2.0 * cos_w0 * inv_a0);
        bq->b2 = bq->b0;
        break;
    }
    }

    bq->x1 = bq->x2 = 0;
    bq->y1 = bq->y2 = 0;
}

void lfe_biquad_init_lp_butter(lfe_biquad_state *bq,
                               uint32_t cutoff_hz,
                               uint32_t sample_rate)
{
    biquad_set_with_q(bq, cutoff_hz, sample_rate,
                      0.70710678118654752440, BQ_SHAPE_LP);
}

void lfe_biquad_init_hp_butter(lfe_biquad_state *bq,
                               uint32_t cutoff_hz,
                               uint32_t sample_rate)
{
    biquad_set_with_q(bq, cutoff_hz, sample_rate,
                      0.70710678118654752440, BQ_SHAPE_HP);
}

void lfe_biquad_init_lp(lfe_biquad_state *bq,
                        uint32_t cutoff_hz, uint32_t sample_rate, double q)
{
    biquad_set_with_q(bq, cutoff_hz, sample_rate, q, BQ_SHAPE_LP);
}

void lfe_biquad_init_hp(lfe_biquad_state *bq,
                        uint32_t cutoff_hz, uint32_t sample_rate, double q)
{
    biquad_set_with_q(bq, cutoff_hz, sample_rate, q, BQ_SHAPE_HP);
}

void lfe_biquad_init_bp(lfe_biquad_state *bq,
                        uint32_t cutoff_hz, uint32_t sample_rate, double q)
{
    biquad_set_with_q(bq, cutoff_hz, sample_rate, q, BQ_SHAPE_BP);
}

void lfe_biquad_init_notch(lfe_biquad_state *bq,
                           uint32_t cutoff_hz, uint32_t sample_rate, double q)
{
    biquad_set_with_q(bq, cutoff_hz, sample_rate, q, BQ_SHAPE_NOTCH);
}

void lfe_biquad_reset(lfe_biquad_state *bq)
{
    if (!bq) return;
    bq->x1 = bq->x2 = 0;
    bq->y1 = bq->y2 = 0;
}

LFE_HOT
q15_t lfe_biquad_step(lfe_biquad_state *bq, q15_t in)
{
    /* Widen the Q15 input to Q29 so state arithmetic is coherent. */
    int32_t in_q29 = (int32_t)in << BQ_Q15_TO_Q29;

    /* Q29 coef * Q29 state = Q58. Sum of 5 fits int64 when
     * |coef|*|state| ≤ 2 (true for Butterworth in steady state). */
    int64_t acc = (int64_t)bq->b0 * in_q29
                + (int64_t)bq->b1 * bq->x1
                + (int64_t)bq->b2 * bq->x2
                - (int64_t)bq->a1 * bq->y1
                - (int64_t)bq->a2 * bq->y2;

    int32_t y_q29 = (int32_t)(acc >> BQ_Q);  /* Q58 >> 29 = Q29 */

    bq->x2 = bq->x1;
    bq->x1 = in_q29;
    bq->y2 = bq->y1;
    bq->y1 = y_q29;

    /* Convert Q29 → Q15 for caller. */
    int32_t y_q15 = y_q29 >> BQ_Q15_TO_Q29;
    return lfe_sat_q15(y_q15);
}
