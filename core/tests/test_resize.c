/*
 * The resize gesture, end to end through the state machine.
 *
 * The motion is a blend between two REAL frames of the same hand: the open
 * pinch-ready pose and the fully pinched one (aligned to the same place and
 * size). Blending moves every landmark the way a real pinch does; moving only
 * the thumb tip would bend the thumb and look like a different gesture. Going
 * a little past the open frame (k < 0) spreads the fingers beyond it.
 */
#include "test.h"

#include "fixtures.h"
#include "pinch_motion.h"
#include "pose.h"

#define DT   (1.0 / 30.0)
#define REAL FIXTURE_REAL_ASPECT

typedef struct {
    gv_ctx   *ctx;
    double    t;
    gv_output out;
    int       changes;
    int       releases;
    int       crossed;         /* armed in one gesture and, next frame, armed in another */
    float     max_octaves_per_frame;
    float     last_armed_scale;
} session;

static gv_config pinned(void)
{
    gv_config c = gv_default_config();
    c.aspect = REAL;
    c.scale_min = 0.25f;
    c.scale_max = 4.0f;
    c.scale_dead_band = 0.01f;
    c.scale_max_slew = 1.0f; /* octaves per second: the size may at most double in a second */
    return c;
}

static session start_with(gv_config c)
{
    session s = { 0 };
    s.ctx = gv_create(&c);
    s.out.scale = 1.0f;
    gv_set_current_volume(s.ctx, 0.5f);
    return s;
}

static session start(void) { return start_with(pinned()); }

static void finish(session *s) { gv_destroy(s->ctx); s->ctx = NULL; }

static void frame(session *s, const gv_frame *hand)
{
    gv_frame f = { 0 };
    if (hand != NULL)
        f = *hand;
    f.t_sec = s->t;
    gv_output before = s->out;
    s->out = gv_update(s->ctx, &f);
    s->changes += s->out.changed;
    if (before.state == GV_ARMED && s->out.state != GV_ARMED)
        s->releases++;
    if (before.state == GV_ARMED && s->out.state == GV_ARMED && before.kind != s->out.kind)
        s->crossed = 1;
    if (before.state == GV_ARMED && s->out.state == GV_ARMED && before.scale > 0.0f) {
        float octaves = fabsf(log2f(s->out.scale / before.scale));
        if (octaves > s->max_octaves_per_frame)
            s->max_octaves_per_frame = octaves;
    }
    if (s->out.state == GV_ARMED)
        s->last_armed_scale = s->out.scale;
    s->t += DT;
}

static void hold(session *s, const gv_frame *hand, double seconds)
{
    int n = (int)(seconds / DT + 0.5);
    for (int i = 0; i < n; i++)
        frame(s, hand);
}

/* What the scale should be at blend k, armed at k = 0: measured, not assumed linear. */
static float expected_scale(float k)
{
    gv_frame at = pinch(k), open = pinch(0.0f);
    return gv_pinch_gap(&at, REAL) / gv_pinch_gap(&open, REAL);
}

static void ramp(session *s, float from, float to, double seconds)
{
    int n = (int)(seconds / DT + 0.5);
    for (int i = 1; i <= n; i++) {
        gv_frame f = pinch(from + (to - from) * (float)i / (float)n);
        frame(s, &f);
    }
}

static void hold_pinch(session *s, float k, double seconds)
{
    gv_frame f = pinch(k);
    hold(s, &f, seconds);
}

/* --- tests -------------------------------------------------------------- */

static void arming_reports_resize_at_scale_one(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.1);
    CHECK(s.out.state == GV_ARMING && s.out.kind == GV_KIND_RESIZE);
    hold_pinch(&s, 0.0f, 0.4);
    CHECK(s.out.state == GV_ARMED && s.out.kind == GV_KIND_RESIZE);
    CHECK_NEAR(s.out.scale, 1.0f, 1e-3);
    CHECK(s.changes == 0); /* arming alone resizes nothing */
    finish(&s);
}

