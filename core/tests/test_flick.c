/*
 * The flick: an open palm swiped down, the one-shot gesture that minimises the
 * front window. It never arms, so there is no state to watch; what a test
 * counts is FIRINGS - frames where the output says GV_KIND_MINIMIZE with
 * `changed` set.
 *
 * The hand is real (MediaPipe landmarks from the fixture photos); the motion is
 * synthetic: a fixture is slid by a number of hand-lengths and fed in at 30 fps.
 * Unlike test_gesture, which slides sideways, everything here is about falling,
 * so `dy` is measured DOWN (the direction image y already grows in).
 *
 * The gesture is in two parts: an open palm HELD nearly still, and then a DROP.
 * The distances and durations below are arithmetic on the SHIPPED defaults
 * (the hold: flick_hold_sec 0.25 s of open palm moving slower than
 * flick_still_speed 1.0 hand-lengths per second; the drop: flick_drop 0.8
 * hand-lengths below where it was held, inside flick_drop_window_sec 0.6 s,
 * sideways at most flick_max_sideways 0.5 of the drop, no landmark past
 * 1 - flick_edge_margin 0.1 of the frame height and not a fist at the end;
 * flick_refractory_sec 1.5 after any release and after the previous flick).
 * That is deliberate: this gesture's whole risk is firing when nobody asked, so
 * the tests exercise the numbers that actually ship. If a default is retuned,
 * the motions here move with it.
 *
 * Most tests hold for 0.4 s rather than the 0.25 s minimum. At 30 fps a 0.25 s
 * hold is eight frames with no margin at all, and a test that fails because it
 * is one frame short of a hold is a test about rounding, not about the rule.
 *
 * WHERE THE HAND STARTS. The fixture photo has the hand low in the picture -
 * its lowest landmark sits at 0.82 of the frame height - so a two hand-length
 * fall from there ends outside the frame entirely. Every session below
 * therefore begins with the hand raised (see `start_with`), and the tests that
 * are about the edge move it back down on purpose. Without that, every test
 * here would silently be a test of the edge veto.
 */
#include "test.h"

#include "fixtures.h"
#include "palm_motion.h" /* hand_length, moved: shared with test_step.c */
#include "pose.h"

#define DT     (1.0 / 30.0)
#define REAL   FIXTURE_REAL_ASPECT /* the fixture frames from the live recording are 16:9 */
#define START_VOLUME 0.5f

typedef struct {
    gv_ctx   *ctx;
    double    t;
    gv_output out;            /* result of the most recent frame */
    gv_output at_flick;       /* ... and of the last frame that fired a flick */
    int       flicks;         /* frames that reported a minimise */
    int       volume_changes;
    int       busy_frames;    /* frames where some gesture was arming or armed */
    int       armed_frames;
    /* Where the hand has been moved to, in hand-lengths: right is +dx, DOWN is
     * +dy. Kept in the session so a motion can carry on from where the last one
     * left off (raise the hand, flick again), which the multi-flick and
     * lost-tracking tests need. */
    float     dx, dy;
} session;

/* Two and a half hand-lengths up from where the fixture photo puts the hand:
 * far enough that a two hand-length fall ends at 0.72 of the frame height,
 * clear of the edge veto's 0.9, and not so far that the hand leaves the top. */
#define START_DY (-2.5f)

static session start_with(gv_config cfg)
{
    session s = { 0 };
    s.ctx = gv_create(&cfg);
    s.out.scale = 1.0f;
    s.dy = START_DY;
    gv_set_current_volume(s.ctx, START_VOLUME);
    return s;
}

static gv_config shipped(void)
{
    gv_config cfg = gv_default_config();
    cfg.aspect = REAL;
    return cfg;
}

static session start(void) { return start_with(shipped()); }

static void finish(session *s) { gv_destroy(s->ctx); s->ctx = NULL; }

