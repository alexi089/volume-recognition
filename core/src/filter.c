#include "filter.h"

#include <math.h>

#define TWO_PI 6.28318530717958647692f

/*
 * Smoothing factor for a first-order low-pass filter with the given cutoff
 * frequency, sampled every dt seconds. The result is in (0, 1]:
 * near 0 = "mostly keep the old value", 1 = "take the new value as is".
 *
 * Deriving alpha from dt every call, rather than hard-coding one alpha, is
 * what keeps the filter's behaviour the same whether frames arrive at 30 fps,
 * 24 fps, or unevenly.
 */
static float smoothing_factor(float cutoff_hz, float dt)
{
    float tau = 1.0f / (TWO_PI * cutoff_hz);
    return 1.0f / (1.0f + tau / dt);
}

static float low_pass(float x, float prev, float alpha)
{
    return alpha * x + (1.0f - alpha) * prev;
}

void gv_filter_init(gv_filter *f, float min_cutoff, float beta, float d_cutoff)
{
    f->min_cutoff = min_cutoff;
    f->beta = beta;
    f->d_cutoff = d_cutoff;
    gv_filter_reset(f);
}

void gv_filter_reset(gv_filter *f)
{
    f->has_prev = 0;
    f->x_prev = 0.0f;
    f->raw_prev = 0.0f;
    f->dx_prev = 0.0f;
    f->t_prev = 0.0;
}

float gv_filter_apply(gv_filter *f, float x, double t_sec)
{
    /* NaN or infinity in any of the arithmetic below gives NaN out, and the
     * output is stored as the next step's input, so one bad sample would
     * poison every later result. Drop it and keep the state untouched. */
    if (!isfinite(x))
        return f->has_prev ? f->x_prev : x;

    if (!f->has_prev) {
        f->has_prev = 1;
        f->x_prev = x;
        f->raw_prev = x;
        f->dx_prev = 0.0f;
        f->t_prev = t_sec;
        return x;
    }

    /* The subtraction is done in double and only then narrowed: timestamps
     * grow without bound, and a float runs out of precision for millisecond
     * differences after a few hours of uptime. */
    float dt = (float)(t_sec - f->t_prev);
    if (dt <= 0.0f)
        return f->x_prev;

    /* 1. Estimate speed from the raw samples, as the paper does, and smooth
     *    it: raw frame-to-frame speed is the noisiest quantity here, since it
     *    divides jitter by a tiny dt. */
    float dx = (x - f->raw_prev) / dt;
    float dx_hat = low_pass(dx, f->dx_prev, smoothing_factor(f->d_cutoff, dt));

    /* 2. Faster movement -> higher cutoff -> less smoothing, less lag. */
    float cutoff = f->min_cutoff + f->beta * fabsf(dx_hat);

    /* 3. Smooth the value itself with that cutoff. */
    float x_hat = low_pass(x, f->x_prev, smoothing_factor(cutoff, dt));

    f->x_prev = x_hat;
    f->raw_prev = x;
    f->dx_prev = dx_hat;
    f->t_prev = t_sec;
    return x_hat;
}
