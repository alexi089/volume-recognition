/*
 * Tests for the per-frame glue, run against fake backends so the machine's
 * real volume and real windows are never touched.
 */
#include "test.h"

#include "fixtures.h"
#include "palm_motion.h"  /* hand_length, moved: shared with test_flick.c */
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

/* The window fake records what was asked of it, in order. */
static win_status fake_acquire_status;
static win_status fake_apply_status;
static win_status fake_minimize_status;
static int        fake_acquire_calls;
static int        fake_apply_calls;
static int        fake_release_calls;
static int        fake_minimize_calls;
static float      fake_last_scale;
static int        fake_applies_after_release; /* a write to a window already let go is a bug */
static int        fake_window_held;

static win_status fake_acquire(void)
{
    fake_acquire_calls++;
    fake_window_held = (fake_acquire_status == WIN_OK);
    return fake_acquire_status;
}

static win_status fake_apply(float scale)
{
    fake_apply_calls++;
    fake_last_scale = scale;
    if (!fake_window_held)
        fake_applies_after_release++;
    return fake_apply_status;
}

static void fake_release(void)
{
    fake_release_calls++;
    fake_window_held = 0;
}

/* Minimising takes its own hold of a window inside the real backend, so this
 * fake neither needs nor touches fake_window_held: a test that sees it called
 * without an acquire is seeing the right thing. */
static win_status fake_minimize(void)
{
    fake_minimize_calls++;
    return fake_minimize_status;
}

static const gv_backends fake_ops = { { fake_get, fake_set },
                                      { fake_acquire, fake_apply, fake_release, fake_minimize } };

/* What the flick driver (step_frame, below) counts. Declared up here because
 * fake_reset zeroes them, and in C a name has to exist before it is used. */
static int        flicks;              /* frames whose output reported a minimise */
static win_status flick_win;           /* the window status on the last such frame */
static win_status win_after_flick;     /* ... and on the frame straight after it */
static int        flick_on_last_frame;

