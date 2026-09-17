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

/* The tests pin the gain. Expected volumes below are arithmetic on this value,
 * and the shipped default is a tuning choice that must be free to change. */
#define GAIN (1.0f / 3.0f)

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

/* One frame. hand = NULL means "no hand in view". offset is how far the hand
 * has been slid from its position in the photo, in hand-lengths (+ = right). */
static void frame(session *s, const gv_frame *hand, float offset)
{
    gv_frame f = { 0 };
    if (hand != NULL) {
        f = *hand;
        float dx = offset * gv_hand_scale(hand, s->aspect) / s->aspect;
        for (int i = 0; i < GV_NUM_LANDMARKS; i++)
            f.lm[i].x += dx;
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
    c = gv_default_config(); c.gate_scale_min = 1.1f;      CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.gate_scale_max = 0.9f;      CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.gate_max_speed = 0.0f;      CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.max_slew = 0.0f;            CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.max_slew = NAN;             CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.arm_hold_sec = NAN;         CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.arm_hold_sec = -0.1f;       CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.release_grace_sec = -1.0f;  CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.gain = INFINITY;            CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.dead_band = -0.01f;         CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.filter_min_cutoff = 0.0f;   CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.filter_d_cutoff = 0.0f;     CHECK(gv_create(&c) == NULL);
    c = gv_default_config(); c.filter_beta = -1.0f;        CHECK(gv_create(&c) == NULL);

    /* a negative gain is legitimate: it reverses the slider's direction */
    c = gv_default_config(); c.gain = -c.gain;
    gv_ctx *ctx = gv_create(&c);
    CHECK(ctx != NULL);
    gv_destroy(ctx);
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

/* The last step to an end of the range can be smaller than the dead-band. It
 * must still be reported, or the slider can never reach silence or full. */
static void a_small_final_step_still_reaches_the_end_of_the_range(void)
{
    session s = start(0.50f);
    arm(&s, &fixture_victory, 0.0f);
    /* 1.485 hand-lengths * 1/3 = +0.495: parks the volume just short of 1.0 */
    slide(&s, &fixture_victory, 0.0f, 1.485f, 0.6);
    hold(&s, &fixture_victory, 1.485f, 0.6);
    CHECK(s.out.volume > 0.98f && s.out.volume < 1.0f);

    slide(&s, &fixture_victory, 1.485f, 1.6f, 0.2); /* target clamps to 1.0 */
    hold(&s, &fixture_victory, 1.6f, 0.5);
    CHECK(s.out.volume == 1.0f);

    int before = s.changes;
    hold(&s, &fixture_victory, 1.6f, 0.5); /* parked at the end: no repeats */
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
    CHECK_NEAR(s.out.volume, 0.80f, 0.03);
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
    CHECK_NEAR(set, 0.60f, 0.02);

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
    frame(&s, hand, 1.5f); /* +0.5 at the pinned gain, if taken in one go */
    CHECK(s.out.state == GV_ARMED);
    CHECK(s.out.volume - before <= d.max_slew * 0.1f + 1e-4f);
    CHECK(s.out.volume - before > 0.0f); /* followed, just not all at once */
    hold(&s, hand, 1.5f, 0.6);
    CHECK_NEAR(s.out.volume, 0.70f, 0.03);
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
    slide(&s, hand, 0.0f, 3.0f, 0.15); /* 20 hand-lengths per second, five frames */
    /* 3 hand-lengths at the pinned gain is +1.0. Followed means a change on
     * nearly every frame of the sweep; rejected-then-caught-up means none
     * during it and one jump afterwards. */
    CHECK(s.changes - before >= 4);
    hold(&s, hand, 3.0f, 0.4);
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
    slide(&s, hand, 0.0f, 3.0f, 0.15); /* asks for +1.0 in 0.15 s */
    hold(&s, hand, 3.0f, 0.2);
    CHECK(s.out.volume > 0.3f && s.out.volume < 1.0f); /* on its way, not there yet */
    hold(&s, hand, 3.0f, 0.6);
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
    CHECK_NEAR(s.out.volume, 0.70f, 0.03);
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
    CHECK_NEAR(s.out.volume, 0.80f, 0.02);
    finish(&s);
}

int main(void)
{
    fprintf(stderr, "test_gesture\n");
    RUN(null_config_gives_working_defaults);
    RUN(nonsense_config_is_rejected);
    RUN(no_hand_stays_idle);
    RUN(pose_must_be_held_before_arming);
    RUN(a_passing_glimpse_of_the_pose_does_not_arm);
    RUN(arming_itself_does_not_touch_the_volume);
    RUN(sliding_right_raises_the_volume);
    RUN(sliding_left_lowers_the_volume);
    RUN(volume_is_clamped_to_its_range);
    RUN(a_small_final_step_still_reaches_the_end_of_the_range);
    RUN(travel_is_measured_in_hand_lengths_not_pixels);
    RUN(movement_inside_the_dead_band_is_not_reported);
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