static void pinching_shrinks_and_spreading_grows(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.5);

    ramp(&s, 0.0f, 0.4f, 1.0);
    hold_pinch(&s, 0.4f, 0.8);
    CHECK(expected_scale(0.4f) < 0.75f); /* the blend really does pinch */
    CHECK_NEAR(s.out.scale, expected_scale(0.4f), 0.03);

    ramp(&s, 0.4f, -0.25f, 1.5);
    hold_pinch(&s, -0.25f, 0.8);
    CHECK(expected_scale(-0.25f) > 1.15f);
    CHECK_NEAR(s.out.scale, expected_scale(-0.25f), 0.04);
    CHECK(s.out.state == GV_ARMED && s.releases == 0);
    CHECK(s.changes > 0);
    finish(&s);
}

/* How far one pinch goes. Near the pinched end the index finger curls, and
 * from there a pinch is indistinguishable from a fist, so the gesture stops
 * following (and lets go) rather than guess. Measured on the real pinch clip:
 * the index reads curled only below a gap of 0.66 hand-lengths, about 0.4x from
 * a typical start. Further than that means re-arming. */
static void a_deep_pinch_freezes_instead_of_collapsing_the_window(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.5);
    ramp(&s, 0.0f, 1.0f, 2.0); /* all the way to touching fingertips (ratio 0.02) */
    hold_pinch(&s, 1.0f, 1.0);
    CHECK(s.out.state == GV_IDLE);
    CHECK(s.last_armed_scale > 0.30f); /* never anywhere near the 0.25 clamp, let alone 0.02 */
    CHECK(s.last_armed_scale < 0.60f); /* but a real pinch did get through */
    finish(&s);
}

static void the_scale_limits_are_clamps(void)
{
    gv_config c = pinned();
    c.scale_min = 0.80f; /* pinned where a pinch reaches them with the index still up */
    c.scale_max = 1.15f;
    session s = start_with(c);
    hold_pinch(&s, 0.0f, 0.5);
    ramp(&s, 0.0f, 0.4f, 1.0);
    hold_pinch(&s, 0.4f, 1.0);
    CHECK(s.out.scale == 0.80f);  /* exactly the clamp, dead-band or not */
    int before = s.changes;
    hold_pinch(&s, 0.4f, 0.5);
    CHECK(s.changes == before);   /* parked at the end: no repeats */

    ramp(&s, 0.4f, -0.25f, 1.5);
    hold_pinch(&s, -0.25f, 1.0);
    CHECK(s.out.scale == 1.15f);
    finish(&s);
}

/* The safety net, as for volume: whatever the detector does, the window cannot
 * snap. One octave per second, and lost frames earn credit for 0.1 s at most. */
static void the_scale_cannot_jump(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.5);
    gv_frame pinched = pinch(0.55f);
    frame(&s, &pinched); /* from open to half the size in ONE frame */
    hold(&s, &pinched, 3.0);
    /* One octave per second is 1/30 of an octave per frame, at EVERY size: the
     * limit is on the ratio. A limit on the difference would let the last
     * frames, at half the size, move twice as many octaves as the first. */
    CHECK(s.max_octaves_per_frame <= 1.0f * (float)DT * 1.05f);
    CHECK(s.max_octaves_per_frame > 0.02f); /* the limit was actually reached */
    CHECK_NEAR(s.out.scale, expected_scale(0.55f), 0.03); /* and it does arrive */
    finish(&s);
}

static void the_scale_cannot_jump_upward_either(void)
{
    session s = start();
    hold_pinch(&s, 0.25f, 0.5); /* armed with the fingers part-way together (the blend's index stops reading extended past 0.25) */
    CHECK(s.out.state == GV_ARMED && s.out.kind == GV_KIND_RESIZE);
    gv_frame spread = pinch(-0.2f);
    frame(&s, &spread); /* to wider than the open hand in ONE frame */
    float just_after = s.out.scale;
    hold(&s, &spread, 3.0);
    CHECK(just_after < 1.05f);                                   /* it did not leap there */
    CHECK(s.max_octaves_per_frame <= 1.0f * (float)DT * 1.05f);
    CHECK(s.max_octaves_per_frame > 0.02f);
    CHECK(s.out.scale > 1.4f);                                   /* and it does arrive */
    finish(&s);
}

