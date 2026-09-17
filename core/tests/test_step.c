/*
 * Tests for the per-frame glue, run against a fake volume backend so the
 * machine's real volume is never touched.
 */
#include "test.h"

#include "fixtures.h"
#include "pinch_motion.h"
#include "pose.h"
#include "step.h"

#define DT     (1.0 / 30.0)
#define ASPECT 1.0f

/* --- the fake backend --------------------------------------------------- */

static float      fake_volume;
static vol_status fake_get_status;
static vol_status fake_set_status;
static int        fake_get_calls;
static int        fake_set_calls;
static float      fake_last_set;

static vol_status fake_get(float *out)
{
    fake_get_calls++;
    if (fake_get_status == VOL_OK)
        *out = fake_volume;
    return fake_get_status;
}

static vol_status fake_set(float v)
{
    fake_set_calls++;
    fake_last_set = v;
    if (fake_set_status == VOL_OK)
        fake_volume = v;
    return fake_set_status;
}

static const gv_volume_ops fake_ops = { fake_get, fake_set };

static void fake_reset(float volume)
{
    fake_volume = volume;
    fake_get_status = VOL_OK;
    fake_set_status = VOL_OK;
    fake_get_calls = 0;
    fake_set_calls = 0;
    fake_last_set = -1.0f;
}

/* --- driving frames ----------------------------------------------------- */

static double         now;
static gv_step_result last;

static gv_ctx *start(void)
{
    gv_config cfg = gv_default_config();
    cfg.aspect = ASPECT;
    cfg.gain = 1.0f / 3.0f; /* pinned, and flat: expected volumes below are arithmetic on it */
    cfg.gain_fast = cfg.gain;
    cfg.edge_rate = 0.0f;
    cfg.axis_lock_hands = 0.5f; /* likewise: travel counts from 0.5 hand-lengths out */
    now = 0.0;
    return gv_create(&cfg);
}

static void run(gv_ctx *ctx, const gv_frame *hand, float from, float to, double seconds)
{
    int n = (int)(seconds / DT + 0.5);
    for (int i = 1; i <= n; i++) {
        gv_frame f = *hand;
        float offset = from + (to - from) * (float)i / (float)n;
        float dx = offset * gv_hand_scale(hand, ASPECT) / ASPECT;
        for (int k = 0; k < GV_NUM_LANDMARKS; k++)
            f.lm[k].x += dx;
        f.t_sec = now;
        last = gv_step(ctx, &f, &fake_ops);
        now += DT;
    }
}

/* --- tests -------------------------------------------------------------- */

static void arming_starts_from_the_system_volume(void)
{
    fake_reset(0.42f);
    gv_ctx *ctx = start();
    run(ctx, &fixture_victory, 0.0f, 0.0f, 0.5);
    CHECK(last.out.state == GV_ARMED);
    CHECK(fake_get_calls > 0);
    CHECK_NEAR(last.out.volume, 0.42f, 1e-6);
    CHECK(fake_set_calls == 0); /* arming alone writes nothing */
    gv_destroy(ctx);
}

static void sliding_writes_the_target_to_the_backend(void)
{
    fake_reset(0.30f);
    gv_ctx *ctx = start();
    run(ctx, &fixture_victory, 0.0f, 0.0f, 0.5);
    run(ctx, &fixture_victory, 0.0f, 1.5f, 0.6);
    run(ctx, &fixture_victory, 1.5f, 1.5f, 0.5);
    CHECK(fake_set_calls > 0);
    CHECK_NEAR(fake_volume, 0.30f + (1.5f - 0.5f) / 3.0f, 0.02);
    CHECK_NEAR(fake_last_set, last.out.volume, 1e-6);
    CHECK(last.vol == VOL_OK);
    gv_destroy(ctx);
}

static void a_volume_change_made_elsewhere_is_picked_up_while_idle(void)
{
    fake_reset(0.30f);
    gv_ctx *ctx = start();
    run(ctx, &fixture_thumbs_up, 0.0f, 0.0f, 0.2);
    fake_volume = 0.65f; /* the user pressed the volume keys */
    run(ctx, &fixture_victory, 0.0f, 0.0f, 0.5);
    CHECK(last.out.state == GV_ARMED);
    CHECK_NEAR(last.out.volume, 0.65f, 1e-6);
    gv_destroy(ctx);
}

static void the_status_of_a_failing_write_is_on_that_frames_result(void)
{
    fake_reset(0.30f);
    gv_ctx *ctx = start();
    run(ctx, &fixture_victory, 0.0f, 0.0f, 0.5);
    fake_set_status = VOL_ERR_IGNORED;

    int saw_error_on_a_writing_frame = 0;
    for (int i = 1; i <= 12; i++) {
        int before = fake_set_calls;
        run(ctx, &fixture_victory, 0.1f * (float)(i - 1), 0.1f * (float)i, DT);
        if (fake_set_calls > before && last.vol == VOL_ERR_IGNORED)
            saw_error_on_a_writing_frame = 1;
    }
    CHECK(saw_error_on_a_writing_frame);
    gv_destroy(ctx);
}

static void an_output_without_volume_control_is_reported_while_idle(void)
{
    fake_reset(0.30f);
    fake_get_status = VOL_ERR_NO_SOFTWARE_VOLUME;
    gv_ctx *ctx = start();
    run(ctx, &fixture_thumbs_up, 0.0f, 0.0f, 0.2);
    CHECK(last.vol == VOL_ERR_NO_SOFTWARE_VOLUME);
    CHECK(last.out.state == GV_IDLE);
    gv_destroy(ctx);
}

/* `changed` belongs to whichever gesture is armed. A resize that changes its
 * scale must never reach the volume backend: the output's `volume` field is
 * not what moved. */
static void a_resize_never_writes_the_volume(void)
{
    fake_reset(0.40f);
    gv_config cfg = gv_default_config();
    cfg.aspect = FIXTURE_REAL_ASPECT;
    gv_ctx *ctx = gv_create(&cfg);
    now = 0.0;
    int resize_changes = 0;
    for (int i = 0; i < 60; i++) { /* hold open for 1 s, then pinch a third of the way over 1 s */
        float k = i < 30 ? 0.0f : 0.35f * (float)(i - 30) / 30.0f;
        gv_frame f = pinch(k);
        f.t_sec = now;
        last = gv_step(ctx, &f, &fake_ops);
        if (last.out.changed && last.out.kind == GV_KIND_RESIZE)
            resize_changes++;
        now += DT;
    }
    CHECK(last.out.state == GV_ARMED && last.out.kind == GV_KIND_RESIZE);
    CHECK(resize_changes > 0); /* the scale really did move */
    CHECK(fake_set_calls == 0);
    CHECK_NEAR(fake_volume, 0.40f, 1e-6);
    CHECK(last.vol == VOL_OK);
    gv_destroy(ctx);
}

int main(void)
{
    fprintf(stderr, "test_step\n");
    RUN(arming_starts_from_the_system_volume);
    RUN(sliding_writes_the_target_to_the_backend);
    RUN(a_volume_change_made_elsewhere_is_picked_up_while_idle);
    RUN(the_status_of_a_failing_write_is_on_that_frames_result);
    RUN(an_output_without_volume_control_is_reported_while_idle);
    RUN(a_resize_never_writes_the_volume);
    return TEST_SUMMARY();
}