static void frame(session *s, const gv_frame *hand)
{
    gv_frame f = { 0 };
    if (hand != NULL)
        f = *hand;
    f.t_sec = s->t;
    s->out = gv_update(s->ctx, &f);
    if (s->out.kind == GV_KIND_MINIMIZE && s->out.changed) {
        s->flicks++;
        s->at_flick = s->out;
    }
    if (s->out.changed && s->out.kind == GV_KIND_VOLUME)
        s->volume_changes++;
    s->busy_frames += (s->out.state != GV_IDLE);
    s->armed_frames += (s->out.state == GV_ARMED);
    s->t += DT;
}

/* Every offset below is in hand-lengths of the open palm, in frame heights:
 * `hand_length` and `moved` (palm_motion.h) do that arithmetic, and test_step.c
 * drives the same motion through the glue from the same two functions. */

/* No hand in view. */
static void vanish(session *s, double seconds)
{
    int n = (int)(seconds / DT + 0.5);
    for (int i = 0; i < n; i++)
        frame(s, NULL);
}

/* The hand sits where it is. */
static void still(session *s, const gv_frame *hand, double seconds)
{
    gv_frame f = moved(hand, s->dx, s->dy);
    int n = (int)(seconds / DT + 0.5);
    for (int i = 0; i < n; i++)
        frame(s, &f);
}

/* The hand travels dx_hands sideways and dy_hands DOWN from where it is now,
 * in a straight line at a steady speed, over `seconds`. */
static void travel(session *s, const gv_frame *hand, float dx_hands, float dy_hands, double seconds)
{
    float x0 = s->dx, y0 = s->dy;
    int n = (int)(seconds / DT + 0.5);
    for (int i = 1; i <= n; i++) {
        float k = (float)i / (float)n;
        gv_frame f = moved(hand, x0 + dx_hands * k, y0 + dy_hands * k);
        frame(s, &f);
    }
    s->dx = x0 + dx_hands;
    s->dy = y0 + dy_hands;
}

/* The gesture itself: the OPEN PALM travelling. */
static void sweep(session *s, float dx_hands, float dy_hands, double seconds)
{
    travel(s, &fixture_real_open_palm, dx_hands, dy_hands, seconds);
}

/* Put the hand somewhere without showing any of the journey: where it starts,
 * or where tracking picks it up again after losing it. Offsets are from the
 * fixture photo's own position, so 0 is the hand low in the picture. */
static void hand_at(session *s, float dx, float dy) { s->dx = dx; s->dy = dy; }

/* The fixture, resized about its palm centre by `k`. The palm centre stays
 * where it was, so this changes the hand's apparent SIZE and nothing else -
 * what a hand moving towards or away from the camera does. */
static gv_frame resized(const gv_frame *hand, float k)
{
    gv_frame f = *hand;
    float cx = gv_palm_x(hand, REAL) / REAL, cy = gv_palm_y(hand); /* palm_x carries the aspect */
    for (int i = 0; i < GV_NUM_LANDMARKS; i++) {
        f.lm[i].x = cx + (hand->lm[i].x - cx) * k;
        f.lm[i].y = cy + (hand->lm[i].y - cy) * k;
    }
    return f;
}

/* The fist fixture, moved so that its palm centre sits exactly where the open
 * palm's does. Going from one to the other is then a change of SHAPE and not a
 * jump across the frame, which is what a test about the pose at each end of the
 * fall needs. (The two hands' apparent sizes differ by 6%; nothing here
 * depends on that.) */
static gv_frame fist_where_the_palm_is(void)
{
    gv_frame f = fixture_real_fist;
    float dx = (gv_palm_x(&fixture_real_open_palm, REAL) - gv_palm_x(&f, REAL)) / REAL;
    float dy = gv_palm_y(&fixture_real_open_palm) - gv_palm_y(&f);
    for (int i = 0; i < GV_NUM_LANDMARKS; i++) {
        f.lm[i].x += dx;
        f.lm[i].y += dy;
    }
    return f;
}