static void fake_reset(float volume)
{
    fake_volume = volume;
    fake_get_status = VOL_OK;
    fake_set_status = VOL_OK;
    fake_get_calls = 0;
    fake_set_calls = 0;
    fake_last_set = -1.0f;
    fake_acquire_status = WIN_OK;
    fake_apply_status = WIN_OK;
    fake_minimize_status = WIN_OK;
    fake_acquire_calls = fake_apply_calls = fake_release_calls = fake_minimize_calls = 0;
    fake_last_scale = -1.0f;
    fake_applies_after_release = 0;
    fake_window_held = 0;
    /* What the driver below counts, zeroed here too: fake_reset is the first
     * line of every test, so this is the one place that runs before all of
     * them, whether the test builds its gv_ctx with start_real or by hand. */
    flicks = 0;
    flick_win = WIN_OK;
    win_after_flick = WIN_OK;
    flick_on_last_frame = 0;
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

/* --- the window side ----------------------------------------------------- */

static gv_ctx *start_real(void)
{
    gv_config cfg = gv_default_config();
    cfg.aspect = FIXTURE_REAL_ASPECT;
    now = 0.0;
    return gv_create(&cfg);
}

static void step_frame(gv_ctx *ctx, const gv_frame *hand)
{
    gv_frame f = { 0 };
    if (hand != NULL)
        f = *hand;
    f.t_sec = now;
    last = gv_step(ctx, &f, &fake_ops);
    /* A flick is over in one frame, so the frame it fired on is the only one
     * that can carry its status - and the frame after it must be clean again.
     * Both are gone by the time a test looks at `last`, so catch them here. */
    if (flick_on_last_frame)
        win_after_flick = last.win;
    flick_on_last_frame = (last.out.kind == GV_KIND_MINIMIZE && last.out.changed);
    if (flick_on_last_frame) {
        flicks++;
        flick_win = last.win;
    }
    now += DT;
}

static void step_pinch(gv_ctx *ctx, float from, float to, double seconds)
{
    int n = (int)(seconds / DT + 0.5);
    for (int i = 1; i <= n; i++) {
        gv_frame f = pinch(from + (to - from) * (float)i / (float)n);
        step_frame(ctx, &f);
    }
}

/* Copied from test_resize.c: the pinch blend `k` that puts the gap at `offset`
 * relative to the gap at k0. Under rate control, holding a fixed offset (not
 * zero, and outside the dead zone) keeps the scale moving at a fixed rate, so
 * unlike a ramp its `changed` fires on essentially every frame - what the
 * throttle tests below need. See k_for_offset_from_hits_the_asked_offset in
 * test_resize.c for a direct check that this bisection lands where asked. */
static float k_for_offset_from(float k0, float offset)
{
    gv_frame base_frame = pinch(k0);
    float base = gv_pinch_gap(&base_frame, FIXTURE_REAL_ASPECT);
    float lo = -0.4f, hi = 0.6f;
    for (int i = 0; i < 40; i++) {
        float mid = (lo + hi) / 2.0f;
        gv_frame f = pinch(mid);
        if (gv_pinch_gap(&f, FIXTURE_REAL_ASPECT) / base - 1.0f > offset) lo = mid; else hi = mid;
    }
    return (lo + hi) / 2.0f;
}

/* The common case: an offset relative to the gap at k = 0, the arming gap of
 * every test below that arms with step_pinch(ctx, 0.0f, 0.0f, ...). */
static float k_for_offset(float offset) { return k_for_offset_from(0.0f, offset); }

static void arming_a_resize_takes_hold_of_the_window_once_and_writes_nothing(void)
{
    fake_reset(0.40f);
    gv_ctx *ctx = start_real();
    step_pinch(ctx, 0.0f, 0.0f, 0.2);
    CHECK(last.out.state == GV_ARMING);
    CHECK(fake_acquire_calls == 0); /* not while still arming */
    step_pinch(ctx, 0.0f, 0.0f, 0.8);
    CHECK(last.out.state == GV_ARMED && last.out.kind == GV_KIND_RESIZE);
    CHECK(fake_acquire_calls == 1);
    CHECK(fake_apply_calls == 0);
    CHECK(fake_release_calls == 0);
    CHECK(last.win == WIN_OK);
    gv_destroy(ctx);
}

static void a_changed_scale_is_written_to_the_window(void)
{
    fake_reset(0.40f);
    gv_ctx *ctx = start_real();
    step_pinch(ctx, 0.0f, 0.0f, 0.5);
    step_pinch(ctx, 0.0f, 0.2f, 0.5);
    step_pinch(ctx, 0.2f, 0.2f, 0.5);
    CHECK(fake_apply_calls > 0);
    CHECK_NEAR(fake_last_scale, last.out.scale, 1e-6); /* the latest scale, not a stale one */
    CHECK(last.out.scale < 0.9f);
    CHECK(fake_acquire_calls == 1);                    /* still the same window */
    CHECK(fake_minimize_calls == 0);                   /* a changed scale is not a flick */
    gv_destroy(ctx);
}

/* Closing the hand ends the resize with one last, rolled-back scale, on a frame
 * whose state is already IDLE. It has to reach the window, and only then is
 * the window let go. */
static void the_rolled_back_scale_is_written_before_the_window_is_let_go(void)
{
    fake_reset(0.40f);
    gv_ctx *ctx = start_real();
    step_pinch(ctx, 0.0f, 0.0f, 0.5);  /* arm at neutral */
    step_pinch(ctx, 0.0f, 0.3f, 0.6);  /* move off-centre briefly by rate ... */
    step_pinch(ctx, 0.3f, 0.0f, 0.15); /* ... back toward neutral ... */
    step_pinch(ctx, 0.0f, 0.0f, 0.6);  /* ... and held still: the size settles, under rate
                                        * control unlike the old ratio law, held OFF-centre
                                        * never settles - it keeps moving for as long as it
                                        * is held, so "set" below has to be captured at
                                        * neutral, not mid-pinch. */
    float set = last.out.scale;

    gv_frame from = pinch(0.0f), fist = aligned_to(&fixture_real_fist, &fixture_real_pinch_open);
    float lowest_written = set;
    for (int i = 1; i <= 21; i++) { /* 0.2 s closing into the recorded fist, then held */
        float k = i <= 6 ? (float)i / 6.0f : 1.0f;
        gv_frame f = from;
        for (int j = 0; j < GV_NUM_LANDMARKS; j++) {
            f.lm[j].x = from.lm[j].x + (fist.lm[j].x - from.lm[j].x) * k;
            f.lm[j].y = from.lm[j].y + (fist.lm[j].y - from.lm[j].y) * k;
        }
        step_frame(ctx, &f);
        if (fake_last_scale < lowest_written)
            lowest_written = fake_last_scale;
    }
    CHECK(last.out.state == GV_IDLE);
    CHECK(lowest_written < set - 0.01f);          /* the closing hand did drag the window down ... */
    CHECK_NEAR(fake_last_scale, set, 0.015);      /* ... and the last write put it back */
    CHECK(fake_release_calls == 1);
    CHECK(fake_applies_after_release == 0);
    gv_destroy(ctx);
}

/* Writes are throttled: at most one every win_write_interval_sec, so a resize
 * armed while the window is being dragged does not cost 30 window writes a
 * second (measured at ~14 ms each; that is what dropped the camera loop from
 * 30 to 16 fps live). Steady motion still gets written, just not every frame.
 *
 * A ramp from 0 to 0.3 (as the brief first suggests) spends its first 14 of 30
 * frames inside the 8% resize_dead_zone, where nothing changes at all - not
 * "steady change". Holding a fixed offset instead (via k_for_offset, as
 * test_resize.c does) keeps the rate-control integrator moving every frame, so
 * `changed` is 1 on essentially every one of the 30 frames measured. Checked:
 * a frame-by-frame probe of the plain ramp counted `last.out.changed` true on
 * only 13 of the 30 frames (confirmed empirically before this fix, see the
 * report); the fixed-offset motion changes on all 30. */
static void window_writes_are_throttled_but_nothing_is_lost(void)
{
    fake_reset(0.40f);
    gv_config cfg = gv_default_config();
    cfg.aspect = FIXTURE_REAL_ASPECT;
    cfg.win_write_interval_sec = 0.06f; /* about every other frame at 30 fps */
    gv_ctx *ctx = gv_create(&cfg);
    now = 0.0;
    step_pinch(ctx, 0.0f, 0.0f, 0.5);
    float k = k_for_offset(-0.30f); /* well past the 8% dead zone, moves every frame */
    int before = fake_apply_calls;
    step_pinch(ctx, k, k, 1.0); /* 30 frames of steady change */
    int writes = fake_apply_calls - before;
    CHECK(writes >= 14 && writes <= 16); /* every other frame, not every frame */
    /* The write cadence (every other frame, starting the frame the gesture's
     * first change fires) does not necessarily land ON the very last of these
     * 30 frames: whichever of the two frames after any pending change is the
     * due one depends on a parity this test does not control (which frame,
     * within the 30, first crossed the dead zone). win_write_interval_sec /
     * DT = 0.06 / (1/30) = 1.8, so at most ceil(1.8) = 2 more frames are ever
     * needed before a pending change is due - not 1, which would only pass by
     * coincidence of that parity. Settling at the arming gap (not holding k)
     * both bounds it at exactly 2 frames (further held-offset motion would
     * keep generating new, later changes of its own, chasing the same
     * problem one frame down) and, once flushed, stays flushed: neutral
     * generates no further change for either settle frame to race against. */
    step_pinch(ctx, 0.0f, 0.0f, 2 * DT);
    CHECK_NEAR(fake_last_scale, last.out.scale, 1e-6); /* the latest value, never a stale one */
    CHECK(fake_applies_after_release == 0);
    gv_destroy(ctx);
}

/* A change that lands inside the interval is not dropped: it is written on the
 * first frame the interval allows, even if that frame itself changed nothing.
 *
 * The brief's original motion held the last two frames at the same non-zero
 * k (0.32) it jumped to. Under rate control that offset is still outside
 * resize_dead_zone, so - as in window_writes_are_throttled_but_nothing_is_lost
 * above - a "held" non-zero offset keeps moving every frame; it never becomes
 * two frames "that do not [change]", so nothing this test captured as `wanted`
 * would still be current by the time it checked. Two changes: the hold
 * returns to the arming gap (k = 0.0, offset 0, inside the dead zone), which
 * genuinely stops the scale moving; and the moving phase is shortened by one
 * frame (0.3 - DT instead of 0.3) so the jump frame lands on a throttled
 * (skipped) frame rather than a frame the interval happened to already allow -
 * confirmed with a frame-by-frame probe (see task-2-report.md) - so the test
 * actually exercises "arrives inside the interval", not a coincidental hit. */
static void a_change_that_arrives_inside_the_interval_is_written_on_the_next_allowed_frame(void)
{
    fake_reset(0.40f);
    gv_config cfg = gv_default_config();
    cfg.aspect = FIXTURE_REAL_ASPECT;
    cfg.win_write_interval_sec = 0.06f;
    gv_ctx *ctx = gv_create(&cfg);
    now = 0.0;
    step_pinch(ctx, 0.0f, 0.0f, 0.5);
    step_pinch(ctx, 0.0f, 0.3f, 0.3 - DT); /* moving */
    /* one more frame that changes, then two frames that do not: the change must land */
    step_pinch(ctx, 0.3f, 0.32f, DT);
    float wanted = last.out.scale;
    step_pinch(ctx, 0.0f, 0.0f, 2 * DT); /* back at the arming gap: genuinely still */
    CHECK_NEAR(fake_last_scale, wanted, 1e-6);
    gv_destroy(ctx);
}

/* Whatever the interval, a gesture's last word (the rolled-back scale on the
 * frame the state goes IDLE) is written at once: releasing never waits for the
 * next allowed frame. Same shape as the_rolled_back_scale_is_written_before_the_
 * window_is_let_go, but with an interval long enough (0.5 s, against a ~0.7 s
 * closing motion) that the release write lands well inside the interval since
 * the previous one - only the `!armed_resize` override, not a naturally due
 * frame, can produce it. (1.0 s, tried first, blocked every write during the
 * closing dip too, so the stale value already at the window happened to equal
 * the rolled-back one anyway and the check could not tell a working override
 * from a broken one; confirmed with a frame-by-frame probe, see the report.) */
static void the_last_word_is_written_whatever_the_interval(void)
{
    fake_reset(0.40f);
    gv_config cfg = gv_default_config();
    cfg.aspect = FIXTURE_REAL_ASPECT;
    cfg.win_write_interval_sec = 0.5f;
    gv_ctx *ctx = gv_create(&cfg);
    now = 0.0;
    step_pinch(ctx, 0.0f, 0.0f, 0.5);  /* arm at neutral */
    step_pinch(ctx, 0.0f, 0.3f, 0.6);  /* move off-centre briefly by rate ... */
    step_pinch(ctx, 0.3f, 0.0f, 0.15); /* ... back toward neutral ... */
    step_pinch(ctx, 0.0f, 0.0f, 0.6);  /* ... and held still: the size settles */
    float set = last.out.scale;

    gv_frame from = pinch(0.0f), fist = aligned_to(&fixture_real_fist, &fixture_real_pinch_open);
    float lowest_written = set;
    for (int i = 1; i <= 21; i++) { /* 0.2 s closing into the recorded fist, then held */
        float k = i <= 6 ? (float)i / 6.0f : 1.0f;
        gv_frame f = from;
        for (int j = 0; j < GV_NUM_LANDMARKS; j++) {
            f.lm[j].x = from.lm[j].x + (fist.lm[j].x - from.lm[j].x) * k;
            f.lm[j].y = from.lm[j].y + (fist.lm[j].y - from.lm[j].y) * k;
        }
        step_frame(ctx, &f);
        if (fake_last_scale < lowest_written)
            lowest_written = fake_last_scale;
    }
    CHECK(last.out.state == GV_IDLE);
    CHECK(lowest_written < set - 0.01f);      /* the closing hand did reach the window ... */
    CHECK_NEAR(fake_last_scale, set, 0.015);  /* ... and the last write put it back */
    CHECK(fake_release_calls == 1);
    CHECK(fake_applies_after_release == 0);
    gv_destroy(ctx);
}

static void a_hand_that_leaves_lets_go_of_the_window_once(void)
{
    fake_reset(0.40f);
    gv_ctx *ctx = start_real();
    step_pinch(ctx, 0.0f, 0.0f, 0.5);
    step_pinch(ctx, 0.0f, 0.2f, 0.5);
    int writes = fake_apply_calls;
    for (int i = 0; i < 30; i++)
        step_frame(ctx, NULL);
    CHECK(last.out.state == GV_IDLE);
    CHECK(fake_release_calls == 1);
    CHECK(fake_apply_calls == writes); /* nothing rolled back, nothing written */
    CHECK(last.win == WIN_OK);

    step_pinch(ctx, 0.0f, 0.0f, 0.5);  /* and the next resize takes hold afresh */
    CHECK(fake_acquire_calls == 2);
    gv_destroy(ctx);
}

/* A window that cannot be resized: said on every armed frame (the overlay shows
 * it for as long as the hand is up), and nothing is written or let go. */
/* Two gestures back to back, with no arm hold and a stall between them: the
 * first window is let go and the second is taken afresh. */
static void a_second_gesture_takes_hold_of_its_own_window(void)
{
    fake_reset(0.40f);
    gv_config cfg = gv_default_config();
    cfg.aspect = FIXTURE_REAL_ASPECT;
    cfg.arm_hold_sec = 0.0f;
    gv_ctx *ctx = gv_create(&cfg);
    now = 0.0;
    step_pinch(ctx, 0.0f, 0.0f, 0.3);
    step_pinch(ctx, 0.0f, 0.2f, 0.5);
    CHECK(fake_acquire_calls == 1 && fake_release_calls == 0);
    now += 1.0; /* a stall, then the arm pose again */
    step_pinch(ctx, 0.0f, 0.0f, 0.3);
    CHECK(last.out.state == GV_ARMED);
    CHECK(fake_release_calls == 1);
    CHECK(fake_acquire_calls == 2);
    gv_destroy(ctx);
}

static void a_window_that_cannot_be_taken_is_reported_and_left_alone(void)
{
    fake_reset(0.40f);
    fake_acquire_status = WIN_ERR_NOT_RESIZABLE;
    gv_ctx *ctx = start_real();
    step_pinch(ctx, 0.0f, 0.0f, 0.5);
    CHECK(last.out.state == GV_ARMED);
    CHECK(last.win == WIN_ERR_NOT_RESIZABLE);
    int armed_frames = 0, reported = 0;
    for (int i = 1; i <= 30; i++) {
        gv_frame f = pinch(0.2f * (float)i / 30.0f);
        step_frame(ctx, &f);
        if (last.out.state == GV_ARMED) {
            armed_frames++;
            reported += (last.win == WIN_ERR_NOT_RESIZABLE);
        }
    }
    CHECK(armed_frames == 30 && reported == 30);
    CHECK(fake_acquire_calls == 1); /* asked once, not once a frame */
    CHECK(fake_apply_calls == 0);
    for (int i = 0; i < 30; i++)
        step_frame(ctx, NULL);
    CHECK(fake_release_calls == 0); /* nothing was held */
    CHECK(last.win == WIN_OK);      /* and the complaint ends with the gesture */
    gv_destroy(ctx);
}

static void a_failing_write_is_reported_on_that_frame(void)
{
    fake_reset(0.40f);
    gv_ctx *ctx = start_real();
    step_pinch(ctx, 0.0f, 0.0f, 0.5);
    fake_apply_status = WIN_ERR_IGNORED;
    int failed_writes = 0, reported = 0;
    for (int i = 1; i <= 30; i++) {
        gv_frame f = pinch(0.2f * (float)i / 30.0f);
        int before = fake_apply_calls;
        step_frame(ctx, &f);
        if (fake_apply_calls > before) {
            failed_writes++;
            reported += (last.win == WIN_ERR_IGNORED);
        } else {
            CHECK(last.win == WIN_OK); /* a frame that wrote nothing has nothing to complain about */
        }
    }
    CHECK(failed_writes > 0 && reported == failed_writes);
    gv_destroy(ctx);
}

/* --- the flick ----------------------------------------------------------- */

/* The open palm, standing still `dy` hand-lengths below where it was recorded,
 * for `seconds`. */
static void step_palm_still(gv_ctx *ctx, float dy, double seconds)
{
    gv_frame f = moved(&fixture_real_open_palm, 0.0f, dy);
    int n = (int)(seconds / DT + 0.5);
    for (int i = 0; i < n; i++)
        step_frame(ctx, &f);
}

/* The open palm falling `dy_hands` hand-lengths from `dy0`, in a straight line
 * at a steady speed, over `seconds`: the gesture itself. The same motion as
 * test_flick.c's `sweep`, built from the same palm_motion.h arithmetic, but
 * driven through gv_step and the fake backends instead of gv_update. */
static void step_palm_fall(gv_ctx *ctx, float dy0, float dy_hands, double seconds)
{
    int n = (int)(seconds / DT + 0.5);
    for (int i = 1; i <= n; i++) {
        gv_frame f = moved(&fixture_real_open_palm, 0.0f, dy0 + dy_hands * (float)i / (float)n);
        step_frame(ctx, &f);
    }
}

static void a_flick_minimises_the_front_window_once(void)
{
    fake_reset(0.40f);
    gv_ctx *ctx = start_real();
    step_palm_still(ctx, 0.0f, 0.3);
    step_palm_fall(ctx, 0.0f, 2.0f, 0.25); /* two hand-lengths in a quarter second */
    step_palm_still(ctx, 2.0f, 0.5);       /* and rests where it landed */

    CHECK(flicks == 1);                    /* the core fired once ... */
    CHECK(fake_minimize_calls == 1);       /* ... and the backend heard it once */
    CHECK(fake_acquire_calls == 0);        /* minimising takes its own hold, inside the backend */
    CHECK(fake_apply_calls == 0 && fake_release_calls == 0);
    CHECK(flick_win == WIN_OK);
    CHECK(fake_set_calls == 0);            /* a flick is not a volume change either */
    gv_destroy(ctx);
}

/* A flick is over in one frame, so its status has exactly one frame to be seen
 * on. Nothing is held afterwards for a later frame to keep complaining about,
 * unlike a resize that could not take its window. */
static void a_failed_minimise_is_reported_on_that_frame(void)
{
    fake_reset(0.40f);
    fake_minimize_status = WIN_ERR_NOT_TRUSTED;
    gv_ctx *ctx = start_real();
    step_palm_still(ctx, 0.0f, 0.3);
    step_palm_fall(ctx, 0.0f, 2.0f, 0.25);
    step_palm_still(ctx, 2.0f, 0.5);

    CHECK(flicks == 1 && fake_minimize_calls == 1);
    CHECK(flick_win == WIN_ERR_NOT_TRUSTED); /* on the frame that fired ... */
    CHECK(win_after_flick == WIN_OK);        /* ... and gone by the next one */
    CHECK(last.win == WIN_OK);
    gv_destroy(ctx);
}

static void a_volume_slide_never_touches_the_window(void)
{
    fake_reset(0.30f);
    gv_ctx *ctx = start();
    run(ctx, &fixture_victory, 0.0f, 0.0f, 0.5);
    run(ctx, &fixture_victory, 0.0f, 1.5f, 1.0);
    CHECK(fake_set_calls > 0);
    CHECK(fake_acquire_calls == 0 && fake_apply_calls == 0 && fake_release_calls == 0);
    CHECK(fake_minimize_calls == 0);
    CHECK(last.win == WIN_OK);
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
    RUN(arming_a_resize_takes_hold_of_the_window_once_and_writes_nothing);
    RUN(a_changed_scale_is_written_to_the_window);
    RUN(the_rolled_back_scale_is_written_before_the_window_is_let_go);
    RUN(window_writes_are_throttled_but_nothing_is_lost);
    RUN(a_change_that_arrives_inside_the_interval_is_written_on_the_next_allowed_frame);
    RUN(the_last_word_is_written_whatever_the_interval);
    RUN(a_hand_that_leaves_lets_go_of_the_window_once);
    RUN(a_second_gesture_takes_hold_of_its_own_window);
    RUN(a_window_that_cannot_be_taken_is_reported_and_left_alone);
    RUN(a_failing_write_is_reported_on_that_frame);
    RUN(a_flick_minimises_the_front_window_once);
    RUN(a_failed_minimise_is_reported_on_that_frame);
    RUN(a_volume_slide_never_touches_the_window);
    return TEST_SUMMARY();
}