static void a_gap_in_tracking_does_not_earn_a_big_jump(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.5);
    hold(&s, NULL, 0.2);
    float before = s.out.scale;
    gv_frame pinched = pinch(0.55f);
    frame(&s, &pinched);
    CHECK(s.out.state == GV_ARMED);
    CHECK(fabsf(log2f(s.out.scale / before)) <= 1.0f * 0.1f + 1e-3f);
    finish(&s);
}

/* Closing the hand is how every gesture here ends, and for this one it is a
 * trap: on the way to a fist the fingertips pass through every smaller gap, so
 * the window shrinks for a few frames before the index reads curled. The
 * gesture therefore ends by going back to the size of a moment (0.3 s) earlier.
 * The last `changed` it reports, on the frame it lets go, carries that size. */
static void closing_the_hand_ends_the_resize_where_it_was(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.5);
    ramp(&s, 0.0f, 0.3f, 0.8);
    hold_pinch(&s, 0.3f, 1.0);
    float set = s.out.scale;
    CHECK_NEAR(set, expected_scale(0.3f), 0.03);

    /* a real closing motion: 0.2 s from the held pinch into the recorded fist */
    gv_frame from = pinch(0.3f);
    gv_frame fist = aligned_to(&fixture_real_fist, &fixture_real_pinch_open);
    float lowest = set, final_scale = set;
    int n = 6;
    for (int i = 1; i <= n + 15; i++) {
        float k = i <= n ? (float)i / (float)n : 1.0f;
        gv_frame f = from;
        for (int j = 0; j < GV_NUM_LANDMARKS; j++) {
            f.lm[j].x = from.lm[j].x + (fist.lm[j].x - from.lm[j].x) * k;
            f.lm[j].y = from.lm[j].y + (fist.lm[j].y - from.lm[j].y) * k;
        }
        frame(&s, &f);
        if (s.out.kind == GV_KIND_RESIZE) {
            if (s.out.scale < lowest) lowest = s.out.scale;
            if (s.out.changed) final_scale = s.out.scale;
        }
    }
    CHECK(s.out.state == GV_IDLE);
    CHECK(lowest < set - 0.01f);          /* the closing motion did drag it down on the way ... */
    CHECK_NEAR(final_scale, set, 0.015);  /* ... and the last word put it back */
    CHECK_NEAR(s.out.scale, 1.0f, 1e-6);  /* idle again: scale reads 1 */
    finish(&s);
}

/* Leaving the frame is different: nothing about the last frames is suspect, so
 * nothing is rolled back and no final change is reported. */
/* The rollback reaches back release_rollback_sec and no further: a size that
 * was set and held for longer than that is what the gesture ends on, not
 * whatever the window was a second ago. */
static void the_rollback_goes_back_a_moment_not_to_the_start(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.35);
    ramp(&s, 0.0f, 0.2f, 0.3); /* 0.3 octaves in 0.3 s: inside the slew limit, so the size keeps up */
    hold_pinch(&s, 0.2f, 0.6); /* under a second since scale 1.0, over 0.3 s settled at the new size */
    float set = s.out.scale;
    CHECK_NEAR(set, expected_scale(0.2f), 0.02);

    float final_scale = set;
    for (int i = 0; i < 15; i++) {
        frame(&s, &fixture_real_fist); /* the hand is a fist at once: nothing drags the size */
        if (s.out.kind == GV_KIND_RESIZE && s.out.changed) final_scale = s.out.scale;
    }
    CHECK(s.out.state == GV_IDLE);
    CHECK_NEAR(final_scale, set, 0.015);
    finish(&s);
}

/* A resize shorter than the rollback ends where it began, and "where it began"
 * is THIS resize, not the one before it. */
