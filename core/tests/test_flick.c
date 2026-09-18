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
 * The distances and durations below are arithmetic on the SHIPPED defaults
 * (flick_travel 1.5 hand-lengths inside flick_window_sec 0.35 s, sideways at
 * most flick_max_sideways 0.5 of the fall, the hand the same size at both ends
 * of the span within gate_scale_min .. gate_scale_max = 0.7 .. 1.4, and
 * flick_refractory_sec 1.5 after any release and after the previous flick).
 * That is deliberate: this gesture's whole risk is firing when nobody asked, so
 * the tests exercise the numbers that actually ship. If a default is retuned,
 * the motions here move with it.
 */
#include "test.h"

#include "fixtures.h"
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

static session start_with(gv_config cfg)
{
    session s = { 0 };
    s.ctx = gv_create(&cfg);
    s.out.scale = 1.0f;
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

/* One hand-length, in frame heights. Every offset below is in THESE units,
 * whichever fixture is being moved, so that a motion means the same distance
 * when the hand changes shape half way through it. (The fist fixture's own
 * hand measures 6% smaller; nothing here turns on the difference.) */
static float hand_length(void) { return gv_hand_scale(&fixture_real_open_palm, REAL); }

/* The fixture, moved by dx hand-lengths right and dy hand-lengths down. */
static gv_frame moved(const gv_frame *hand, float dx, float dy)
{
    gv_frame f = *hand;
    float hl = hand_length();
    for (int j = 0; j < GV_NUM_LANDMARKS; j++) {
        /* Landmark x is a fraction of the WIDTH and y of the HEIGHT, while a
         * hand-length (gv_hand_scale) is in frame heights. Dividing the x step
         * by the aspect leaves both offsets the same physical length. */
        f.lm[j].x += dx * hl / REAL;
        f.lm[j].y += dy * hl;
    }
    return f;
}

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

/* Move the hand without showing any of the journey: where it is when tracking
 * picks it up again. */
static void reappears_at(session *s, float dx, float dy) { s->dx = dx; s->dy = dy; }

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

/* The open palm falling `dy_hands` in a quarter of a second, while its apparent
 * size is multiplied by `k` over every flick window. The size changes at that
 * same steady rate before and after the fall as well, so WHATEVER span the rule
 * ends up judging, the hand at its two ends differs in size by k - there is no
 * moment where the span happens to hold two frames of equal size. (The judged
 * span is ten frames, a shade under the 0.35 s window, so the ends differ by
 * k^0.95 in practice: 0.61 and 1.45 against the 0.7 - 1.4 gate.) k = 1 leaves
 * the size alone and is the control: the identical fall, one flick.
 *
 * The fall is deliberately a long one. Travel is counted in hand-lengths of the
 * span's FIRST frame, so a hand that has been growing is measured against a
 * bigger hand than it started as, and a 2 hand-length fall would fail the
 * travel test on its own - which would make this a test of the travel rule
 * wearing a disguise. At 3 the travel test passes in all three cases (2.0, 4.9
 * and 3.0 hand-lengths), and the hand's size is the only thing that differs. */
static void fall_while_resizing(session *s, float k, float dy_hands)
{
    const float window = gv_default_config().flick_window_sec;
    int lead = (int)(0.4 / DT + 0.5), fall = (int)(0.25 / DT + 0.5), tail = (int)(0.4 / DT + 0.5);
    for (int i = 0; i < lead + fall + tail; i++) {
        float size = powf(k, (float)(i * DT) / window);
        float fallen = i < lead ? 0.0f
                     : (i < lead + fall ? dy_hands * (float)(i - lead + 1) / (float)fall
                                        : dy_hands);
        gv_frame smaller = resized(&fixture_real_open_palm, size);
        gv_frame f = moved(&smaller, s->dx, s->dy + fallen);
        frame(s, &f);
    }
    s->dy += dy_hands;
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

/* --- the gesture ------------------------------------------------------- */

static void a_fast_downward_open_palm_fires_once(void)
{
    session s = start();
    still(&s, &fixture_real_open_palm, 0.3);
    sweep(&s, 0.0f, 2.0f, 0.25);               /* two hand-lengths in a quarter second */
    still(&s, &fixture_real_open_palm, 0.5);   /* and rests where it landed */

    CHECK(s.flicks == 1);                      /* once, not once per frame of the fall */
    CHECK(s.at_flick.state == GV_IDLE);        /* a flick never arms ... */
    CHECK(s.busy_frames == 0);                 /* ... nor does the palm arm anything else */
    CHECK(s.at_flick.changed == 1);
    CHECK_NEAR(s.at_flick.scale, 1.0f, 1e-6);  /* it is not a resize */
    CHECK_NEAR(s.at_flick.volume, START_VOLUME, 1e-6);
    CHECK(s.volume_changes == 0);
    finish(&s);
}

static void too_slow_does_not_fire(void)
{
    /* The same two hand-lengths, taken four times as long: inside any 0.35 s
     * window the palm has fallen about half a hand-length. Lowering a hand is
     * not a flick. */
    session s = start();
    still(&s, &fixture_real_open_palm, 0.3);
    sweep(&s, 0.0f, 2.0f, 1.0);
    still(&s, &fixture_real_open_palm, 0.5);
    CHECK(s.flicks == 0);
    finish(&s);
}

static void not_far_enough_does_not_fire(void)
{
    session s = start();
    still(&s, &fixture_real_open_palm, 0.3);
    sweep(&s, 0.0f, 1.0f, 0.25);   /* fast, but only one hand-length */
    still(&s, &fixture_real_open_palm, 0.5);
    CHECK(s.flicks == 0);
    finish(&s);
}

static void sideways_does_not_fire(void)
{
    /* Diagonal: it falls far enough and fast enough, but travels just as far
     * across, which is a wave, not a flick. */
    session s = start();
    still(&s, &fixture_real_open_palm, 0.3);
    sweep(&s, 2.0f, 2.0f, 0.25);
    still(&s, &fixture_real_open_palm, 0.5);
    CHECK(s.flicks == 0);
    finish(&s);

    /* And straight across, which has no fall at all. */
    session t = start();
    still(&t, &fixture_real_open_palm, 0.3);
    sweep(&t, 2.0f, 0.0f, 0.25);
    still(&t, &fixture_real_open_palm, 0.5);
    CHECK(t.flicks == 0);
    finish(&t);
}

static void upward_does_not_fire(void)
{
    session s = start();
    reappears_at(&s, 0.0f, 2.0f);              /* start low, so the rise stays in frame */
    still(&s, &fixture_real_open_palm, 0.3);
    sweep(&s, 0.0f, -2.0f, 0.25);
    still(&s, &fixture_real_open_palm, 0.5);
    CHECK(s.flicks == 0);
    finish(&s);
}

static void a_closed_hand_sweeping_down_does_not_fire(void)
{
    /* The motion of a flick made by a fist. The pose carries the veto: 46% of
     * the talking-hands frames read as an open palm, so the pose alone can
     * never be enough - but without it, every dropped hand is a flick. */
    session s = start();
    still(&s, &fixture_real_fist, 0.3);
    travel(&s, &fixture_real_fist, 0.0f, 2.0f, 0.25);
    still(&s, &fixture_real_fist, 0.5);
    CHECK(s.flicks == 0);
    finish(&s);
}

static void a_hand_that_opens_as_it_falls_does_not_fire(void)
{
    /* The same two hand-lengths in a quarter second as a real flick, but the
     * hand is closed for the first two thirds of the fall and only opens near
     * the bottom: a hand being dropped and relaxed. Measured end to end that is
     * a flick; measured over the part where the palm was actually open it is
     * 0.8 hand-lengths, well under the travel. The palm has to be open at BOTH
     * ends of the span, or the gesture becomes "let your hand down", which is
     * the commonest thing in the recordings. */
    session s = start();
    gv_frame fist = fist_where_the_palm_is();
    still(&s, &fist, 0.3);
    travel(&s, &fist, 0.0f, 1.2f, 0.15);
    sweep(&s, 0.0f, 0.8f, 0.1);
    still(&s, &fixture_real_open_palm, 0.4);
    CHECK(s.flicks == 0);
    finish(&s);
}

static void a_hand_that_closes_as_it_falls_does_not_fire(void)
{
    /* The mirror image: open at the top, closed for the last two thirds of the
     * fall - a hand dropping to grab something. Both ends of the span have to
     * be an open palm, so this is not a flick either, and it stays not a flick
     * for as long as the hand is closed. */
    session s = start();
    gv_frame fist = fist_where_the_palm_is();
    still(&s, &fixture_real_open_palm, 0.3);
    sweep(&s, 0.0f, 0.8f, 0.1);
    travel(&s, &fist, 0.0f, 1.2f, 0.15);
    still(&s, &fist, 0.4);
    CHECK(s.flicks == 0);
    finish(&s);
}

static void a_hand_that_shrinks_or_grows_while_falling_is_not_a_flick(void)
{
    /* A hand dropping out of the bottom of the frame changes apparent size as
     * it goes; a hand swiped across the picture does not. Both false flicks the
     * recordings produced were the first kind (the worse of them grew 1.71x in
     * a third of a second), so the span has to show one hand, by the same scale
     * gate that judges every armed frame: 0.6 is below gate_scale_min (0.7) and
     * 1.5 is above gate_scale_max (1.4). The fall is identical in all three. */
    session shrinking = start();
    fall_while_resizing(&shrinking, 0.6f, 3.0f);
    CHECK(shrinking.flicks == 0);
    finish(&shrinking);

    session growing = start();
    fall_while_resizing(&growing, 1.5f, 3.0f);
    CHECK(growing.flicks == 0);
    finish(&growing);

    session steady = start();
    fall_while_resizing(&steady, 1.0f, 3.0f);
    CHECK(steady.flicks == 1);   /* the same fall, at one size: a flick */
    finish(&steady);
}

static void a_gap_in_tracking_breaks_the_sweep(void)
{
    /* The hand is still at the top, disappears for eight frames, and is next
     * seen two hand-lengths lower. Measured end to end that is a flick; but
     * the 0.30 s hole between the two real frames is longer than the release
     * grace (0.25 s), and nothing says the hand travelled there rather than
     * being put there. (The hole is still shorter than the 0.35 s window, so
     * the two frames really are both in the span being judged - that is what
     * makes this a test of the gap rule and not of the window.) */
    session s = start();
    still(&s, &fixture_real_open_palm, 0.3);
    vanish(&s, 8 * DT);
    reappears_at(&s, 0.0f, 2.0f);
    still(&s, &fixture_real_open_palm, 0.5);
    CHECK(s.flicks == 0);
    finish(&s);
}

/* --- when it is allowed to fire ----------------------------------------- */

static void just_after_letting_go_nothing_fires(void)
{
    /* A volume slide, then the hand leaves and the slide releases, then the
     * flick motion straight away. This is the shape of "finished adjusting,
     * put the hand down", and it must not minimise the window just adjusted. */
    session s = start();
    still(&s, &fixture_real_tight_two, 0.5);
    CHECK(s.out.state == GV_ARMED && s.out.kind == GV_KIND_VOLUME);
    vanish(&s, 0.3);                       /* longer than the release grace */
    CHECK(s.out.state == GV_IDLE);
    still(&s, &fixture_real_open_palm, 0.3);
    sweep(&s, 0.0f, 2.0f, 0.25);
    still(&s, &fixture_real_open_palm, 0.3);
    CHECK(s.flicks == 0);
    finish(&s);

    /* The same sweep, well clear of the refractory: now it is a flick. */
    session t = start();
    still(&t, &fixture_real_tight_two, 0.5);
    CHECK(t.out.state == GV_ARMED && t.out.kind == GV_KIND_VOLUME);
    vanish(&t, 2.0);
    still(&t, &fixture_real_open_palm, 0.3);
    sweep(&t, 0.0f, 2.0f, 0.25);
    still(&t, &fixture_real_open_palm, 0.3);
    CHECK(t.flicks == 1);
    finish(&t);
}

static void a_second_flick_waits_for_the_refractory(void)
{
    /* Flick, raise the hand, flick again - all inside the refractory. Only the
     * first one counts. */
    session s = start();
    still(&s, &fixture_real_open_palm, 0.3);
    sweep(&s, 0.0f, 2.0f, 0.25);
    sweep(&s, 0.0f, -2.0f, 0.3);           /* the hand comes back up */
    still(&s, &fixture_real_open_palm, 0.2);
    sweep(&s, 0.0f, 2.0f, 0.25);
    still(&s, &fixture_real_open_palm, 0.2);
    CHECK(s.flicks == 1);
    finish(&s);

    /* The same two flicks, with a pause between them. */
    session t = start();
    still(&t, &fixture_real_open_palm, 0.3);
    sweep(&t, 0.0f, 2.0f, 0.25);
    sweep(&t, 0.0f, -2.0f, 0.3);
    still(&t, &fixture_real_open_palm, 1.8);
    sweep(&t, 0.0f, 2.0f, 0.25);
    still(&t, &fixture_real_open_palm, 0.2);
    CHECK(t.flicks == 2);
    finish(&t);
}

static void one_fall_fires_once_even_with_no_refractory(void)
{
    /* The refractory is not the only thing keeping a single fall to a single
     * firing: the frames that fired are dropped from the history, so the same
     * motion cannot be counted again as the window slides across it. Pinning
     * the refractory to zero is what makes that rule visible on its own. */
    gv_config c = shipped();
    c.flick_refractory_sec = 0.0f;
    session s = start_with(c);
    still(&s, &fixture_real_open_palm, 0.3);
    sweep(&s, 0.0f, 2.0f, 0.25);
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
    sweep(&s, 0.0f, 2.0f, 0.25);
    still(&s, &fixture_real_open_palm, 0.3);
    CHECK(s.armed_frames > 0);
    CHECK(s.flicks == 0);
    finish(&s);
}

int main(void)
{
    fprintf(stderr, "test_flick\n");
    RUN(a_fast_downward_open_palm_fires_once);
    RUN(too_slow_does_not_fire);
    RUN(not_far_enough_does_not_fire);
    RUN(sideways_does_not_fire);
    RUN(upward_does_not_fire);
    RUN(a_closed_hand_sweeping_down_does_not_fire);
    RUN(a_hand_that_opens_as_it_falls_does_not_fire);
    RUN(a_hand_that_closes_as_it_falls_does_not_fire);
    RUN(a_hand_that_shrinks_or_grows_while_falling_is_not_a_flick);
    RUN(a_gap_in_tracking_breaks_the_sweep);
    RUN(just_after_letting_go_nothing_fires);
    RUN(a_second_flick_waits_for_the_refractory);
    RUN(one_fall_fires_once_even_with_no_refractory);
    RUN(a_flick_never_fires_while_armed);
    return TEST_SUMMARY();
}
