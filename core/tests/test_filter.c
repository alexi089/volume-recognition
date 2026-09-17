#include "test.h"

#include "filter.h"

#define FPS 30.0
#define DT  (1.0 / FPS)

/* Landmark x is in normalised image units (0..1 across the frame), so a brisk
 * slide is about 1 unit/s. beta = 4 opens the cutoff from 1 Hz at rest to 5 Hz
 * at that speed, for a steady-state lag of 1 / (2*pi*5) = 0.03 units. */
#define BETA 4.0f

static gv_filter make_filter(float beta)
{
    gv_filter f;
    gv_filter_init(&f, 1.0f, beta, 1.0f);
    return f;
}

static void first_sample_passes_through(void)
{
    gv_filter f = make_filter(BETA);
    CHECK_NEAR(gv_filter_apply(&f, 0.42f, 0.0), 0.42f, 1e-6);
}

static void constant_input_stays_constant(void)
{
    gv_filter f = make_filter(BETA);
    float y = 0.0f;
    for (int i = 0; i < 60; i++)
        y = gv_filter_apply(&f, 0.3f, i * DT);
    CHECK_NEAR(y, 0.3f, 1e-5);
}

/* A still hand: 0.5 with +/-0.01 of frame-to-frame jitter. */
static void jitter_on_a_still_hand_is_attenuated(void)
{
    gv_filter f = make_filter(BETA);
    float lo = 1.0f, hi = 0.0f;
    for (int i = 0; i < 120; i++) {
        float x = 0.5f + ((i % 2) ? 0.01f : -0.01f);
        float y = gv_filter_apply(&f, x, i * DT);
        if (i >= 30) { /* after settling */
            if (y < lo) lo = y;
            if (y > hi) hi = y;
        }
    }
    float input_swing = 0.02f;
    CHECK(hi - lo < 0.25f * input_swing);
    CHECK_NEAR((hi + lo) / 2.0f, 0.5f, 0.002);
}

/* Lag behind a hand sweeping at 1 unit/s, measured after one second. */
static float lag_on_ramp(float beta)
{
    gv_filter f = make_filter(beta);
    float y = 0.0f, x = 0.0f;
    for (int i = 0; i <= 30; i++) {
        x = (float)(i * DT);
        y = gv_filter_apply(&f, x, i * DT);
    }
    return x - y;
}

static void fast_movement_is_followed_closely(void)
{
    CHECK(lag_on_ramp(BETA) < 0.06f);
}

/* This is the point of One Euro over a plain low-pass: same smoothing at
 * rest, less lag in motion. With beta = 0 the filter is a plain low-pass. */
static void speed_adaptation_reduces_lag(void)
{
    float plain = lag_on_ramp(0.0f);
    float adaptive = lag_on_ramp(BETA);
    CHECK(plain > 0.10f);
    CHECK(adaptive < 0.5f * plain);
}

static void repeated_timestamp_returns_previous_output(void)
{
    gv_filter f = make_filter(BETA);
    gv_filter_apply(&f, 0.2f, 0.0);
    float y1 = gv_filter_apply(&f, 0.3f, DT);
    float y2 = gv_filter_apply(&f, 0.9f, DT);        /* same timestamp */
    float y3 = gv_filter_apply(&f, 0.9f, DT - 0.01); /* going backwards */
    CHECK(y1 > 0.2f && y1 < 0.3f); /* a real, partly smoothed value */
    CHECK(isfinite(y2) && isfinite(y3));
    CHECK_NEAR(y2, y1, 1e-6);
    CHECK_NEAR(y3, y1, 1e-6);
}

static void reset_forgets_history(void)
{
    gv_filter f = make_filter(BETA);
    for (int i = 0; i < 10; i++)
        gv_filter_apply(&f, 0.1f, i * DT);
    gv_filter_reset(&f);
    CHECK_NEAR(gv_filter_apply(&f, 0.8f, 10 * DT), 0.8f, 1e-6);
}

int main(void)
{
    fprintf(stderr, "test_filter\n");
    RUN(first_sample_passes_through);
    RUN(constant_input_stays_constant);
    RUN(jitter_on_a_still_hand_is_attenuated);
    RUN(fast_movement_is_followed_closely);
    RUN(speed_adaptation_reduces_lag);
    RUN(repeated_timestamp_returns_previous_output);
    RUN(reset_forgets_history);
    return TEST_SUMMARY();
}
