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
#define REAL   FIXTURE_REAL_ASPECT /* the frames from the live recording are 16:9 */

/* The tests pin the gain, and pin it FLAT (the same slow and fast, so speed
 * makes no difference). Expected volumes below are arithmetic on this value;
 * the shipped defaults are tuning choices that must be free to change. The
 * speed-dependent part has its own tests, with its own pinned numbers. */
#define GAIN (1.0f / 3.0f)

/* Nothing moves until the hand has travelled LOCK hand-lengths along one axis
 * (see "which way is the slider" below); travel is counted from there. So a
 * slide of h hand-lengths is worth GAIN * (h - LOCK). Pinned, like the gain. */
#define LOCK 0.5f
#define AFTER(start, hands) ((start) + GAIN * ((hands) - LOCK))
#define BEFORE(start, hands) ((start) - GAIN * ((hands) - LOCK))

typedef struct {
    gv_ctx   *ctx;
    float     aspect;
    double    t;
    gv_output out;        /* result of the most recent frame */
    int       changes;    /* frames that reported changed = 1 */
    int       ever_armed;
    int       releases;   /* times the state went from ARMED to anything else */
    float     max_rate;   /* fastest volume change seen, per second */
} session;

static session start_with_aspect(float system_volume, float aspect)
{
    gv_config cfg = gv_default_config();
    cfg.aspect = aspect;
    cfg.gain = GAIN;
    cfg.gain_fast = GAIN;
    cfg.axis_lock_hands = LOCK;
    cfg.edge_rate = 0.0f; /* edge push has its own tests; off everywhere else */
    session s = { 0 };
    s.aspect = aspect;
    s.ctx = gv_create(&cfg);
    gv_set_current_volume(s.ctx, system_volume);
    return s;
}

static session start(float system_volume)      { return start_with_aspect(system_volume, ASPECT); }
static session start_real(float system_volume) { return start_with_aspect(system_volume, REAL); }

static void finish(session *s)
{
    gv_destroy(s->ctx);
    s->ctx = NULL;
}

/* One frame. hand = NULL means "no hand in view". The offsets are how far the
 * hand has been moved from its position in the photo, in hand-lengths:
 * right is +, UP is + (image y grows downwards, hence the minus below). */
static void frame_xy(session *s, const gv_frame *hand, float right, float up)
{
    gv_frame f = { 0 };
    if (hand != NULL) {
        f = *hand;
        float dx = right * gv_hand_scale(hand, s->aspect) / s->aspect;
        float dy = -up * gv_hand_scale(hand, s->aspect);
        for (int i = 0; i < GV_NUM_LANDMARKS; i++) {
            f.lm[i].x += dx;
            f.lm[i].y += dy;
        }
    }
    f.t_sec = s->t;
    gv_state before = s->out.state;
    float volume_before = s->out.volume;
    int first = (s->t == 0.0);
    s->out = gv_update(s->ctx, &f);
    if (!first && fabsf(s->out.volume - volume_before) / (float)DT > s->max_rate)
        s->max_rate = fabsf(s->out.volume - volume_before) / (float)DT;
    if (before == GV_ARMED && s->out.state != GV_ARMED)
        s->releases++;
    s->changes += s->out.changed;
    if (s->out.state == GV_ARMED)
        s->ever_armed = 1;
    s->t += DT;
}

static void frame(session *s, const gv_frame *hand, float right)
{
    frame_xy(s, hand, right, 0.0f);
}

/* Move in a straight line from (x0, y0) to (x1, y1), in hand-lengths. */
static void slide_xy(session *s, const gv_frame *hand, float x0, float y0, float x1, float y1,
                     double seconds)
{
    int n = (int)(seconds / DT + 0.5);
    for (int i = 1; i <= n; i++) {
        float k = (float)i / (float)n;
        frame_xy(s, hand, x0 + (x1 - x0) * k, y0 + (y1 - y0) * k);
    }
}