/* The open palm held still, then dropping `dy_hands` in a quarter of a second
 * while its apparent size is multiplied by `k` across that drop: what happens
 * when the drop also carries the hand away from the camera (k < 1) or towards
 * it (k > 1). The size is left alone during the hold, so the drop is measured
 * in hand-lengths of the hand as it was held. */
static void fall_while_resizing(session *s, float k, float dy_hands)
{
    /* The lead is the HOLD: the hand is motionless and at one size through it,
     * so it is where the drop is measured from. */
    int lead = (int)(0.4 / DT + 0.5), fall = (int)(0.25 / DT + 0.5), tail = (int)(0.3 / DT + 0.5);
    for (int i = 0; i < lead + fall + tail; i++) {
        float done = i < lead ? 0.0f
                   : (i < lead + fall ? (float)(i - lead + 1) / (float)fall : 1.0f);
        gv_frame sized = resized(&fixture_real_open_palm, 1.0f + (k - 1.0f) * done);
        gv_frame f = moved(&sized, s->dx, s->dy + dy_hands * done);
        frame(s, &f);
    }
    s->dy += dy_hands;
}

/* The open palm blurred part of the way towards a fist: every landmark moved a
 * fraction `k` from the open-palm fixture to the fist (shifted first so the two
 * hands' palm centres coincide, so only the FINGERS change). At a quarter of
 * the way the hand is neither confidently open nor confidently closed, which is
 * what the end of a fast swipe looks like: motion blur costs the fingers their
 * "long AND straight" reading. The tests assert that rather than trust it. */
static gv_frame blurred(float k)
{
    gv_frame fist = fist_where_the_palm_is();
    gv_frame f = fixture_real_open_palm;
    for (int i = 0; i < GV_NUM_LANDMARKS; i++) {
        f.lm[i].x += (fist.lm[i].x - f.lm[i].x) * k;
        f.lm[i].y += (fist.lm[i].y - f.lm[i].y) * k;
        f.lm[i].z += (fist.lm[i].z - f.lm[i].z) * k;
    }
    return f;
}

/* --- the gesture: hold, then drop --------------------------------------- */

static void a_held_open_palm_then_a_drop_fires_once(void)
{
    session s = start();
    still(&s, &fixture_real_open_palm, 0.4);   /* held: the half of the gesture that says "I mean it" */
    sweep(&s, 0.0f, 1.2f, 0.3);                /* dropped: 1.2 hand-lengths, well over the 0.8 */
    still(&s, &fixture_real_open_palm, 0.5);   /* and rests where it landed */

    CHECK(s.flicks == 1);                      /* once, not once per frame of the drop */
    CHECK(s.at_flick.state == GV_IDLE);        /* a flick never arms ... */
    CHECK(s.busy_frames == 0);                 /* ... nor does the palm arm anything else */
    CHECK(s.at_flick.changed == 1);
    CHECK_NEAR(s.at_flick.scale, 1.0f, 1e-6);  /* it is not a resize */
    CHECK_NEAR(s.at_flick.volume, START_VOLUME, 1e-6);
    CHECK(s.volume_changes == 0);
    finish(&s);
}

static void a_drop_with_no_hold_before_it_does_not_fire(void)
{
    /* THE REASON THIS GESTURE HAS TWO HALVES. Until this round the fall alone
     * WAS the gesture, and it could not be told apart from a hand being put
     * down: measured on a live session, the owner's own swipes covered 0.78 -
     * 1.07 hand-lengths, and so does ordinary hand-lowering. Here the hand
     * arrives already moving and falls two hand-lengths at speed - more than
     * the old rule ever asked for - and nothing happens, because it was never
     * held. */
    session s = start();
    sweep(&s, 0.0f, 2.0f, 0.25);
    still(&s, &fixture_real_open_palm, 0.5);
    CHECK(s.flicks == 0);
    finish(&s);
}