static void a_short_resize_rolls_back_to_its_own_start(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.5);
    ramp(&s, 0.0f, 0.3f, 0.8);
    hold_pinch(&s, 0.3f, 0.5);
    CHECK(s.out.scale < 0.85f);
    hold(&s, NULL, 1.0); /* the hand leaves: that size stands */
    CHECK(s.out.state == GV_IDLE);

    hold_pinch(&s, 0.0f, 0.3); /* arms after 0.25 s, then one or two frames armed */
    CHECK(s.out.state == GV_ARMED && s.out.kind == GV_KIND_RESIZE);
    ramp(&s, 0.0f, 0.2f, 0.1);
    float final_scale = -1.0f;
    gv_frame fist = aligned_to(&fixture_real_fist, &fixture_real_pinch_open);
    for (int i = 0; i < 15; i++) {
        frame(&s, &fist);
        if (s.out.kind == GV_KIND_RESIZE && s.out.changed) final_scale = s.out.scale;
    }
    CHECK(s.out.state == GV_IDLE);
    CHECK_NEAR(final_scale, 1.0f, 1e-6);
    finish(&s);
}

static void a_hand_that_just_leaves_keeps_the_last_size(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.5);
    ramp(&s, 0.0f, 0.3f, 0.8);
    float set = s.out.scale; /* still mid-pinch when the hand goes */
    int before = s.changes;
    hold(&s, NULL, 0.5);
    CHECK(s.out.state == GV_IDLE);
    CHECK(s.changes == before);
    CHECK_NEAR(s.last_armed_scale, set, 1e-6);
    finish(&s);
}

static void each_resize_starts_again_from_one(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.5);
    ramp(&s, 0.0f, 0.4f, 0.8);
    hold_pinch(&s, 0.4f, 0.5);
    hold(&s, NULL, 0.5);                  /* hand away: released */
    CHECK(s.out.state == GV_IDLE);

    int before = s.changes;
    hold_pinch(&s, 0.0f, 0.5);            /* re-arm, open */
    CHECK(s.out.state == GV_ARMED);
    CHECK_NEAR(s.out.scale, 1.0f, 1e-3);  /* relative to THIS arming, not the last */
    CHECK(s.changes == before);
    finish(&s);
}

/* With no arm hold at all (a valid setting), a stalled app that comes back to
 * a hand in the arm pose would let go and arm again on ONE frame. Anything
 * watching the state would then see one long gesture where there were two, and
 * the glue would keep resizing the first gesture's window from the second
 * gesture's numbers. So the frame that lets go is never an armed frame. */
static void the_frame_that_lets_go_is_never_an_armed_frame(void)
{
    gv_config c = pinned();
    c.arm_hold_sec = 0.0f;
    session s = start_with(c);
    hold_pinch(&s, 0.0f, 0.3);
    ramp(&s, 0.0f, 0.2f, 0.5);
    CHECK(s.out.state == GV_ARMED && s.out.scale < 0.95f);

    s.t += 1.0;                 /* the app stalls for a second: no frames at all */
    gv_frame open = pinch(0.0f);
    frame(&s, &open);           /* and comes back to the arm pose */
    CHECK(s.out.state != GV_ARMED);
    CHECK(s.releases == 1);
    frame(&s, &open);           /* the next frame may arm, as a new gesture from 1.0 */
    CHECK(s.out.state == GV_ARMED && s.out.kind == GV_KIND_RESIZE);
    CHECK_NEAR(s.out.scale, 1.0f, 1e-6);
    finish(&s);
}

static void a_pinched_start_does_not_arm(void)
{
    session s = start();
    hold_pinch(&s, 1.0f, 1.0);
    CHECK(s.out.state == GV_IDLE);
    hold_pinch(&s, 0.65f, 1.0); /* index up again, but the gap is still under the minimum */
    CHECK(s.out.state == GV_IDLE);
    finish(&s);
}

/* One gesture at a time. Changing hand shape mid-gesture ends the one that was
 * running; it never turns into the other without passing through idle. */