static void hold_xy(session *s, const gv_frame *hand, float right, float up, double seconds)
{
    int n = (int)(seconds / DT + 0.5);
    for (int i = 0; i < n; i++)
        frame_xy(s, hand, right, up);
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

/* Move and resize `src` so that it is the same hand, in the same place, at the
 * same size as `ref`, only in a different shape. The recording's frames were
 * taken seconds apart with the hand in different places; without this, going
 * from one to another would also be a jump across the frame. */
static gv_frame aligned_to(const gv_frame *src, const gv_frame *ref, float aspect)
{
    gv_frame out = *src;
    float k = gv_hand_scale(ref, aspect) / gv_hand_scale(src, aspect);
    for (int i = 0; i < GV_NUM_LANDMARKS; i++) { /* resize about the wrist */
        out.lm[i].x = src->lm[0].x + (src->lm[i].x - src->lm[0].x) * k;
        out.lm[i].y = src->lm[0].y + (src->lm[i].y - src->lm[0].y) * k;
    }
    float dx = (gv_palm_x(ref, aspect) - gv_palm_x(&out, aspect)) / aspect;
    float dy = ref->lm[0].y - out.lm[0].y;
    for (int i = 0; i < GV_NUM_LANDMARKS; i++) {
        out.lm[i].x += dx;
        out.lm[i].y += dy;
    }
    return out;
}

/* --- lifecycle ---------------------------------------------------------- */

static void null_config_gives_working_defaults(void)
{
    gv_ctx *ctx = gv_create(NULL);
    CHECK(ctx != NULL);
    gv_config d = gv_default_config();
    CHECK(d.aspect > 1.0f && d.gain > 0.0f && d.arm_hold_sec > 0.0f && d.pose.extend_reach > 1.0f);
    gv_destroy(ctx);
    gv_destroy(NULL); /* must be a no-op, like free(NULL) */
}

static void nonsense_config_is_rejected(void)
{
    gv_config c;

    c = gv_default_config(); c.aspect = 0.0f;              CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.aspect = NAN;               CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.pose.extend_reach = -1.0f;  CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.pose.curl_straight = NAN;   CHECK(gv_create(&c) == NULL);
    /* the curl limits must sit below the extend limits, or one finger could be
     * extended and curled at once */
    c = gv_default_config(); c.pose.curl_reach = c.pose.extend_reach + 0.1f;
                                                           CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.pose.curl_straight = 0.99f; CHECK(gv_create(&c) == NULL);
    /* the scale gate must let an unchanged hand (1.0) through */
    c = gv_default_config(); c.pose.thumb_extend_reach = 0.0f; CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.pose.thumb_curl_reach = c.pose.thumb_extend_reach + 0.1f;
                                                           CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.pose.thumb_curl_straight = 0.99f; CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.pose.pinch_gap_min = -0.1f; CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.pose.pinch_gap_min = NAN;   CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.gate_scale_min = 1.1f;      CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.gate_scale_max = 0.9f;      CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.gate_max_speed = 0.0f;      CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.max_slew = 0.0f;            CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.max_slew = NAN;             CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.axis_lock_hands = -0.1f;    CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.axis_dominance = 0.5f;      CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.gain_fast = NAN;            CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.edge_margin = 0.5f;         CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.edge_margin = -0.01f;       CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.edge_rate = -0.1f;          CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.accel_speed_lo = -1.0f;     CHECK(gv_create(&c) == NULL);
    /* the fast speed must be above the slow one, or the ramp divides by zero */
    c = gv_default_config(); c.accel_speed_hi = c.accel_speed_lo; CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.arm_hold_sec = NAN;         CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.arm_hold_sec = -0.1f;       CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.release_grace_sec = -1.0f;  CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.gain = INFINITY;            CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.dead_band = -0.01f;         CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.filter_min_cutoff = 0.0f;   CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.filter_d_cutoff = 0.0f;     CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.filter_beta = -1.0f;        CHECK(gv_create(&c) == NULL);

    /* a negative gain is legitimate: it reverses the slider's direction */
    c = gv_default_config(); c.gain = -c.gain; c.gain_fast = -c.gain_fast;
    gv_ctx *ctx = gv_create(&c);
    CHECK(ctx != NULL);
    gv_destroy(ctx);
}

static void the_output_says_which_gesture_is_active(void)
{
    session s = start(0.50f);
    hold(&s, NULL, 0.0f, 0.2);
    CHECK(s.out.kind == GV_KIND_NONE);
    CHECK_NEAR(s.out.scale, 1.0f, 1e-6);
    hold(&s, &fixture_victory, 0.0f, 0.1);
    CHECK(s.out.state == GV_ARMING && s.out.kind == GV_KIND_VOLUME);
    arm(&s, &fixture_victory, 0.0f);
    CHECK(s.out.kind == GV_KIND_VOLUME);
    CHECK_NEAR(s.out.scale, 1.0f, 1e-6); /* scale belongs to resize; it is 1 for every other gesture */
    hold(&s, &fixture_thumbs_up, 0.0f, 0.4);
    CHECK(s.out.state == GV_IDLE && s.out.kind == GV_KIND_NONE);
    finish(&s);
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
    CHECK_NEAR(s.out.volume, AFTER(0.30f, 1.5f), 0.02);
    CHECK(s.changes > 0);
    finish(&s);
}

static void sliding_left_lowers_the_volume(void)
{
    session s = start(0.70f);
    arm(&s, &fixture_victory, 0.0f);
    slide(&s, &fixture_victory, 0.0f, -0.9f, 0.5);
    hold(&s, &fixture_victory, -0.9f, 0.5);
    CHECK_NEAR(s.out.volume, BEFORE(0.70f, 0.9f), 0.02);
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

/* The last step to an end of the range can be smaller than the dead-band. It
 * must still be reported, or the slider can never reach silence or full. */
static void a_small_final_step_still_reaches_the_end_of_the_range(void)
{
    session s = start(0.50f);
    arm(&s, &fixture_victory, 0.0f);
    /* (1.985 - LOCK) hand-lengths * 1/3 = +0.495: parks the volume just short of 1.0 */
    slide(&s, &fixture_victory, 0.0f, 1.985f, 0.8);
    hold(&s, &fixture_victory, 1.985f, 0.6);
    CHECK(s.out.volume > 0.98f && s.out.volume < 1.0f);

    slide(&s, &fixture_victory, 1.985f, 2.1f, 0.2); /* target clamps to 1.0 */
    hold(&s, &fixture_victory, 2.1f, 0.5);
    CHECK(s.out.volume == 1.0f);

    int before = s.changes;
    hold(&s, &fixture_victory, 2.1f, 0.5); /* parked at the end: no repeats */
    CHECK(s.changes == before);
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
    CHECK_NEAR(s.out.volume, AFTER(0.30f, 1.5f), 0.02);
    finish(&s);
}

/* What the dead-band is for: a hand held "still" wobbles, and that must not
 * become a stream of volume writes. The band is measured from the last value
 * reported, which can itself be almost a full band behind the hand, so a
 * wobble may trip it once; it must not keep tripping it. */
static void a_wobbling_hand_does_not_chatter(void)
{
    session s = start(0.50f);
    arm(&s, &fixture_victory, 0.0f);
    slide(&s, &fixture_victory, 0.0f, 1.0f, 0.5); /* get the slider going first */
    hold(&s, &fixture_victory, 1.0f, 1.0);
    int before = s.changes;
    CHECK(before > 0);

    /* two seconds of +/-0.012 hand-lengths: +/-0.004 of volume at the pinned
     * gain, under half the 0.01 band */
    for (int i = 0; i < 60; i++)
        frame(&s, &fixture_victory, 1.0f + ((i / 5) % 2 ? 0.012f : -0.012f));
    CHECK(s.out.state == GV_ARMED);
    CHECK(s.changes - before <= 1);
    finish(&s);
}

/* --- which way is the slider -------------------------------------------- */

/* Up and down work as well as left and right. The two are not added together:
 * measured on real slides, a sideways sweep wanders 0.7 - 1.6 hand-lengths
 * vertically (the arm swings in an arc), which added in would be 13 - 32% of
 * unasked-for volume. Instead the first clearly dominant movement picks the
 * axis, and the slide stays on it until release. */

static void sliding_up_raises_the_volume(void)
{
    session s = start(0.30f);
    arm(&s, &fixture_victory, 0.0f);
    slide_xy(&s, &fixture_victory, 0.0f, 0.0f, 0.0f, 1.5f, 0.6);
    hold_xy(&s, &fixture_victory, 0.0f, 1.5f, 0.5);
    CHECK(s.out.axis == GV_AXIS_Y);
    CHECK_NEAR(s.out.volume, AFTER(0.30f, 1.5f), 0.02);
    finish(&s);
}

static void sliding_down_lowers_the_volume(void)
{
    session s = start(0.70f);
    arm(&s, &fixture_victory, 0.0f);
    slide_xy(&s, &fixture_victory, 0.0f, 0.0f, 0.0f, -1.1f, 0.5);
    hold_xy(&s, &fixture_victory, 0.0f, -1.1f, 0.5);
    CHECK(s.out.axis == GV_AXIS_Y);
    CHECK_NEAR(s.out.volume, BEFORE(0.70f, 1.1f), 0.02);
    finish(&s);
}

static void nothing_moves_until_an_axis_is_chosen(void)
{
    session s = start(0.50f);
    arm(&s, &fixture_victory, 0.0f);
    slide(&s, &fixture_victory, 0.0f, 0.4f, 0.3); /* under LOCK */
    hold(&s, &fixture_victory, 0.4f, 0.4);
    CHECK(s.out.axis == GV_AXIS_NONE);
    CHECK(s.changes == 0);

    /* and then travel counts from the lock point, so there is no jump and a
     * small adjustment stays possible */
    slide(&s, &fixture_victory, 0.4f, 0.62f, 0.3);
    hold(&s, &fixture_victory, 0.62f, 0.5);
    CHECK(s.out.axis == GV_AXIS_X);
    CHECK_NEAR(s.out.volume, AFTER(0.50f, 0.62f), 0.015); /* +0.04, not +0.2 */
    finish(&s);
}

static void a_sideways_slide_ignores_the_arc_of_the_arm(void)
{
    session s = start(0.30f);
    arm(&s, &fixture_victory, 0.0f);
    /* right 1.5, rising 0.2 on the way to the lock and 1.0 by the end: once
     * the slide is sideways, a hand-length of vertical wander changes nothing */
    slide_xy(&s, &fixture_victory, 0.0f, 0.0f, 0.6f, 0.2f, 0.3);
    slide_xy(&s, &fixture_victory, 0.6f, 0.2f, 1.5f, 1.0f, 0.4);
    hold_xy(&s, &fixture_victory, 1.5f, 1.0f, 0.5);
    CHECK(s.out.axis == GV_AXIS_X);
    CHECK_NEAR(s.out.volume, AFTER(0.30f, 1.5f), 0.02);
    finish(&s);
}

static void an_up_down_slide_ignores_sideways_wander(void)
{
    session s = start(0.30f);
    arm(&s, &fixture_victory, 0.0f);
    slide_xy(&s, &fixture_victory, 0.0f, 0.0f, 0.2f, 0.6f, 0.3);
    slide_xy(&s, &fixture_victory, 0.2f, 0.6f, -1.0f, 1.5f, 0.4);
    hold_xy(&s, &fixture_victory, -1.0f, 1.5f, 0.5);
    CHECK(s.out.axis == GV_AXIS_Y);
    CHECK_NEAR(s.out.volume, AFTER(0.30f, 1.5f), 0.02);
    finish(&s);
}

/* A diagonal is not a choice. The axis is picked only when one direction is
 * clearly ahead (twice the other), however far the hand has gone. */
static void a_diagonal_does_not_pick_an_axis(void)
{
    session s = start(0.50f);
    arm(&s, &fixture_victory, 0.0f);
    slide_xy(&s, &fixture_victory, 0.0f, 0.0f, 1.2f, 1.0f, 0.5);
    hold_xy(&s, &fixture_victory, 1.2f, 1.0f, 0.4);
    CHECK(s.out.axis == GV_AXIS_NONE);
    CHECK(s.changes == 0);
    finish(&s);
}

static void each_slide_picks_its_own_axis(void)
{
    session s = start(0.30f);
    arm(&s, &fixture_victory, 0.0f);
    slide(&s, &fixture_victory, 0.0f, 1.1f, 0.5);
    hold(&s, &fixture_victory, 1.1f, 0.4);
    CHECK(s.out.axis == GV_AXIS_X);
    float set = s.out.volume;

    hold(&s, &fixture_thumbs_up, 0.0f, 0.4); /* release */
    CHECK(s.out.state == GV_IDLE);
    CHECK(s.out.axis == GV_AXIS_NONE);

    gv_set_current_volume(s.ctx, set);
    arm(&s, &fixture_victory, 1.1f);
    slide_xy(&s, &fixture_victory, 1.1f, 0.0f, 1.1f, 1.1f, 0.5);
    hold_xy(&s, &fixture_victory, 1.1f, 1.1f, 0.4);
    CHECK(s.out.axis == GV_AXIS_Y);
    CHECK_NEAR(s.out.volume, AFTER(set, 1.1f), 0.02);
    finish(&s);
}

/* --- precise when slow, quick when fast --------------------------------- */

/* One fixed gain cannot be both. At 1/3 the first live session was too twitchy
 * to set a level; at 0.2 the second was too slow to get anywhere. So the gain
 * depends on how fast the hand is moving, the way a mouse pointer's does. */

static session start_accelerated(float system_volume)
{
    gv_config cfg = gv_default_config();
    cfg.aspect = ASPECT;
    cfg.axis_lock_hands = LOCK;
    cfg.gain = 0.1f;          /* at or below 1 hand-length/s */
    cfg.gain_fast = 0.5f;     /* at or above 5 */
    cfg.accel_speed_lo = 1.0f;
    cfg.accel_speed_hi = 5.0f;
    session s = { 0 };
    s.aspect = ASPECT;
    s.ctx = gv_create(&cfg);
    gv_set_current_volume(s.ctx, system_volume);
    return s;
}

static void a_slow_slide_uses_the_fine_gain(void)
{
    session s = start_accelerated(0.30f);
    arm(&s, &fixture_victory, 0.0f);
    slide(&s, &fixture_victory, 0.0f, 1.5f, 3.0); /* 0.5 hand-lengths/s */
    hold(&s, &fixture_victory, 1.5f, 1.0);
    CHECK_NEAR(s.out.volume, 0.30f + 0.1f * (1.5f - LOCK), 0.015);
    finish(&s);
}

static void the_same_distance_fast_goes_much_further(void)
{
    session s = start_accelerated(0.30f);
    arm(&s, &fixture_victory, 0.0f);
    slide(&s, &fixture_victory, 0.0f, 1.5f, 0.2); /* 7.5 hand-lengths/s */
    hold(&s, &fixture_victory, 1.5f, 1.0);
    /* not the full 0.5 * 1.0: the hand starts and stops, and the filter
     * smooths the speed, so part of the travel is at lower gain */
    CHECK(s.out.volume > 0.30f + 0.30f);
    CHECK(s.out.volume < 0.30f + 0.5f * (1.5f - LOCK) + 0.02f);
    finish(&s);
}

static void gain_rises_smoothly_between_the_two_speeds(void)
{
    float gained[3];
    double seconds[3] = { 1.0, 0.5, 0.3 }; /* 1.5, 3 and 5 hand-lengths/s */
    for (int i = 0; i < 3; i++) {
        session s = start_accelerated(0.10f);
        arm(&s, &fixture_victory, 0.0f);
        slide(&s, &fixture_victory, 0.0f, 1.5f, seconds[i]);
        hold(&s, &fixture_victory, 1.5f, 1.0);
        gained[i] = s.out.volume - 0.10f;
        finish(&s);
    }
    CHECK(gained[0] < gained[1] && gained[1] < gained[2]);
    CHECK(gained[1] > 1.3f * gained[0]); /* a real difference, not rounding */
}

/* The price of speed-dependent gain, stated rather than hidden: out fast and
 * back slow does not return to where it started. It is a relative control,
 * like a mouse, not a physical slider. */
static void out_fast_and_back_slow_is_a_net_change(void)
{
    session s = start_accelerated(0.30f);
    arm(&s, &fixture_victory, 0.0f);
    slide(&s, &fixture_victory, 0.0f, 2.0f, 0.25);
    hold(&s, &fixture_victory, 2.0f, 0.5);
    slide(&s, &fixture_victory, 2.0f, LOCK, 4.0);
    hold(&s, &fixture_victory, LOCK, 0.5);
    CHECK(s.out.volume > 0.30f + 0.15f);
    finish(&s);
}

/* --- running out of frame ----------------------------------------------- */

/* A webcam frame is small: measured live, 8.1 hand-lengths wide and 4.6 tall,
 * and the hand itself is two tall. A slide therefore hits the edge of the
 * picture long before the volume hits the end of its range, and once the
 * fingers leave the picture tracking is gone. So reaching the edge in the
 * slide's own direction and holding there keeps the volume moving, the way
 * dragging against the edge of a window keeps it scrolling. */

#define EDGE_RATE 0.3f
#define EDGE_GAIN 0.1f /* low, so a slide to the edge leaves room to see the push */

static session start_with_edge_push(float system_volume)
{
    gv_config cfg = gv_default_config();
    cfg.aspect = ASPECT;
    cfg.gain = EDGE_GAIN;
    cfg.gain_fast = EDGE_GAIN;
    cfg.axis_lock_hands = LOCK;
    cfg.edge_margin = 0.04f;
    cfg.edge_rate = EDGE_RATE;
    session s = { 0 };
    s.aspect = ASPECT;
    s.ctx = gv_create(&cfg);
    gv_set_current_volume(s.ctx, system_volume);
    return s;
}

/* The victory photo's hand reaches each edge of the picture after sliding
 * 3.56 hand-lengths right, 1.03 left, 2.24 up (fingertips) or 1.39 down
 * (wrist); each test goes a little past that. */

static void holding_at_the_right_edge_keeps_raising_the_volume(void)
{
    session s = start_with_edge_push(0.0f);
    arm(&s, &fixture_victory, 0.0f);
    slide(&s, &fixture_victory, 0.0f, 2.0f, 0.8);
    hold(&s, &fixture_victory, 2.0f, 0.6); /* mid-picture: travel only, nothing extra */
    CHECK_NEAR(s.out.volume, EDGE_GAIN * (2.0f - LOCK), 0.015);

    slide(&s, &fixture_victory, 2.0f, 3.7f, 0.6);
    float arrived = s.out.volume;
    hold(&s, &fixture_victory, 3.7f, 1.0);
    CHECK(s.out.volume > arrived + 0.8f * EDGE_RATE);
    CHECK(s.out.volume < EDGE_GAIN * (3.7f - LOCK) + 1.2f * EDGE_RATE);

    hold(&s, &fixture_victory, 3.7f, 3.0);
    CHECK_NEAR(s.out.volume, 1.0f, 1e-6); /* and held there, it gets all the way */
    finish(&s);
}

static void holding_at_the_left_edge_keeps_lowering_it(void)
{
    session s = start_with_edge_push(1.0f);
    arm(&s, &fixture_victory, 0.0f);
    slide(&s, &fixture_victory, 0.0f, -1.2f, 0.5);
    float arrived = s.out.volume;
    hold(&s, &fixture_victory, -1.2f, 1.0);
    CHECK(s.out.volume < arrived - 0.8f * EDGE_RATE);
    finish(&s);
}

static void the_top_edge_pushes_up_and_the_bottom_edge_down(void)
{
    session up = start_with_edge_push(0.20f);
    arm(&up, &fixture_victory, 0.0f);
    slide_xy(&up, &fixture_victory, 0.0f, 0.0f, 0.0f, 2.4f, 0.8); /* fingertips at the top */
    float arrived = up.out.volume;
    hold_xy(&up, &fixture_victory, 0.0f, 2.4f, 1.0);
    CHECK(up.out.axis == GV_AXIS_Y);
    CHECK(up.out.volume > arrived + 0.8f * EDGE_RATE);
    finish(&up);

    session down = start_with_edge_push(0.90f);
    arm(&down, &fixture_victory, 0.0f);
    slide_xy(&down, &fixture_victory, 0.0f, 0.0f, 0.0f, -1.5f, 0.6); /* wrist at the bottom */
    arrived = down.out.volume;
    hold_xy(&down, &fixture_victory, 0.0f, -1.5f, 1.0);
    CHECK(down.out.volume < arrived - 0.8f * EDGE_RATE);
    finish(&down);
}

/* Lost frames must not bank up push, any more than they bank up slew. After a
 * 0.2 s gap at the edge, the first frame back is credited 0.1 s of push, not 0.2. */
static void a_gap_at_the_edge_does_not_earn_a_big_push(void)
{
    session s = start_with_edge_push(0.0f);
    arm(&s, &fixture_victory, 0.0f);
    slide(&s, &fixture_victory, 0.0f, 3.7f, 1.0);
    hold(&s, &fixture_victory, 3.7f, 0.5);       /* settled at the right edge, being pushed */
    hold(&s, NULL, 0.0f, 0.2);                   /* tracking drops out */
    float before = s.out.volume;
    frame(&s, &fixture_victory, 3.7f);
    CHECK(s.out.state == GV_ARMED);
    float step = s.out.volume - before;
    CHECK(step > 0.0f);
    CHECK(step < EDGE_RATE * 0.1f + 0.015f);     /* 0.1 s of credit plus a little filter settle; 0.2 s would be 0.06+ */
    finish(&s);
}

/* Only the edge the slide is heading for counts. The case that matters: a
 * raised hand very often has its wrist at or below the bottom of the picture
 * (the landmark model extrapolates it). Sliding UP from there, the wrist is
 * still "at the bottom edge" for a while, and that must not drag the volume
 * down against the slide. Nor does an edge matter to a slide on the other
 * axis, or before an axis is chosen. */
static void an_edge_the_slide_is_not_heading_for_does_nothing(void)
{
    session s = start_with_edge_push(0.30f);
    arm(&s, &fixture_victory, 0.0f);
    /* re-arm low: wrist 0.34 below the bottom of the picture */
    hold(&s, &fixture_thumbs_up, 0.0f, 0.4);
    gv_set_current_volume(s.ctx, 0.30f);
    hold_xy(&s, &fixture_victory, 0.0f, -2.2f, 0.5);
    CHECK(s.out.state == GV_ARMED);

    slide_xy(&s, &fixture_victory, 0.0f, -2.2f, 0.0f, -1.5f, 0.4); /* up 0.7: wrist still past the edge */
    hold_xy(&s, &fixture_victory, 0.0f, -1.5f, 1.0);
    CHECK(s.out.axis == GV_AXIS_Y);
    CHECK_NEAR(s.out.volume, 0.30f + EDGE_GAIN * (0.7f - LOCK), 0.015); /* up a little, not pushed down */
    finish(&s);

    session low = start_with_edge_push(0.30f);
    arm(&low, &fixture_victory, 0.0f);
    /* a slide to the LEFT (quieter) that sinks until the wrist is at the bottom
     * edge: the bottom edge also means "quieter", but only to an up/down slide */
    slide_xy(&low, &fixture_victory, 0.0f, 0.0f, -0.9f, -0.3f, 0.6);
    slide_xy(&low, &fixture_victory, -0.9f, -0.3f, -0.9f, -1.5f, 0.4);
    hold_xy(&low, &fixture_victory, -0.9f, -1.5f, 0.4);
    CHECK(low.out.axis == GV_AXIS_X);
    float before = low.out.volume;
    hold_xy(&low, &fixture_victory, -0.9f, -1.5f, 1.0);
    CHECK_NEAR(low.out.volume, before, 0.011);
    finish(&low);
}

/* On a 16:9 picture there is 1.78x less room vertically than horizontally, so
 * vertical travel counts 1.78x as much: the same fraction of the picture gives
 * the same change on either axis. */
static void vertical_travel_is_scaled_by_the_aspect_ratio(void)
{
    const gv_frame *hand = &fixture_real_tight_two;
    session s = start_real(0.20f);
    arm(&s, hand, 0.0f);
    slide_xy(&s, hand, 0.0f, 0.0f, 0.0f, 1.1f, 0.5);
    hold_xy(&s, hand, 0.0f, 1.1f, 0.6);
    CHECK(s.out.axis == GV_AXIS_Y);
    CHECK_NEAR(s.out.volume, 0.20f + GAIN * REAL * (1.1f - LOCK), 0.03);
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
    CHECK_NEAR(set, AFTER(0.30f, 0.9f), 0.02);

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
    /* Released, and the very same frame (it shows the pose) starts a fresh
     * arming rather than being thrown away: no frame of latency on the way back. */
    CHECK(s.out.state == GV_ARMING);
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

/* --- a real, relaxed hand (frames from the first live recording) -------- */

static void a_relaxed_two_finger_pose_arms(void)
{
    session s = start_real(0.50f);
    arm(&s, &fixture_real_relaxed_two, 0.0f);
    CHECK(s.out.state == GV_ARMED);
    finish(&s);
}

static void an_open_palm_does_not_arm(void)
{
    session s = start_real(0.50f);
    hold(&s, &fixture_real_open_palm, 0.0f, 1.0);
    CHECK(s.out.state == GV_IDLE);
    CHECK(!s.ever_armed);
    finish(&s);
}

/* The failure of the first live session: 40 of 69 slides ended because a
 * loosely curled ring or pinky read as extended for a few frames. */
static void ring_and_pinky_opening_up_mid_slide_does_not_release(void)
{
    const gv_frame *tight = &fixture_real_tight_two;
    gv_frame relaxed = aligned_to(&fixture_real_relaxed_two, tight, REAL);
    gv_frame open = aligned_to(&fixture_real_open_palm, tight, REAL);

    session s = start_real(0.50f);
    arm(&s, tight, 0.0f);
    CHECK(s.out.state == GV_ARMED);
    hold(&s, &relaxed, 0.0f, 0.5);
    CHECK(s.out.state == GV_ARMED);
    hold(&s, &open, 0.0f, 0.5);
    CHECK(s.out.state == GV_ARMED);

    /* and the slider still works in that shape */
    slide(&s, &open, 0.0f, 0.9f, 0.5);
    hold(&s, &open, 0.9f, 0.5);
    CHECK(s.out.state == GV_ARMED);
    CHECK_NEAR(s.out.volume, AFTER(0.50f, 0.9f), 0.03);
    finish(&s);
}

static void closing_the_hand_releases_and_keeps_the_volume(void)
{
    const gv_frame *tight = &fixture_real_tight_two;
    gv_frame fist = aligned_to(&fixture_real_fist, tight, REAL);

    session s = start_real(0.30f);
    arm(&s, tight, 0.0f);
    slide(&s, tight, 0.0f, 0.9f, 0.5);
    hold(&s, tight, 0.9f, 0.5);
    float set = s.out.volume;
    CHECK_NEAR(set, AFTER(0.30f, 0.9f), 0.02);

    int before = s.changes;
    hold(&s, &fist, 0.9f, 0.4);
    CHECK(s.out.state == GV_IDLE);
    CHECK_NEAR(s.out.volume, set, 1e-6);
    CHECK(s.changes == before);
    finish(&s);
}

/* --- frames that cannot be true ----------------------------------------- */

/* Landmark models occasionally emit a frame where the hand collapses to a
 * fraction of its size somewhere else. One moved the volume by 0.57. */
static void a_glitch_frame_does_not_move_the_volume(void)
{
    const gv_frame *hand = &fixture_real_tight_two;
    gv_frame shrunk_hand = *hand;
    for (int i = 0; i < GV_NUM_LANDMARKS; i++) { /* 0.3x the size, about its wrist */
        shrunk_hand.lm[i].x = hand->lm[0].x + (hand->lm[i].x - hand->lm[0].x) * 0.3f;
        shrunk_hand.lm[i].y = hand->lm[0].y + (hand->lm[i].y - hand->lm[0].y) * 0.3f;
    }

    session s = start_real(0.50f);
    arm(&s, hand, 0.0f);
    frame(&s, &shrunk_hand, 3.0f);
    CHECK(s.out.state == GV_ARMED);
    CHECK(s.out.changed == 0);
    CHECK_NEAR(s.out.volume, 0.50f, 1e-6);

    hold(&s, hand, 0.0f, 0.3); /* the real hand is still where it was */
    CHECK(s.out.state == GV_ARMED);
    CHECK(s.changes == 0);
    finish(&s);
}

/* Over a slide lasting seconds the hand tilts and drifts towards or away from
 * the camera, so its apparent size wanders well away from what it was at
 * arming. That is not a glitch. In the first recording, gating against the
 * size at arming nearly doubled the number of dropped slides. */
static void a_hand_that_slowly_changes_size_stays_armed(void)
{
    const gv_frame *hand = &fixture_real_tight_two;
    session s = start_real(0.50f);
    arm(&s, hand, 0.0f);

    for (int i = 0; i <= 60; i++) { /* shrinks to 45% over two seconds */
        float k = 1.0f - 0.55f * (float)i / 60.0f;
        gv_frame f = *hand;
        for (int j = 0; j < GV_NUM_LANDMARKS; j++) {
            f.lm[j].x = hand->lm[0].x + (hand->lm[j].x - hand->lm[0].x) * k;
            f.lm[j].y = hand->lm[0].y + (hand->lm[j].y - hand->lm[0].y) * k;
        }
        /* keep the palm centre where it was, so only the size changes */
        float dx = (gv_palm_x(hand, REAL) - gv_palm_x(&f, REAL)) / REAL;
        for (int j = 0; j < GV_NUM_LANDMARKS; j++)
            f.lm[j].x += dx;
        frame(&s, &f, 0.0f);
    }
    CHECK(s.out.state == GV_ARMED);
    CHECK(s.releases == 0); /* armed throughout, not dropped and re-armed */
    finish(&s);
}

static void a_hand_that_teleports_is_not_believed(void)
{
    const gv_frame *hand = &fixture_real_tight_two;
    session s = start_real(0.50f);
    arm(&s, hand, 0.0f);
    frame(&s, hand, 2.0f); /* two hand-lengths in one frame: 60 hand-lengths/s, twice the limit */
    CHECK(s.out.state == GV_ARMED);
    CHECK(s.out.changed == 0);
    frame(&s, hand, 0.0f);
    CHECK(s.changes == 0);
    finish(&s);
}

/* Lost frames must not bank up permission to jump. The hand blurs out for
 * 200 ms and comes back 1.5 hand-lengths away: believable (7.5 hand-lengths/s),
 * so it is followed, but the first step is still a step and not the whole gap. */
static void a_gap_in_tracking_does_not_earn_a_big_jump(void)
{
    const gv_frame *hand = &fixture_real_tight_two;
    gv_config d = gv_default_config();

    session s = start_real(0.20f);
    arm(&s, hand, 0.0f);
    hold(&s, NULL, 0.0f, 0.2);
    float before = s.out.volume;
    frame(&s, hand, 1.5f); /* +0.33 at the pinned gain, if taken in one go */
    CHECK(s.out.state == GV_ARMED);
    CHECK(s.out.volume - before <= d.max_slew * 0.1f + 1e-4f);
    CHECK(s.out.volume - before > 0.0f); /* followed, just not all at once */
    hold(&s, hand, 1.5f, 0.6);
    CHECK_NEAR(s.out.volume, AFTER(0.20f, 1.5f), 0.03);
    finish(&s);
}

/* People do sweep fast: the first recording has 12.5 hand-lengths/s at the
 * 99th percentile and 28 at the 99.9th. Those frames must be followed, not rejected and then caught
 * up in one jump. */
static void a_fast_sweep_is_followed_frame_by_frame(void)
{
    const gv_frame *hand = &fixture_real_tight_two;
    session s = start_real(0.10f);
    arm(&s, hand, 0.0f);
    int before = s.changes;
    slide(&s, hand, 0.0f, 3.5f, 0.15); /* 23 hand-lengths per second, five frames */
    /* (3.5 - LOCK) hand-lengths at the pinned gain is +1.0. Followed means a
     * change on nearly every frame of the sweep once it is under way;
     * rejected-then-caught-up means none during it and one jump afterwards. */
    CHECK(s.changes - before >= 3);
    hold(&s, hand, 3.5f, 0.4);
    CHECK_NEAR(s.out.volume, 1.0f, 1e-6);
    finish(&s);
}

/* num_hands is 1, so when the tracked hand blurs out the detector is free to
 * come back with a different one: the other hand resting on the desk, someone
 * walking past. In the first recording that took the volume from 78% to 0% in
 * one frame. A different hand is a different size and somewhere else, and must
 * end the slide rather than drive it. */
static void tracking_that_jumps_to_a_different_hand_releases(void)
{
    const gv_frame *hand = &fixture_real_tight_two;
    gv_frame other = aligned_to(&fixture_real_open_palm, hand, REAL); /* can stay, cannot arm */
    for (int i = 0; i < GV_NUM_LANDMARKS; i++) { /* 0.65x the size, as it was in the recording */
        other.lm[i].x = other.lm[0].x + (other.lm[i].x - other.lm[0].x) * 0.65f;
        other.lm[i].y = other.lm[0].y + (other.lm[i].y - other.lm[0].y) * 0.65f;
    }

    session s = start_real(0.78f);
    arm(&s, hand, 0.0f);
    frame(&s, NULL, 0.0f);         /* blurred out for a frame, as it was */
    hold(&s, &other, -3.0f, 0.6);  /* three hand-lengths to the left, and it stays there */
    CHECK(s.releases == 1);
    CHECK(s.out.state == GV_IDLE);
    CHECK(s.changes == 0);
    CHECK_NEAR(s.out.volume, 0.78f, 1e-6);
    finish(&s);
}

/* Distance is measured in the plane, not along x alone: a hand cannot jump
 * down the frame any more than across it. */
static void a_jump_that_is_mostly_vertical_is_not_believed(void)
{
    const gv_frame *hand = &fixture_real_tight_two;
    gv_frame moved = *hand;
    float three_hands = 3.0f * gv_hand_scale(hand, REAL);
    for (int i = 0; i < GV_NUM_LANDMARKS; i++)
        moved.lm[i].y -= three_hands;

    session s = start_real(0.50f);
    arm(&s, hand, 0.0f);
    /* 0.8 hand-lengths sideways in a frame is 24/s: believable on its own.
     * With three hand-lengths of vertical jump on top, it is not. */
    frame(&s, &moved, 0.8f);
    CHECK(s.out.changed == 0);
    frame(&s, hand, 0.0f);
    CHECK(s.changes == 0);
    finish(&s);
}

/* The last line of defence, and the one that matters with headphones on: no
 * matter what the detector does, the volume cannot move faster than max_slew.
 * A glitch nobody thought of is then a ramp that the release timer cuts short,
 * not a jump to full volume. */
static void volume_never_moves_faster_than_the_slew_limit(void)
{
    const gv_frame *hand = &fixture_real_tight_two;
    gv_config d = gv_default_config();

    session s = start_real(0.0f);
    arm(&s, hand, 0.0f);
    slide(&s, hand, 0.0f, 3.5f, 0.15); /* asks for +1.0 in 0.15 s */
    hold(&s, hand, 3.5f, 0.2);
    CHECK(s.out.volume > 0.3f && s.out.volume < 1.0f); /* on its way, not there yet */
    hold(&s, hand, 3.5f, 0.6);
    CHECK_NEAR(s.out.volume, 1.0f, d.dead_band);       /* and it does arrive (the filter
                                                        * closes in from below, inside the dead-band) */
    CHECK(s.max_rate <= d.max_slew * 1.01f);
    CHECK(s.max_rate > d.max_slew * 0.5f);             /* the limit was actually reached */
    finish(&s);
}

/* The gate must not lock the slider out for good. If the hand really is
 * somewhere new, the implied speed falls as time passes, and it is believed
 * again well inside the release grace period. */
static void a_genuinely_fast_move_is_accepted_a_moment_later(void)
{
    const gv_frame *hand = &fixture_real_tight_two;
    session s = start_real(0.20f);
    arm(&s, hand, 0.0f);
    hold(&s, hand, 1.5f, 0.6);
    CHECK(s.out.state == GV_ARMED);
    CHECK_NEAR(s.out.volume, AFTER(0.20f, 1.5f), 0.03);
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

static void a_frame_with_a_non_finite_timestamp_is_ignored(void)
{
    session s = start(0.50f);
    arm(&s, &fixture_victory, 0.0f);
    CHECK(s.out.state == GV_ARMED);

    gv_frame bad = fixture_victory;
    bad.t_sec = NAN;
    gv_output o = gv_update(s.ctx, &bad);
    CHECK(o.state == GV_ARMED && o.changed == 0);
    bad.t_sec = INFINITY;
    o = gv_update(s.ctx, &bad);
    CHECK(o.state == GV_ARMED && o.changed == 0);

    /* and the session carries on as if they had never arrived */
    slide(&s, &fixture_victory, 0.0f, 0.9f, 0.5);
    hold(&s, &fixture_victory, 0.9f, 0.5);
    CHECK(s.out.state == GV_ARMED);
    CHECK_NEAR(s.out.volume, AFTER(0.50f, 0.9f), 0.02);
    finish(&s);
}

int main(void)
{
    fprintf(stderr, "test_gesture\n");
    RUN(null_config_gives_working_defaults);
    RUN(nonsense_config_is_rejected);
    RUN(the_output_says_which_gesture_is_active);
    RUN(no_hand_stays_idle);
    RUN(pose_must_be_held_before_arming);
    RUN(a_passing_glimpse_of_the_pose_does_not_arm);
    RUN(arming_itself_does_not_touch_the_volume);
    RUN(sliding_right_raises_the_volume);
    RUN(sliding_left_lowers_the_volume);
    RUN(volume_is_clamped_to_its_range);
    RUN(a_small_final_step_still_reaches_the_end_of_the_range);
    RUN(travel_is_measured_in_hand_lengths_not_pixels);
    RUN(a_wobbling_hand_does_not_chatter);
    RUN(sliding_up_raises_the_volume);
    RUN(sliding_down_lowers_the_volume);
    RUN(nothing_moves_until_an_axis_is_chosen);
    RUN(a_sideways_slide_ignores_the_arc_of_the_arm);
    RUN(an_up_down_slide_ignores_sideways_wander);
    RUN(a_diagonal_does_not_pick_an_axis);
    RUN(each_slide_picks_its_own_axis);
    RUN(a_slow_slide_uses_the_fine_gain);
    RUN(the_same_distance_fast_goes_much_further);
    RUN(gain_rises_smoothly_between_the_two_speeds);
    RUN(out_fast_and_back_slow_is_a_net_change);
    RUN(holding_at_the_right_edge_keeps_raising_the_volume);
    RUN(holding_at_the_left_edge_keeps_lowering_it);
    RUN(the_top_edge_pushes_up_and_the_bottom_edge_down);
    RUN(a_gap_at_the_edge_does_not_earn_a_big_push);
    RUN(an_edge_the_slide_is_not_heading_for_does_nothing);
    RUN(vertical_travel_is_scaled_by_the_aspect_ratio);
    RUN(dropping_the_pose_releases_and_keeps_the_volume);
    RUN(one_lost_frame_does_not_release);
    RUN(a_stall_longer_than_the_grace_period_releases);
    RUN(rearming_starts_from_the_new_position_without_a_jump);
    RUN(external_volume_updates_are_ignored_while_armed);
    RUN(a_relaxed_two_finger_pose_arms);
    RUN(an_open_palm_does_not_arm);
    RUN(ring_and_pinky_opening_up_mid_slide_does_not_release);
    RUN(closing_the_hand_releases_and_keeps_the_volume);
    RUN(a_glitch_frame_does_not_move_the_volume);
    RUN(a_hand_that_slowly_changes_size_stays_armed);
    RUN(a_hand_that_teleports_is_not_believed);
    RUN(tracking_that_jumps_to_a_different_hand_releases);
    RUN(a_jump_that_is_mostly_vertical_is_not_believed);
    RUN(volume_never_moves_faster_than_the_slew_limit);
    RUN(a_gap_in_tracking_does_not_earn_a_big_jump);
    RUN(a_fast_sweep_is_followed_frame_by_frame);
    RUN(a_genuinely_fast_move_is_accepted_a_moment_later);
    RUN(other_hand_shapes_moving_across_the_frame_do_nothing);
    RUN(garbage_landmarks_are_treated_as_no_hand);
    RUN(a_frame_with_a_non_finite_timestamp_is_ignored);
    return TEST_SUMMARY();
}