static void a_hand_lowered_slowly_instead_of_held_does_not_fire(void)
{
    /* The hand drifts down at 2 hand-lengths a second - twice the still limit -
     * for 0.4 s and then drops. Drifting is not holding: no frame of the drift
     * counts towards a hold, so the drop has nothing to be measured from. This
     * is the shape of a hand being lowered and then dropped, which the
     * recordings are full of. */
    session s = start();
    travel(&s, &fixture_real_open_palm, 0.0f, 0.8f, 0.4);
    sweep(&s, 0.0f, 1.2f, 0.3);
    still(&s, &fixture_real_open_palm, 0.5);
    CHECK(s.flicks == 0);
    finish(&s);
}

static void a_drop_slower_than_the_drop_window_does_not_fire(void)
{
    /* Held, then lowered at 1.1 hand-lengths a second: fast enough to end the
     * hold (the limit is 1.0) and slow enough that 0.6 s later - when the held
     * frames leave the drop window - the hand has only come down 0.66, short of
     * the 0.8. The band between those two limits is narrow by construction: a
     * drop slower than 1.0 goes on counting as a hold and the reference follows
     * the hand down, and one faster than 1.33 covers the 0.8 inside the window.
     * That is the arithmetic being pinned here, not a round number. */
    session s = start();
    still(&s, &fixture_real_open_palm, 0.4);
    travel(&s, &fixture_real_open_palm, 0.0f, 1.32f, 1.2);
    still(&s, &fixture_real_open_palm, 0.5);
    CHECK(s.flicks == 0);
    finish(&s);
}

static void a_drop_that_is_not_far_enough_does_not_fire(void)
{
    session s = start();
    still(&s, &fixture_real_open_palm, 0.4);
    sweep(&s, 0.0f, 0.5f, 0.2);   /* fast, but half a hand-length */
    still(&s, &fixture_real_open_palm, 0.5);
    CHECK(s.flicks == 0);
    finish(&s);
}

static void sideways_after_a_hold_does_not_fire(void)
{
    /* Diagonal: far enough down and fast enough, but just as far across, which
     * is a wave, not a drop. */
    session s = start();
    still(&s, &fixture_real_open_palm, 0.4);
    sweep(&s, 1.2f, 1.2f, 0.3);
    still(&s, &fixture_real_open_palm, 0.4);
    CHECK(s.flicks == 0);
    finish(&s);

    /* And straight across, which has no drop at all. */
    session t = start();
    still(&t, &fixture_real_open_palm, 0.4);
    sweep(&t, 1.2f, 0.0f, 0.3);
    still(&t, &fixture_real_open_palm, 0.4);
    CHECK(t.flicks == 0);
    finish(&t);
}

static void upward_after_a_hold_does_not_fire(void)
{
    session s = start();
    hand_at(&s, 0.0f, START_DY + 1.2f);        /* start low, so the rise stays in frame */
    still(&s, &fixture_real_open_palm, 0.4);
    sweep(&s, 0.0f, -1.2f, 0.3);
    still(&s, &fixture_real_open_palm, 0.4);
    CHECK(s.flicks == 0);
    finish(&s);
}

static void a_drop_that_ends_out_of_the_picture_does_not_fire(void)
{
    /* Two identical hold-and-drops; the only difference is where in the frame
     * they happen. The first is the gesture. The second starts from where the
     * fixture photo puts the hand - low - so the same drop carries it out of the
     * bottom of the picture, which is a hand being taken away. MediaPipe keeps
     * reporting landmarks past the edge, so "out of the picture" is a landmark
     * reading beyond 1 - flick_edge_margin, not a hand that vanished. */
    session inside = start();
    still(&inside, &fixture_real_open_palm, 0.4);
    sweep(&inside, 0.0f, 1.2f, 0.3);
    still(&inside, &fixture_real_open_palm, 0.4);
    CHECK(inside.flicks == 1);
    finish(&inside);

    session leaving = start();
    hand_at(&leaving, 0.0f, 0.0f);
    still(&leaving, &fixture_real_open_palm, 0.4);
    sweep(&leaving, 0.0f, 1.2f, 0.3);
    still(&leaving, &fixture_real_open_palm, 0.4);
    CHECK(leaving.flicks == 0);
    finish(&leaving);
}