static void volume_and_resize_never_cross(void)
{
    const gv_frame *open = &fixture_real_pinch_open;
    gv_frame two = aligned_to(&fixture_real_tight_two, open);

    session s = start();
    hold(&s, &two, 0.5);
    CHECK(s.out.state == GV_ARMED && s.out.kind == GV_KIND_VOLUME);
    hold(&s, open, 1.0);                  /* the middle finger curls: volume's stay pose ends */
    CHECK(!s.crossed);
    CHECK(s.out.kind == GV_KIND_RESIZE);  /* released, then armed afresh as a resize */
    CHECK(s.releases == 1);

    hold(&s, &two, 1.0);                  /* and back: the middle finger extends, resize ends */
    CHECK(!s.crossed);
    CHECK(s.out.kind == GV_KIND_VOLUME);
    CHECK(s.releases == 2);
    CHECK(s.changes == 0);                /* none of that moved the volume or the window */
    finish(&s);
}

static void a_wobbling_gap_does_not_chatter(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.5);
    ramp(&s, 0.0f, 0.3f, 0.8);
    hold_pinch(&s, 0.3f, 1.0);
    int before = s.changes;
    CHECK(before > 0);
    for (int i = 0; i < 60; i++) {       /* two seconds of +/-0.3% of gap */
        gv_frame f = pinch(0.3f + ((i / 5) % 2 ? 0.002f : -0.002f));
        frame(&s, &f);
    }
    CHECK(s.changes - before <= 1);
    finish(&s);
}

static void nonsense_scale_config_is_rejected(void)
{
    gv_config c;
    c = pinned(); c.scale_min = 0.0f;              CHECK(gv_create(&c) == NULL);
    c = pinned(); c.scale_min = 1.5f;              CHECK(gv_create(&c) == NULL); /* must allow 1.0 */
    c = pinned(); c.scale_max = 0.9f;              CHECK(gv_create(&c) == NULL);
    c = pinned(); c.scale_max = INFINITY;          CHECK(gv_create(&c) == NULL);
    c = pinned(); c.scale_dead_band = -0.1f;       CHECK(gv_create(&c) == NULL);
    c = pinned(); c.scale_max_slew = 0.0f;         CHECK(gv_create(&c) == NULL);
    c = pinned(); c.scale_max_slew = NAN;          CHECK(gv_create(&c) == NULL);
    c = pinned(); c.release_rollback_sec = -0.1f;  CHECK(gv_create(&c) == NULL);
    c = pinned(); c.release_rollback_sec = NAN;    CHECK(gv_create(&c) == NULL);
    c = pinned(); c.release_rollback_sec = 0.0f;   { gv_ctx *ok = gv_create(&c); CHECK(ok != NULL); gv_destroy(ok); } /* off is allowed */
}

int main(void)
{
    fprintf(stderr, "test_resize\n");
    RUN(arming_reports_resize_at_scale_one);
    RUN(pinching_shrinks_and_spreading_grows);
    RUN(a_deep_pinch_freezes_instead_of_collapsing_the_window);
    RUN(the_scale_limits_are_clamps);
    RUN(the_scale_cannot_jump);
    RUN(the_scale_cannot_jump_upward_either);
    RUN(a_gap_in_tracking_does_not_earn_a_big_jump);
    RUN(closing_the_hand_ends_the_resize_where_it_was);
    RUN(the_rollback_goes_back_a_moment_not_to_the_start);
    RUN(a_short_resize_rolls_back_to_its_own_start);
    RUN(a_hand_that_just_leaves_keeps_the_last_size);
    RUN(each_resize_starts_again_from_one);
    RUN(the_frame_that_lets_go_is_never_an_armed_frame);
    RUN(a_pinched_start_does_not_arm);
    RUN(volume_and_resize_never_cross);
    RUN(a_wobbling_gap_does_not_chatter);
    RUN(nonsense_scale_config_is_rejected);
    return TEST_SUMMARY();
}
