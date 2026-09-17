/*
 * State-machine tests. The hands are real (MediaPipe landmarks from the
 * fixture photos); the motion is synthetic: a fixture is slid sideways by a
 * number of hand-lengths and fed in at 30 fps.
 */
#include "test.h"

#include "fixtures.h"
#include "pose.h"

#define DT     (1.0 / 30.0)
#define ASPECT 1.0f /* the fixture photos are square */

typedef struct {
    gv_ctx   *ctx;
    double    t;
    gv_output out;        /* result of the most recent frame */
    int       changes;    /* frames that reported changed = 1 */
    int       ever_armed;
} session;

static session start(float system_volume)
{
    gv_config cfg = gv_default_config();
    cfg.aspect = ASPECT;
    session s = { 0 };
    s.ctx = gv_create(&cfg);
    gv_set_current_volume(s.ctx, system_volume);
    return s;
}

static void finish(session *s)
{
    gv_destroy(s->ctx);
    s->ctx = NULL;
}

/* One frame. hand = NULL means "no hand in view". offset is how far the hand
 * has been slid from its position in the photo, in hand-lengths (+ = right). */
static void frame(session *s, const gv_frame *hand, float offset)
{
    gv_frame f = { 0 };
    if (hand != NULL) {
        f = *hand;
        float dx = offset * gv_hand_scale(hand, ASPECT) / ASPECT;
        for (int i = 0; i < GV_NUM_LANDMARKS; i++)
            f.lm[i].x += dx;
    }
    f.t_sec = s->t;
    s->out = gv_update(s->ctx, &f);
    s->changes += s->out.changed;
    if (s->out.state == GV_ARMED)
        s->ever_armed = 1;
    s->t += DT;
}

static void hold(session *s, const gv_frame *hand, float offset, double seconds)
{
    int n = (int)(seconds / DT + 0.5);
    for (int i = 0; i < n; i++)
        frame(s, hand, offset);
}

static void slide(session *s, const gv_frame *hand, float from, float to, double seconds)
{
    int n = (int)(seconds / DT + 0.5);
    for (int i = 1; i <= n; i++)
        frame(s, hand, from + (to - from) * (float)i / (float)n);
}

/* Arm at the photo's position and let things settle. */
static void arm(session *s, const gv_frame *hand, float offset)
{
    hold(s, hand, offset, 0.5);
}

static gv_frame shrunk(const gv_frame *src, float factor)
{
    gv_frame out = *src;
    for (int i = 0; i < GV_NUM_LANDMARKS; i++) {
        out.lm[i].x = 0.5f + (src->lm[i].x - 0.5f) * factor;
        out.lm[i].y = 0.5f + (src->lm[i].y - 0.5f) * factor;
    }
    return out;
}

/* --- lifecycle ---------------------------------------------------------- */

static void null_config_gives_working_defaults(void)
{
    gv_ctx *ctx = gv_create(NULL);
    CHECK(ctx != NULL);
    gv_config d = gv_default_config();
    CHECK(d.aspect > 1.0f && d.gain > 0.0f && d.arm_hold_sec > 0.0f && d.finger_ratio > 1.0f);
    gv_destroy(ctx);
    gv_destroy(NULL); /* must be a no-op, like free(NULL) */
}

/* --- arming ------------------------------------------------------------- */

static void no_hand_stays_idle(void)
{
    session s = start(0.5f);
    hold(&s, NULL, 0.0f, 1.0);
    CHECK(s.out.state == GV_IDLE);
    CHECK(s.changes == 0);
    CHECK_NEAR(s.out.volume, 0.5f, 1e-6);
    finish(&s);
}

static void pose_must_be_held_before_arming(void)
{
    session s = start(0.5f);
    hold(&s, &fixture_victory, 0.0f, 0.15);
    CHECK(s.out.state == GV_ARMING);
    hold(&s, &fixture_victory, 0.0f, 0.20);
    CHECK(s.out.state == GV_ARMED);
    finish(&s);
}