static void a_drop_ending_in_a_fist_does_not_fire(void)
{
    /* Held open, then the hand closes as it falls - a hand dropping to grab
     * something. The end of the drop may be any shape EXCEPT a closed one (see
     * the blurred-palm test below), and it stays not a flick for as long as the
     * hand is closed. */
    session s = start();
    gv_frame fist = fist_where_the_palm_is();
    still(&s, &fixture_real_open_palm, 0.4);
    sweep(&s, 0.0f, 0.4f, 0.1);
    travel(&s, &fist, 0.0f, 0.8f, 0.2);
    still(&s, &fist, 0.4);
    CHECK(s.flicks == 0);
    finish(&s);
}

static void a_fist_held_still_then_dropped_does_not_fire(void)
{
    /* The hold has to be an OPEN PALM on every one of its frames. A fist held
     * just as still, dropped just as far, is nothing: 46% of the talking-hands
     * frames read as an open palm, so the pose alone could never carry this
     * gesture - but without it, every resting hand that then moves is one. */
    session s = start();
    gv_frame fist = fist_where_the_palm_is();
    still(&s, &fist, 0.4);
    travel(&s, &fist, 0.0f, 1.2f, 0.3);
    still(&s, &fist, 0.4);
    CHECK(s.flicks == 0);
    finish(&s);
}

static void a_half_open_hand_held_still_then_dropped_does_not_fire(void)
{
    /* The two halves are asked DIFFERENT questions, and this is the half that
     * is strict. A hand blurred half way to a fist may END a drop (see
     * a_drop_that_ends_with_the_palm_blurred_still_fires) but may not BE the
     * hold: holding still is the deliberate part, and the pose there has to be
     * a confident open palm. Everything else about this motion would pass - it
     * is held, it drops 1.2 hand-lengths straight down, it stays in frame and
     * it does not end in a fist. */
    gv_config cfg = shipped();
    gv_frame half = blurred(0.25f);
    CHECK(!gv_pose_is_open_palm(&half, REAL, &cfg.pose));
    CHECK(!gv_pose_is_fist(&half, REAL, &cfg.pose));

    session s = start();
    still(&s, &half, 0.4);
    travel(&s, &half, 0.0f, 1.2f, 0.3);
    still(&s, &half, 0.4);
    CHECK(s.flicks == 0);
    finish(&s);
}

static void the_drop_is_measured_against_the_hand_as_it_was_held(void)
{
    /* Half a hand-length down - short of the 0.8 - while the hand shrinks to
     * half its size, which is what moving away from the camera does. Measured
     * in hand-lengths of the hand as it was HELD it is 0.5 and nothing happens;
     * measured against the smaller hand it ended as, the same motion would read
     * as a full hand-length and fire. The reference is the held frame, and this
     * pins which end of the drop it is taken from. */
    session s = start();
    fall_while_resizing(&s, 0.5f, 0.5f);
    CHECK(s.flicks == 0);
    finish(&s);
}

static void an_open_palm_held_still_and_left_there_never_fires(void)
{
    /* Three seconds of a held open palm and nothing else. The hold is half the
     * gesture, never the whole of it: a hand can sit open in front of the
     * camera for as long as it likes. */
    session s = start();
    still(&s, &fixture_real_open_palm, 3.0);
    CHECK(s.flicks == 0);
    CHECK(s.busy_frames == 0);
    finish(&s);
}

static void a_hand_that_shrinks_as_it_drops_is_still_a_flick(void)
{
    /* A drop swings the hand away from the camera, so it gets smaller as it
     * falls: on the owner's own session the real swipes measured 0.66, 0.68,
     * 0.51 and 0.24 of their starting size. An earlier version of the rule
     * required the hand to stay within 0.7 - 1.4 of its size across the span,
     * which vetoed exactly those. There is no size condition any more; what
     * stops a hand DROPPING OUT of the picture is the edge veto, tested
     * above. */
    session shrinking = start();
    fall_while_resizing(&shrinking, 0.5f, 1.2f);
    CHECK(shrinking.flicks == 1);
    finish(&shrinking);

    /* And towards the camera, for the same reason: growing is not evidence of
     * anything either. */
    session growing = start();
    fall_while_resizing(&growing, 1.5f, 1.2f);
    CHECK(growing.flicks == 1);
    finish(&growing);
}

