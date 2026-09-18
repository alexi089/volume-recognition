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
    c.resize_rate_max = 2.0f;  /* octaves per second: the size may at most quadruple in a second */
    c.resize_dead_zone = 0.08f;
    c.resize_rate_gain = 4.5f;
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

/* The blend value `k` that puts the fingertip gap at a given OFFSET relative
 * to the gap at some other blend `k0` (not necessarily the arming gap, though
 * it usually is one): bisection on gv_pinch_gap, since the mapping from k to
 * gap is measured, not a formula. The gap falls as k rises, so the search
 * narrows the same way every time. See k_for_offset_from_hits_the_asked_offset
 * for a direct check that this actually lands where asked. */
static float k_for_offset_from(float k0, float offset)
{
    gv_frame base_frame = pinch(k0);
    float base = gv_pinch_gap(&base_frame, REAL);
    float lo = -0.4f, hi = 0.6f;
    for (int i = 0; i < 40; i++) {
        float mid = (lo + hi) / 2.0f;
        gv_frame f = pinch(mid);
        if (gv_pinch_gap(&f, REAL) / base - 1.0f > offset) lo = mid; else hi = mid;
    }
    return (lo + hi) / 2.0f;
}

/* The common case: an offset relative to the gap at k = 0, the arming gap of
 * every test that arms with hold_pinch(s, 0.0f, ...). */
static float k_for_offset(float offset) { return k_for_offset_from(0.0f, offset); }

static void hold_offset(session *s, float offset, double seconds)
{
    hold_pinch(s, k_for_offset(offset), seconds);
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

/* Every test below leans on k_for_offset(_from) to reach a wanted offset from
 * the arming gap. If the bisection itself were wrong, they would all fail in a
 * confusing way that looked like the rate law was wrong instead. So it gets
 * one direct check of its own, against a non-zero, non-trivial base (k0 =
 * 0.25, the same partly-pinched arming point wider_grows_without_pinching_first
 * uses), asserting the offset it actually produces is within 0.005 of asked. */
static void k_for_offset_from_hits_the_asked_offset(void)
{
    float k0 = 0.25f, offset = 0.30f;
    float k = k_for_offset_from(k0, offset);
    gv_frame base_frame = pinch(k0), at = pinch(k);
    float found = gv_pinch_gap(&at, REAL) / gv_pinch_gap(&base_frame, REAL) - 1.0f;
    CHECK_NEAR(found, offset, 0.005);
}

static void inside_the_dead_zone_nothing_moves(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.5);            /* armed at 1.0 */
    hold_offset(&s, +0.05f, 1.0);         /* 5% wider: inside the 8% dead zone */
    CHECK(s.out.state == GV_ARMED);
    CHECK(s.changes == 0);
    CHECK_NEAR(s.out.scale, 1.0f, 1e-6);
    hold_offset(&s, -0.05f, 1.0);
    CHECK(s.changes == 0);
    finish(&s);
}

/* offset 0.30: rate = 4.5 * (0.30 - 0.08) = 0.99 octaves/s: one second later the
 * scale is 2^0.99 = 1.99 (wider) or 0.50 (closer). */
static void a_steady_offset_moves_the_scale_at_the_formulas_rate(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.5);
    hold_offset(&s, -0.30f, 1.0);
    CHECK_NEAR(s.out.scale, exp2f(-0.99f), 0.04);   /* the filter settles over the first frames */
    CHECK(s.changes > 20);                          /* a change most frames, not one jump */
    finish(&s);
}

/* The point of rate control: growing needs no pinch first. Arming with the
 * fingers already part-way together and then only widening reaches a scale
 * well above 1, which the old ratio-of-gaps arithmetic could never do (it
 * would have to pass back through the arming gap first). */
static void wider_grows_without_pinching_first(void)
{
    session s = start();
    hold_pinch(&s, 0.25f, 0.5);                     /* armed with the fingers part-way together */
    hold_pinch(&s, k_for_offset_from(0.25f, 0.30f), 1.0); /* offset relative to THIS arming gap */
    CHECK(s.out.scale > 1.8f);
    finish(&s);
}

static void returning_to_neutral_stops_it(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.5);
    hold_offset(&s, -0.30f, 0.5);
    float held = s.out.scale;
    CHECK(held < 0.8f);
    int before = s.changes;
    hold_pinch(&s, 0.0f, 1.0);            /* back to the arming gap */
    CHECK(s.changes - before <= 3);       /* the filter's settling, then nothing */
    CHECK_NEAR(s.out.scale, held, 0.05);
    finish(&s);
}

static void the_rate_is_capped(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.5);
    hold_offset(&s, -0.60f, 1.0);         /* formula says 2.34 octaves/s; the cap is 2.0 */
    CHECK_NEAR(s.out.scale, exp2f(-2.0f), 0.06);
    CHECK(s.max_octaves_per_frame <= 2.0f * (float)DT * 1.05f);
    finish(&s);
}