static void a_passing_glimpse_of_the_pose_does_not_arm(void)
{
    session s = start(0.5f);
    hold(&s, &fixture_victory, 0.0f, 0.10);
    hold(&s, &fixture_thumbs_up, 0.0f, 0.50);
    CHECK(s.out.state == GV_IDLE);
    CHECK(!s.ever_armed);
    finish(&s);
}

static void arming_itself_does_not_touch_the_volume(void)
{
    session s = start(0.37f);
    arm(&s, &fixture_victory, 0.0f);
    CHECK(s.out.state == GV_ARMED);
    CHECK(s.changes == 0);
    CHECK_NEAR(s.out.volume, 0.37f, 1e-6);
    finish(&s);
}

/* --- the slider --------------------------------------------------------- */

static void sliding_right_raises_the_volume(void)
{
    session s = start(0.30f);
    arm(&s, &fixture_victory, 0.0f);
    slide(&s, &fixture_victory, 0.0f, 1.5f, 0.6);
    hold(&s, &fixture_victory, 1.5f, 0.5);
    /* default gain is 1/3 per hand-length: 1.5 hands = +0.50 */
    CHECK_NEAR(s.out.volume, 0.80f, 0.02);
    CHECK(s.changes > 0);
    finish(&s);
}

static void sliding_left_lowers_the_volume(void)
{
    session s = start(0.70f);
    arm(&s, &fixture_victory, 0.0f);
    slide(&s, &fixture_victory, 0.0f, -0.9f, 0.5);
    hold(&s, &fixture_victory, -0.9f, 0.5);
    CHECK_NEAR(s.out.volume, 0.40f, 0.02);
    finish(&s);
}

static void volume_is_clamped_to_its_range(void)
{
    session s = start(0.90f);
    arm(&s, &fixture_victory, 0.0f);
    slide(&s, &fixture_victory, 0.0f, 2.0f, 0.6);
    hold(&s, &fixture_victory, 2.0f, 0.3);
    CHECK_NEAR(s.out.volume, 1.0f, 1e-6);
    slide(&s, &fixture_victory, 2.0f, -4.0f, 1.0);
    hold(&s, &fixture_victory, -4.0f, 0.3);
    CHECK_NEAR(s.out.volume, 0.0f, 1e-6);
    finish(&s);
}

static void travel_is_measured_in_hand_lengths_not_pixels(void)
{
    /* The same gesture made twice as far from the camera: the hand looks half
     * the size and covers half the pixels, and must give the same result. */
    gv_frame far_hand = shrunk(&fixture_victory, 0.5f);
    session s = start(0.30f);
    arm(&s, &far_hand, 0.0f);
    slide(&s, &far_hand, 0.0f, 1.5f, 0.6);
    hold(&s, &far_hand, 1.5f, 0.5);
    CHECK_NEAR(s.out.volume, 0.80f, 0.02);
    finish(&s);
}

static void movement_inside_the_dead_band_is_not_reported(void)
{
    session s = start(0.50f);
    arm(&s, &fixture_victory, 0.0f);
    /* 0.015 hand-lengths * 1/3 = 0.005 of volume, under the 0.01 dead-band */
    slide(&s, &fixture_victory, 0.0f, 0.015f, 0.3);
    hold(&s, &fixture_victory, 0.015f, 0.5);
    CHECK(s.out.state == GV_ARMED);
    CHECK(s.changes == 0);
    finish(&s);
}

/* --- release ------------------------------------------------------------ */

static void dropping_the_pose_releases_and_keeps_the_volume(void)
{
    session s = start(0.30f);
    arm(&s, &fixture_victory, 0.0f);
    slide(&s, &fixture_victory, 0.0f, 0.9f, 0.5);
    hold(&s, &fixture_victory, 0.9f, 0.5);
    float set = s.out.volume;
    CHECK_NEAR(set, 0.60f, 0.02);

    int before = s.changes;
    hold(&s, &fixture_thumbs_up, 0.0f, 0.4);
    CHECK(s.out.state == GV_IDLE);
    CHECK_NEAR(s.out.volume, set, 1e-6);
    CHECK(s.changes == before);
    finish(&s);
}