static void a_drop_that_ends_with_the_palm_blurred_still_fires(void)
{
    /* The same shape as a_drop_ending_in_a_fist_does_not_fire, but the hand
     * ends half open rather than shut. Seven of the sixteen downward open-palm
     * motions in the first live session had lost the open palm by their last
     * sample (open(old,new) = 1,0): fast motion blurs the fingers and they stop
     * reading as "long AND straight". So the end of the drop asks only that the
     * hand is not CLOSED. */
    gv_config cfg = shipped();
    gv_frame half = blurred(0.25f);
    CHECK(!gv_pose_is_open_palm(&half, REAL, &cfg.pose)); /* not open any more ... */
    CHECK(!gv_pose_is_fist(&half, REAL, &cfg.pose));      /* ... but not a fist either */

    session s = start();
    still(&s, &fixture_real_open_palm, 0.4);
    sweep(&s, 0.0f, 0.5f, 0.1);
    travel(&s, &half, 0.0f, 0.7f, 0.15);
    still(&s, &half, 0.3);
    CHECK(s.flicks == 1);
    finish(&s);
}

static void a_gap_in_tracking_breaks_the_drop(void)
{
    /* Held, then the hand disappears for nine frames and is next seen 1.2
     * hand-lengths lower. Measured end to end that is the gesture; but the
     * 0.30 s hole is longer than the release grace (0.25 s), and nothing says
     * the hand travelled there rather than being put there. (The hole is still
     * shorter than the 0.6 s drop window, so the held frames really are inside
     * the span being searched - that is what makes this a test of the gap rule
     * and not of the window.) */
    session s = start();
    still(&s, &fixture_real_open_palm, 0.4);
    vanish(&s, 9 * DT);
    hand_at(&s, 0.0f, START_DY + 1.2f);
    still(&s, &fixture_real_open_palm, 0.5);
    CHECK(s.flicks == 0);
    finish(&s);
}

/* --- when it is allowed to fire ----------------------------------------- */

static void just_after_letting_go_nothing_fires(void)
{
    /* A volume slide, then the hand leaves and the slide releases, then a hold
     * and a drop straight away. This is the shape of "finished adjusting, put
     * the hand down", and it must not minimise the window just adjusted. */
    session s = start();
    still(&s, &fixture_real_tight_two, 0.5);
    CHECK(s.out.state == GV_ARMED && s.out.kind == GV_KIND_VOLUME);
    vanish(&s, 0.3);                       /* longer than the release grace */
    CHECK(s.out.state == GV_IDLE);
    still(&s, &fixture_real_open_palm, 0.4);
    sweep(&s, 0.0f, 1.2f, 0.3);
    still(&s, &fixture_real_open_palm, 0.3);
    CHECK(s.flicks == 0);
    finish(&s);

    /* The same hold and drop, well clear of the refractory: now it is a flick. */
    session t = start();
    still(&t, &fixture_real_tight_two, 0.5);
    CHECK(t.out.state == GV_ARMED && t.out.kind == GV_KIND_VOLUME);
    vanish(&t, 2.0);
    still(&t, &fixture_real_open_palm, 0.4);
    sweep(&t, 0.0f, 1.2f, 0.3);
    still(&t, &fixture_real_open_palm, 0.3);
    CHECK(t.flicks == 1);
    finish(&t);
}