/* scale_min/scale_max are a hard floor and ceiling on the INTEGRATOR
 * (scale_target), not just on what gets reported: holding the fastest shrink
 * rate for long enough to pass the floor several times over must still park
 * exactly at scale_min, not at whatever the unclamped integral would be. */
static void the_scale_floor_is_a_hard_clamp(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.5);
    hold_offset(&s, -0.60f, 3.0); /* saturates the rate at -2.0 octaves/s: 2^-6 unclamped, 3 s in */
    CHECK_NEAR(s.out.scale, 0.25f, 0.02f); /* pinned scale_min: the floor holds it there */
    finish(&s);
}

/* The credit cap (GV_SLEW_MAX_CREDIT_SEC, 0.1 s) must not punish a merely slow
 * camera loop: as long as no single gap between frames exceeds it, the same
 * total elapsed time reaches the same scale, whether it arrived in many small
 * frames or a few larger ones. Past the cap, each frame earns at most 0.1 s of
 * movement, however much real time actually passed (up to the release grace,
 * beyond which the gesture lets go instead). */
static void credit_across_a_slow_frame_is_capped(void)
{
    gv_frame held = pinch(k_for_offset(-0.30f));

    /* Settle the gap filter onto the held offset identically for both sessions
     * first (same calls, same cadence), so the comparison below measures only
     * what the credit cap does to the following 0.9 s, not a filter transient
     * that a different sampling cadence would settle differently. */
    session fast = start();
    hold_pinch(&fast, 0.0f, 0.5);
    hold_offset(&fast, -0.30f, 0.5);
    float base_fast = fast.out.scale;
    /* frame() always advances s.t by the fixed DT after processing, so to land
     * each frame exactly 0.09 s after the last one the timestamp is set to an
     * absolute value, not added to on top of that automatic step. */
    double t = fast.t;
    for (int i = 0; i < 10; i++) {
        t += 0.09;
        fast.t = t;
        frame(&fast, &held); /* 0.9 s total, each gap under the 0.1 s cap */
    }

    session normal = start();
    hold_pinch(&normal, 0.0f, 0.5);
    hold_offset(&normal, -0.30f, 0.5);
    float base_normal = normal.out.scale;
    hold_offset(&normal, -0.30f, 0.9);    /* the same 0.9 s at the usual frame rate */

    float grown_fast = log2f(fast.out.scale / base_fast);
    float grown_normal = log2f(normal.out.scale / base_normal);
    CHECK_NEAR(grown_fast, grown_normal, 0.03f * fabsf(grown_normal));
    finish(&fast);
    finish(&normal);

    session capped = start();
    hold_pinch(&capped, 0.0f, 0.5);
    hold_offset(&capped, -0.30f, 0.3);    /* get the rate going first */
    capped.t += 0.2 - DT;                 /* net +0.2 s over frame()'s own +DT: a stall past the
                                            * 0.1 s cap, under the 0.25 s release grace */
    frame(&capped, &held);
    float rate = 4.5f * (0.30f - 0.08f);  /* the formula for THIS offset; well under the 2.0 cap */
    CHECK(capped.max_octaves_per_frame <= rate * 0.1f * 1.05f);
    CHECK(capped.max_octaves_per_frame > rate * 0.1f * 0.5f); /* the credit really was spent, not rounded to nothing */
    finish(&capped);
}

/* How far one pinch goes. Near the pinched end the index finger curls, and
 * from there a pinch is indistinguishable from a fist, so the gesture stops
 * following (and lets go) rather than guess. Measured on the real pinch clip:
 * the index reads curled only below a gap of 0.66 hand-lengths, about 0.4x from
 * a typical start. Further than that means re-arming. Under rate control the
 * scale reached by then is a matter of how long the offset had been beyond the
 * dead zone before the pose gave out, not a direct reading of the gap ratio;
 * these bounds are measured on the actual run, not derived from the old formula. */
static void a_deep_pinch_freezes_instead_of_collapsing_the_window(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.5);
    ramp(&s, 0.0f, 1.0f, 2.0); /* all the way to touching fingertips (ratio 0.02) */
    hold_pinch(&s, 1.0f, 1.0);
    CHECK(s.out.state == GV_IDLE);
    /* Measured on this run: 0.42. Under rate control this is a matter of how
     * long the offset sat beyond the dead zone before the pose gave out, not
     * a direct reading of the gap ratio, so the bound is measured, not derived
     * from the old formula; it still checks the same thing the old bounds did
     * (nowhere near the 0.25 clamp, but a real, substantial pinch got through). */
    CHECK(s.last_armed_scale > 0.35f);
    CHECK(s.last_armed_scale < 0.50f);
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
    hold_offset(&s, -0.30f, 0.2);  /* shrink by rate ... */
    hold_pinch(&s, 0.0f, 0.5);     /* ... then back to neutral: the size holds still */
    float set = s.out.scale;
    CHECK(set < 0.95f);

    /* a real closing motion: 0.2 s from the held neutral pose into the recorded fist */
    gv_frame from = pinch(0.0f);
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