static void one_lost_frame_does_not_release(void)
{
    session s = start(0.50f);
    arm(&s, &fixture_victory, 0.0f);
    float before = s.out.volume;
    frame(&s, NULL, 0.0f);
    CHECK(s.out.state == GV_ARMED);
    CHECK_NEAR(s.out.volume, before, 1e-6);
    frame(&s, &fixture_victory, 0.0f);
    CHECK(s.out.state == GV_ARMED);
    finish(&s);
}

static void a_stall_longer_than_the_grace_period_releases(void)
{
    session s = start(0.50f);
    arm(&s, &fixture_victory, 0.0f);
    CHECK(s.out.state == GV_ARMED);
    s.t += 2.0; /* the app froze; the hand is somewhere else entirely now */
    frame(&s, &fixture_victory, 1.5f);
    CHECK(s.out.state != GV_ARMED);
    CHECK(s.changes == 0);
    CHECK_NEAR(s.out.volume, 0.50f, 1e-6);
    finish(&s);
}

static void rearming_starts_from_the_new_position_without_a_jump(void)
{
    session s = start(0.30f);
    arm(&s, &fixture_victory, 0.0f);
    slide(&s, &fixture_victory, 0.0f, 0.9f, 0.5);
    hold(&s, &fixture_victory, 0.9f, 0.5);
    float set = s.out.volume;
    hold(&s, &fixture_thumbs_up, 0.0f, 0.4);
    CHECK(s.out.state == GV_IDLE);

    gv_set_current_volume(s.ctx, set); /* what the glue does every idle frame */
    int before = s.changes;
    arm(&s, &fixture_victory, -1.0f);  /* far from where the last slide ended */
    CHECK(s.out.state == GV_ARMED);
    CHECK(s.changes == before);
    CHECK_NEAR(s.out.volume, set, 1e-6);
    finish(&s);
}

static void external_volume_updates_are_ignored_while_armed(void)
{
    session s = start(0.50f);
    arm(&s, &fixture_victory, 0.0f);
    gv_set_current_volume(s.ctx, 0.10f);
    hold(&s, &fixture_victory, 0.0f, 0.2);
    CHECK_NEAR(s.out.volume, 0.50f, 1e-6);
    finish(&s);
}

/* --- things that must never move the volume ----------------------------- */

static void other_hand_shapes_moving_across_the_frame_do_nothing(void)
{
    session s = start(0.50f);
    slide(&s, &fixture_pointing_up, -2.0f, 2.0f, 1.0);
    slide(&s, &fixture_thumbs_up, 2.0f, -2.0f, 1.0);
    CHECK(s.out.state == GV_IDLE);
    CHECK(!s.ever_armed);
    CHECK(s.changes == 0);
    finish(&s);
}

static void garbage_landmarks_are_treated_as_no_hand(void)
{
    gv_frame bad = fixture_victory;
    bad.lm[GV_WRIST].x = NAN;
    session s = start(0.50f);
    hold(&s, &bad, 0.0f, 1.0);
    CHECK(s.out.state == GV_IDLE);
    CHECK(s.changes == 0);
    CHECK(isfinite(s.out.volume));
    finish(&s);
}

int main(void)
{
    fprintf(stderr, "test_gesture\n");
    RUN(null_config_gives_working_defaults);
    RUN(no_hand_stays_idle);
    RUN(pose_must_be_held_before_arming);
    RUN(a_passing_glimpse_of_the_pose_does_not_arm);
    RUN(arming_itself_does_not_touch_the_volume);
    RUN(sliding_right_raises_the_volume);
    RUN(sliding_left_lowers_the_volume);
    RUN(volume_is_clamped_to_its_range);
    RUN(travel_is_measured_in_hand_lengths_not_pixels);
    RUN(movement_inside_the_dead_band_is_not_reported);
    RUN(dropping_the_pose_releases_and_keeps_the_volume);
    RUN(one_lost_frame_does_not_release);
    RUN(a_stall_longer_than_the_grace_period_releases);
    RUN(rearming_starts_from_the_new_position_without_a_jump);
    RUN(external_volume_updates_are_ignored_while_armed);
    RUN(other_hand_shapes_moving_across_the_frame_do_nothing);
    RUN(garbage_landmarks_are_treated_as_no_hand);
    return TEST_SUMMARY();
}