static void a_second_flick_waits_for_the_refractory(void)
{
    /* Flick, raise the hand, hold and drop again - all inside the refractory.
     * Only the first one counts. */
    session s = start();
    still(&s, &fixture_real_open_palm, 0.4);
    sweep(&s, 0.0f, 1.2f, 0.3);
    sweep(&s, 0.0f, -1.2f, 0.3);           /* the hand comes back up */
    still(&s, &fixture_real_open_palm, 0.4);
    sweep(&s, 0.0f, 1.2f, 0.3);
    still(&s, &fixture_real_open_palm, 0.2);
    CHECK(s.flicks == 1);
    finish(&s);

    /* The same two, with a longer hold between them: the second one is clear of
     * the refractory and counts. */
    session t = start();
    still(&t, &fixture_real_open_palm, 0.4);
    sweep(&t, 0.0f, 1.2f, 0.3);
    sweep(&t, 0.0f, -1.2f, 0.3);
    still(&t, &fixture_real_open_palm, 1.8);
    sweep(&t, 0.0f, 1.2f, 0.3);
    still(&t, &fixture_real_open_palm, 0.2);
    CHECK(t.flicks == 2);
    finish(&t);
}

static void one_drop_fires_once_even_with_no_refractory(void)
{
    /* The refractory is not the only thing keeping one drop to one firing: the
     * frames that fired are dropped from the history, so the same motion cannot
     * be counted again as the drop window slides across it. Pinning the
     * refractory to zero is what makes that rule visible on its own. */
    gv_config c = shipped();
    c.flick_refractory_sec = 0.0f;
    session s = start_with(c);
    still(&s, &fixture_real_open_palm, 0.4);
    sweep(&s, 0.0f, 1.2f, 0.3);
    still(&s, &fixture_real_open_palm, 0.3);
    CHECK(s.flicks == 1);
    finish(&s);
}

static void a_flick_never_fires_while_armed(void)
{
    /* Armed on the volume, the open palm still passes the lenient stay pose
     * (index and middle are not curled), so the slide runs on until the jump
     * to the other fixture stops being believed and the grace period runs out.
     * Either way the state is not IDLE, and the frame it lets go on starts the
     * refractory: nothing minimises in the middle of a slide. */
    session s = start();
    still(&s, &fixture_real_tight_two, 0.5);
    CHECK(s.out.state == GV_ARMED && s.out.kind == GV_KIND_VOLUME);
    still(&s, &fixture_real_open_palm, 0.4);
    sweep(&s, 0.0f, 1.2f, 0.3);
    still(&s, &fixture_real_open_palm, 0.3);
    CHECK(s.armed_frames > 0);
    CHECK(s.flicks == 0);
    finish(&s);
}

int main(void)
{
    fprintf(stderr, "test_flick\n");
    RUN(a_held_open_palm_then_a_drop_fires_once);
    RUN(a_drop_with_no_hold_before_it_does_not_fire);
    RUN(a_hand_lowered_slowly_instead_of_held_does_not_fire);
    RUN(a_drop_slower_than_the_drop_window_does_not_fire);
    RUN(a_drop_that_is_not_far_enough_does_not_fire);
    RUN(sideways_after_a_hold_does_not_fire);
    RUN(upward_after_a_hold_does_not_fire);
    RUN(a_drop_that_ends_out_of_the_picture_does_not_fire);
    RUN(a_drop_ending_in_a_fist_does_not_fire);
    RUN(a_fist_held_still_then_dropped_does_not_fire);
    RUN(a_half_open_hand_held_still_then_dropped_does_not_fire);
    RUN(the_drop_is_measured_against_the_hand_as_it_was_held);
    RUN(an_open_palm_held_still_and_left_there_never_fires);
    RUN(a_hand_that_shrinks_as_it_drops_is_still_a_flick);
    RUN(a_drop_that_ends_with_the_palm_blurred_still_fires);
    RUN(a_gap_in_tracking_breaks_the_drop);
    RUN(just_after_letting_go_nothing_fires);
    RUN(a_second_flick_waits_for_the_refractory);
    RUN(one_drop_fires_once_even_with_no_refractory);
    RUN(a_flick_never_fires_while_armed);
    return TEST_SUMMARY();
}