/* The same closing motion as closing_the_hand_ends_the_resize_where_it_was,
 * after shrinking by rate to about 0.7 and holding still (neutral) for 0.6 s:
 * the rollback must still find that held size, not something dragged down by
 * the closing motion itself. */
static void a_fist_still_rolls_back(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.5);
    hold_offset(&s, -0.30f, 0.5);  /* shrink by rate to about 0.7 ... */
    hold_pinch(&s, 0.0f, 0.6);     /* ... then hold still, neutral, for 0.6 s */
    float set = s.out.scale;
    CHECK(set < 0.85f);
    CHECK(set > 0.55f);

    gv_frame from = pinch(0.0f);
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
    CHECK(lowest < set - 0.01f);
    CHECK_NEAR(final_scale, set, 0.015);
    CHECK_NEAR(s.out.scale, 1.0f, 1e-6);
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
    hold_offset(&s, -0.30f, 0.3);  /* shrink by rate ... */
    hold_pinch(&s, 0.0f, 0.6);     /* ... then held neutral, well over the 0.3 s rollback window */
    float set = s.out.scale;
    CHECK(set < 0.9f);

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
 * is THIS resize, not the one before it. Under rate control a resize's size
 * only moves as fast as time lets it, so the second (short) gesture below has
 * to hold a clear offset, not just brush past one, to move far enough from
 * 1.0 to make "it rolled back to ITS start, not the last gesture's" a real
 * test rather than one where nothing moved either way. */
static void a_short_resize_rolls_back_to_its_own_start(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.5);
    hold_offset(&s, -0.30f, 0.5);
    CHECK(s.out.scale < 0.85f);
    hold(&s, NULL, 1.0); /* the hand leaves: that size stands */
    CHECK(s.out.state == GV_IDLE);

    hold_pinch(&s, 0.0f, 0.3); /* arms after 0.25 s, then one or two frames armed */
    CHECK(s.out.state == GV_ARMED && s.out.kind == GV_KIND_RESIZE);
    hold_offset(&s, -0.30f, 0.15); /* this SECOND gesture moves, visibly, before it is cut short */
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
    hold_offset(&s, -0.30f, 0.5);
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

static void nonsense_scale_config_is_rejected(void)
{
    gv_config c;
    c = pinned(); c.scale_min = 0.0f;              CHECK(gv_create(&c) == NULL);
    c = pinned(); c.scale_min = 1.5f;              CHECK(gv_create(&c) == NULL); /* must allow 1.0 */
    c = pinned(); c.scale_max = 0.9f;              CHECK(gv_create(&c) == NULL);
    c = pinned(); c.scale_max = INFINITY;          CHECK(gv_create(&c) == NULL);
    c = pinned(); c.scale_dead_band = -0.1f;       CHECK(gv_create(&c) == NULL);
    c = pinned(); c.resize_rate_max = 0.0f;        CHECK(gv_create(&c) == NULL);
    c = pinned(); c.resize_rate_max = NAN;         CHECK(gv_create(&c) == NULL);
    c = pinned(); c.resize_dead_zone = 1.0f;       CHECK(gv_create(&c) == NULL); /* must stay under 1 */
    c = pinned(); c.resize_rate_gain = -1.0f;      CHECK(gv_create(&c) == NULL);
    c = pinned(); c.release_rollback_sec = -0.1f;  CHECK(gv_create(&c) == NULL);
    c = pinned(); c.release_rollback_sec = NAN;    CHECK(gv_create(&c) == NULL);
    c = pinned(); c.release_rollback_sec = 0.0f;   { gv_ctx *ok = gv_create(&c); CHECK(ok != NULL); gv_destroy(ok); } /* off is allowed */
}

int main(void)
{
    fprintf(stderr, "test_resize\n");
    RUN(arming_reports_resize_at_scale_one);
    RUN(k_for_offset_from_hits_the_asked_offset);
    RUN(inside_the_dead_zone_nothing_moves);
    RUN(a_steady_offset_moves_the_scale_at_the_formulas_rate);
    RUN(wider_grows_without_pinching_first);
    RUN(returning_to_neutral_stops_it);
    RUN(the_rate_is_capped);
    RUN(the_scale_floor_is_a_hard_clamp);
    RUN(credit_across_a_slow_frame_is_capped);
    RUN(a_deep_pinch_freezes_instead_of_collapsing_the_window);
    RUN(closing_the_hand_ends_the_resize_where_it_was);
    RUN(a_fist_still_rolls_back);
    RUN(the_rollback_goes_back_a_moment_not_to_the_start);
    RUN(a_short_resize_rolls_back_to_its_own_start);
    RUN(a_hand_that_just_leaves_keeps_the_last_size);
    RUN(each_resize_starts_again_from_one);
    RUN(the_frame_that_lets_go_is_never_an_armed_frame);
    RUN(a_pinched_start_does_not_arm);
    RUN(volume_and_resize_never_cross);
    RUN(nonsense_scale_config_is_rejected);
    return TEST_SUMMARY();
}
