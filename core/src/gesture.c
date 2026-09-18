/*
 * gesture.c - the recogniser: one state machine for every gesture.
 *
 *   IDLE    nothing happening. A gesture's arm pose appearing starts ARMING it.
 *   ARMING  pose seen, not yet trusted. Held for arm_hold_sec -> ARMED.
 *   ARMED   the gesture is live; its definition turns each believed frame into
 *           a value (gestures/volume.c: a volume; more to come).
 *
 * One gesture does not go through any of that. The FLICK - an open palm swiped
 * down, which minimises the front window - is a one-shot: it never arms, holds
 * nothing and has no state to leave. It is judged from a short history of the
 * palm's motion, only while the machine is IDLE and only outside a refractory
 * period after any release, and it reports itself on a single frame. See
 * flick_seen below.
 *
 * This file owns what gestures have in common, so it is written and tested
 * once: which frames are usable at all, how long a pose must hold, release,
 * and three rules that keep accidental movement from doing anything:
 *   - only frames that show the right pose count; every other frame freezes
 *     the gesture, so the act of lowering the hand cannot jerk anything;
 *   - while armed, a frame must also be plausible: the hand about the size it
 *     was, and not further away than a hand can travel in the time since the
 *     last believed frame. Anything else is a detector glitch and is ignored;
 *   - good frames may be missing for release_grace_sec before the gesture lets
 *     go, because landmark models lose a frame now and then.
 *
 * Starting is strict and carrying on is lenient (see pose.h): a relaxed hand
 * wanders in and out of a strict pose, and demanding it on every frame made
 * the first version drop out mid-slide. What is particular to one gesture (its
 * two poses, what it measures, what it reports) is a gv_gesture definition; see
 * gesture_internal.h.
 */
#include "gesture.h"

#include <math.h>
#include <stdlib.h>

#include "gesture_internal.h"
#include "pose.h"

/* A hand this small (in frame heights) is noise, not a hand, and dividing
 * travel by it would blow the slider up. */
#define MIN_HAND_SCALE 1e-3f

gv_config gv_default_config(void)
{
    /* Designated initialisers: fields are named, order does not matter, and
     * anything left out would be zero. */
    gv_config cfg = {
        .aspect            = 16.0f / 9.0f,
        .pose              = { .extend_reach = 1.20f, .extend_straight = 0.93f,
                               .curl_reach = 1.00f, .curl_straight = 0.75f,
                               /* Thumb, from the probe clips: out = reach 1.55 - 2.0 and
                                * straightness 0.90 - 0.99; tucked in = 0.12 - 0.53 and
                                * 0.33 - 0.44. Each limit sits midway, or at the tucked
                                * range's upper edge. */
                               .thumb_extend_reach = 1.05f, .thumb_extend_straight = 0.67f,
                               .thumb_curl_reach = 0.55f, .thumb_curl_straight = 0.45f,
                               /* half the smallest gap seen in the held pinch-ready pose */
                               .pinch_gap_min = 0.73f,
                               /* midway between 0.28 (resting, 95th pct) and 0.97 (held out, 5th) */
                               .thumb_clear_min = 0.62f },
        .arm_hold_sec      = 0.25f,
        /* 250 ms is about seven frames at the 27 fps the camera loop manages;
         * the first live session saw frame gaps of 75 ms at the 99th percentile. */
        .release_grace_sec = 0.25f,
        /* Frame to frame, the real hand in the first recording changed size by
         * 0.75x - 1.26x (99% of frames). The different hand that tracking once
         * jumped to was 0.61x. */
        .gate_scale_min    = 0.7f,
        .gate_scale_max    = 1.4f,
        /* Hand-lengths per second. People do sweep fast: the first recording
         * has 12.5 at the 99th percentile and 28 at the 99.9th, against 181
         * for a glitch. A limit of 15 rejected real sweeps, which then caught
         * up in one jump. */
        .gate_max_speed    = 30.0f,
        /* Speed-dependent, like a mouse pointer. One fixed gain failed both
         * ways live: 1/3 was too twitchy to set a level, 0.2 too slow to get
         * anywhere. Slow (under 1 hand-length/s) is fine control, about seven
         * hand-lengths for the full range; fast (over 5/s) covers it in under
         * two. In the recordings the median armed speed is 1.3 and the 90th
         * percentile 6.1. */
        .gain              = 0.15f,
        .gain_fast         = 0.6f,
        .accel_speed_lo    = 1.0f,
        .accel_speed_hi    = 5.0f,
        .edge_margin       = 0.04f,
        .edge_rate         = 0.35f,
        .axis_lock_hands   = 0.5f,
        .axis_dominance    = 2.0f,
        .dead_band         = 0.01f,
        /* Full range in half a second. Faster than anyone needs, and slow enough
         * that a glitch is a ramp the release timer cuts short, not a jump. */
        .max_slew          = 2.0f,
        .filter_min_cutoff = 1.0f,
        .filter_beta       = 4.0f,
        .filter_d_cutoff   = 1.0f,
        .scale_min         = 0.1f,
        .scale_max         = 10.0f,
        .scale_dead_band   = 0.01f,
        /* Two octaves a second at most: a window may at most quadruple or drop
         * to a quarter in a second, however far past the dead zone the fingers
         * sit. See resize_dead_zone and resize_rate_gain below. */
        .resize_rate_max   = 2.0f,
        /* A hand closes in 0.1 - 0.2 s; 0.3 s reaches back to before it started. */
        .release_rollback_sec = 0.3f,
        /* 0.08 sits clear of the measured 2.3% jitter step; 4.5 gives about one
         * doubling per second at a comfortable 30% finger offset. Proxies until
         * the guided clips exist (see gesture.h). */
        .resize_dead_zone  = 0.08f,
        .resize_rate_gain  = 4.5f,
        /* About every other frame at 30 fps. See gesture.h. */
        .win_write_interval_sec = 0.06f,
        /* The flick. A deliberate swipe down covers more than a hand-length in
         * a third of a second; a hand being lowered, or waved, does not do that
         * in a straight line with the palm open at both ends. Proxies from the
         * design spec until a guided flick clip exists - what they ARE measured
         * against is the negative corpus (every recording on disk), where they
         * fire nothing; see the spec's Findings. */
        .flick_travel         = 1.5f,
        .flick_window_sec     = 0.35f,
        .flick_max_sideways   = 0.5f,
        /* 1.5 s, not 1.0: with the motion rule alone the recordings produce
         * twelve downward sweeps of an open palm, and the last of them that is
         * not a genuine gesture lands 1.10 s after a release (a hand lowered
         * after a slide, then dropped out of frame). 1.0 let that one through. */
        .flick_refractory_sec = 1.5f,
    };
    return cfg;
}

/* The config arrives from outside (ctypes, in practice), so it is checked
 * once here rather than trusted everywhere else. `x > 0` is false for NaN, so
 * those comparisons reject NaN for free; the fields that may be zero or
 * negative need the explicit isfinite. */
static int config_is_valid(const gv_config *c)
{
    const gv_pose_limits *p = &c->pose;
    int pose_ok = p->extend_reach > 0.0f && isfinite(p->extend_reach) &&
                  p->extend_straight > 0.0f && isfinite(p->extend_straight) &&
                  p->curl_reach > 0.0f && p->curl_straight > 0.0f &&
                  /* curl below extend, or one finger could be both at once */
                  p->curl_reach <= p->extend_reach &&
                  p->curl_straight <= p->extend_straight &&
                  /* the thumb's limits, by the same rules */
                  p->thumb_extend_reach > 0.0f && isfinite(p->thumb_extend_reach) &&
                  p->thumb_extend_straight > 0.0f && isfinite(p->thumb_extend_straight) &&
                  p->thumb_curl_reach > 0.0f && p->thumb_curl_straight > 0.0f &&
                  p->thumb_curl_reach <= p->thumb_extend_reach &&
                  p->thumb_curl_straight <= p->thumb_extend_straight &&
                  p->pinch_gap_min > 0.0f && isfinite(p->pinch_gap_min) && /* the resize divides by it */
                  p->thumb_clear_min >= 0.0f && isfinite(p->thumb_clear_min);
    /* the scale gate has to let an unchanged hand (ratio 1.0) through */
    int gate_ok = c->gate_scale_min > 0.0f && c->gate_scale_min <= 1.0f &&
                  c->gate_scale_max >= 1.0f && isfinite(c->gate_scale_max) &&
                  c->gate_max_speed > 0.0f && isfinite(c->gate_max_speed) &&
                  c->max_slew > 0.0f && isfinite(c->max_slew) &&
                  c->axis_lock_hands >= 0.0f && isfinite(c->axis_lock_hands) &&
                  /* below 1 the "dominant" axis could be the smaller one */
                  c->axis_dominance >= 1.0f && isfinite(c->axis_dominance) &&
                  /* a margin of half the frame or more would make every hand "at the edge" */
                  c->edge_margin >= 0.0f && c->edge_margin < 0.5f &&
                  c->edge_rate >= 0.0f && isfinite(c->edge_rate);
    /* the scale range has to contain 1.0, the scale at the moment of arming */
    int scale_ok = c->scale_min > 0.0f && c->scale_min <= 1.0f &&
                   c->scale_max >= 1.0f && isfinite(c->scale_max) &&
                   c->scale_dead_band >= 0.0f && isfinite(c->scale_dead_band) &&
                   c->resize_rate_max > 0.0f && isfinite(c->resize_rate_max) &&
                   c->release_rollback_sec >= 0.0f && isfinite(c->release_rollback_sec) &&
                   c->resize_dead_zone >= 0.0f && c->resize_dead_zone < 1.0f &&
                   isfinite(c->resize_dead_zone) &&
                   c->resize_rate_gain >= 0.0f && isfinite(c->resize_rate_gain) &&
                   c->win_write_interval_sec >= 0.0f && isfinite(c->win_write_interval_sec);
    /* A flick with no travel, or no window to travel in, would fire on
     * anything; the other two may be zero (no sideways travel at all; no
     * quiet time), which is strict and slack respectively, but not nonsense. */
    int flick_ok = c->flick_travel > 0.0f && isfinite(c->flick_travel) &&
                   c->flick_window_sec > 0.0f && isfinite(c->flick_window_sec) &&
                   c->flick_max_sideways >= 0.0f && isfinite(c->flick_max_sideways) &&
                   c->flick_refractory_sec >= 0.0f && isfinite(c->flick_refractory_sec);

    return pose_ok && gate_ok && scale_ok && flick_ok &&
           c->aspect > 0.0f && isfinite(c->aspect) &&
           c->arm_hold_sec >= 0.0f && isfinite(c->arm_hold_sec) &&
           c->release_grace_sec >= 0.0f && isfinite(c->release_grace_sec) &&
           isfinite(c->gain) && isfinite(c->gain_fast) && /* negative reverses the slider */
           c->accel_speed_lo >= 0.0f && isfinite(c->accel_speed_hi) &&
           c->accel_speed_hi > c->accel_speed_lo && /* equal would divide by zero */
           c->dead_band >= 0.0f && isfinite(c->dead_band) &&
           c->filter_min_cutoff > 0.0f && isfinite(c->filter_min_cutoff) &&
           c->filter_beta >= 0.0f && isfinite(c->filter_beta) &&
           c->filter_d_cutoff > 0.0f && isfinite(c->filter_d_cutoff);
}

gv_ctx *gv_create(const gv_config *cfg)
{
    gv_config chosen = cfg ? *cfg : gv_default_config();
    if (!config_is_valid(&chosen))
        return NULL;

    /* calloc zero-fills, so every field not set below starts as 0 / GV_IDLE. */
    gv_ctx *ctx = calloc(1, sizeof *ctx);
    if (ctx == NULL)
        return NULL;

    ctx->cfg = chosen;
    gv_filter_init(&ctx->x_filter, ctx->cfg.filter_min_cutoff, ctx->cfg.filter_beta,
                   ctx->cfg.filter_d_cutoff);
    ctx->y_filter = ctx->x_filter; /* same parameters; struct assignment copies them */
    ctx->gap_filter = ctx->x_filter;
    ctx->reported_scale = 1.0f;
    ctx->scale_target = 1.0f;
    /* "Never" rather than "at time zero": the flick's refractory is measured
     * from these, and starting them at 0 would silence the first second of a
     * session, which the trace timestamps begin at. */
    ctx->last_release_t = -INFINITY;
    ctx->last_flick_t = -INFINITY;
    return ctx;
}

void gv_destroy(gv_ctx *ctx)
{
    free(ctx); /* free(NULL) is defined to do nothing */
}

float gv_clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

void gv_set_current_volume(gv_ctx *ctx, float volume)
{
    if (ctx->state == GV_ARMED || !isfinite(volume))
        return;
    ctx->system_volume = gv_clamp01(volume);
    ctx->reported_volume = ctx->system_volume;
}

/* Every gesture the recogniser knows. While idle they are tried in this order
 * and the first whose arm pose holds starts arming; their arm poses are kept
 * mutually exclusive (a test asserts it), so the order decides nothing. An
 * array of pointers to const structs: adding a gesture is one line here. */
static const gv_gesture *const k_gestures[] = {
    &gv_gesture_volume,
    &gv_gesture_resize,
};
#define GESTURE_COUNT ((int)(sizeof k_gestures / sizeof k_gestures[0]))

/* The gesture this frame could arm (idle), or the one already arming if this
 * frame still shows its arm pose. NULL if neither. */
static const gv_gesture *find_arming_gesture(const gv_ctx *ctx, const gv_frame *frame)
{
    if (ctx->state == GV_ARMING)
        return ctx->active->can_arm(frame, &ctx->cfg) ? ctx->active : NULL;
    for (int i = 0; i < GESTURE_COUNT; i++)
        if (k_gestures[i]->can_arm(frame, &ctx->cfg))
            return k_gestures[i];
    return NULL;
}

static void go_idle(gv_ctx *ctx, double t)
{
    /* When a gesture let go, for the flick's refractory. Arming counts: the
     * hand was doing something deliberate either way. */
    if (ctx->state != GV_IDLE)
        ctx->last_release_t = t;
    ctx->state = GV_IDLE;
    if (ctx->active != NULL)
        ctx->active->on_idle(ctx);
    ctx->active = NULL;
    gv_filter_reset(&ctx->x_filter);
    gv_filter_reset(&ctx->y_filter);
}

static void start_arming(gv_ctx *ctx, const gv_gesture *gesture, double t)
{
    ctx->state = GV_ARMING;
    ctx->active = gesture;
    ctx->arm_started_t = t;
    ctx->scale_sum = 0.0f;
    ctx->scale_count = 0;
    gesture->on_idle(ctx); /* a gesture starts from its own clean state */
    gv_filter_reset(&ctx->x_filter);
    gv_filter_reset(&ctx->y_filter);
}

/* MediaPipe should never emit NaN, but this is the boundary where outside
 * data enters, and one NaN would otherwise poison the filter for good: every
 * later output would be NaN too. */
static int frame_is_usable(const gv_frame *f)
{
    if (!f->present)
        return 0;
    for (int i = 0; i < GV_NUM_LANDMARKS; i++)
        if (!isfinite(f->lm[i].x) || !isfinite(f->lm[i].y))
            return 0;
    return 1;
}

/* While armed: could this frame be the same hand as the last believed one?
 *
 * Size is compared with the last believed frame, not with the size at arming.
 * Over a slide lasting seconds a hand tilts and drifts towards or away from
 * the camera, and its apparent size legitimately wanders; gating against the
 * arming size nearly doubled the dropped slides in the first recording. Frame
 * to frame, a real hand changes size by 0.75x - 1.26x (99% of frames there),
 * while glitches measured 0.01x and 10.8x.
 *
 * The speed test measures from the last BELIEVED frame, not the last frame.
 * That is what stops it locking the slider out: if the hand really has moved,
 * the implied speed drops with every frame that passes, and the hand is
 * believed again within a few frames, well inside the release grace period. */
static int frame_is_plausible(const gv_ctx *ctx, const gv_frame *frame, float raw_x, float raw_y,
                              double t)
{
    const gv_config *cfg = &ctx->cfg;

    float size = gv_hand_scale(frame, cfg->aspect) / ctx->believed_scale;
    if (size < cfg->gate_scale_min || size > cfg->gate_scale_max)
        return 0;

    double dt = t - ctx->believed_t;
    if (dt <= 0.0)
        return 0; /* no time has passed: speed is undefined, so not believed */

    /* Distance in the plane, not along x alone: a hand cannot jump down the
     * frame any more than across it. */
    double moved = hypot((double)(raw_x - ctx->believed_x), (double)(raw_y - ctx->believed_y));
    double speed = moved / (double)ctx->anchor_scale / dt;
    return speed <= (double)cfg->gate_max_speed;
}

static gv_kind active_kind(const gv_ctx *ctx)
{
    return ctx->active != NULL ? ctx->active->kind : GV_KIND_NONE;
}

/* `scale` belongs to the resize gesture; for anything else it reads 1. */
static float active_scale(const gv_ctx *ctx)
{
    return active_kind(ctx) == GV_KIND_RESIZE ? ctx->reported_scale : 1.0f;
}

/* --- the flick ---------------------------------------------------------- */

/* Note one usable frame in the ring buffer. Every usable frame goes in,
 * whatever the state machine is doing with it, so that the moment the state is
 * idle the history needed to judge a flick is already there. */
static void flick_remember(gv_ctx *ctx, const gv_frame *f, double t)
{
    const gv_config *cfg = &ctx->cfg;
    int i = ctx->flick_next;
    ctx->flick_hist[i].t = t;
    ctx->flick_hist[i].x = gv_palm_x(f, cfg->aspect);
    ctx->flick_hist[i].y = gv_palm_y(f);
    ctx->flick_hist[i].scale = gv_hand_scale(f, cfg->aspect);
    ctx->flick_hist[i].open = gv_pose_is_open_palm(f, cfg->aspect, &cfg->pose);
    ctx->flick_next = (i + 1) % GV_FLICK_HISTORY;
    if (ctx->flick_count < GV_FLICK_HISTORY)
        ctx->flick_count++;
}

/* Did the hand just flick?
 *
 * The span judged is "as far back as flick_window_sec reaches", and the test is
 * made on its two ends: an open palm at both, no hole in tracking inside it
 * longer than the release grace (a hand that vanishes and reappears lower down
 * did not necessarily travel there), and the palm centre down by flick_travel
 * hand-lengths with little sideways travel. Taking the whole window rather than
 * the best sub-span inside it is deliberate: it means a frame that is NOT an
 * open palm, or a jump, anywhere in the last third of a second is enough to say
 * no, which is the side to err on for a gesture that closes a window.
 *
 * Units: gv_palm_x multiplies by the aspect and gv_palm_y does not need to, so
 * both are in frame heights, as is the hand scale (see pose.h). Dividing by the
 * hand scale therefore gives hand-lengths on either axis, with no aspect factor
 * left over. */
static int flick_seen(const gv_ctx *ctx, double now)
{
    const gv_config *cfg = &ctx->cfg;
    if (ctx->flick_count < 2)
        return 0;

    int newest = (ctx->flick_next - 1 + GV_FLICK_HISTORY) % GV_FLICK_HISTORY;
    if (!ctx->flick_hist[newest].open)
        return 0;

    /* Walk back through the buffer while the frames are inside the window.
     * `back` counts entries from the newest, so it stops at flick_count and
     * never reads a slot that has not been written (or one left over from
     * before the history was cleared). */
    int oldest = newest;
    for (int back = 2; back <= ctx->flick_count; back++) {
        int i = (ctx->flick_next - back + GV_FLICK_HISTORY) % GV_FLICK_HISTORY;
        int later = (i + 1) % GV_FLICK_HISTORY;
        if (now - ctx->flick_hist[i].t > (double)cfg->flick_window_sec)
            break;
        if (ctx->flick_hist[later].t - ctx->flick_hist[i].t > (double)cfg->release_grace_sec)
            return 0;
        oldest = i;
    }
    if (oldest == newest || !ctx->flick_hist[oldest].open)
        return 0;

    float hl = ctx->flick_hist[oldest].scale;
    if (hl < MIN_HAND_SCALE)
        return 0; /* not a hand; dividing by it would make any wobble a flick */

    /* One hand at both ends, not two. A hand dropping out of the bottom of the
     * frame changes apparent size as it goes; a hand swiped across the picture
     * does not. Both of the false flicks the recordings produced were the first
     * kind - the worse of them grew 1.71x in a third of a second - and the
     * travel below is counted in hand-lengths, a unit that means nothing if the
     * hand was not the same size at each end. This is the gate every armed frame
     * is already judged by (frame_is_plausible), applied across the span
     * rather than frame to frame. Measured: it takes the corpus to zero false
     * flicks, where a longer refractory and a larger travel could not. */
    float size = ctx->flick_hist[newest].scale / hl;
    if (size < cfg->gate_scale_min || size > cfg->gate_scale_max)
        return 0;

    float dy = (ctx->flick_hist[newest].y - ctx->flick_hist[oldest].y) / hl; /* down is +y */
    float dx = fabsf(ctx->flick_hist[newest].x - ctx->flick_hist[oldest].x) / hl;
    return dy >= cfg->flick_travel && dx <= cfg->flick_max_sideways * dy;
}

gv_output gv_update(gv_ctx *ctx, const gv_frame *frame)
{
    const gv_config *cfg = &ctx->cfg;
    const double t = frame->t_sec;
    int changed = 0;

    /* Every decision below compares times. A frame without a usable timestamp
     * cannot be placed in the sequence at all, so it is dropped whole: state
     * and volume are reported as they were. */
    if (!isfinite(t)) {
        gv_output unchanged = { ctx->state, ctx->reported_volume, 0, ctx->axis, active_kind(ctx), active_scale(ctx) };
        return unchanged;
    }

    const int usable = frame_is_usable(frame);
    float raw_x = 0.0f, raw_y = 0.0f;
    /* Remember when a hand was last seen BEFORE this frame, then note this one. */
    ctx->last_hand_before_t = ctx->last_hand_t;
    if (usable)
        ctx->last_hand_t = t;

    /* A gesture that ends on this frame may have a last word (see on_release). */
    int final_changed = 0;
    int let_go_this_frame = 0;
    gv_kind final_kind = GV_KIND_NONE;
    float final_scale = 1.0f;

    /* Which frames count as "the gesture is still going" depends on the state:
     * the strict arm pose until armed, then the lenient stay pose plus the
     * plausibility gate. */
    int good;
    if (ctx->state == GV_ARMED) {
        good = usable && ctx->active->can_stay(frame, cfg);
        if (good) {
            raw_x = gv_palm_x(frame, cfg->aspect);
            raw_y = gv_palm_y(frame);
            good = frame_is_plausible(ctx, frame, raw_x, raw_y, t);
        }
    } else {
        good = usable && find_arming_gesture(ctx, frame) != NULL;
        if (good) {
            raw_x = gv_palm_x(frame, cfg->aspect);
            raw_y = gv_palm_y(frame);
        }
    }

    /* Release first. This also covers a stalled app: if the last good frame is
     * older than the grace period, whatever we were tracking is stale, even if
     * this frame happens to be good again. */
    if (ctx->state != GV_IDLE && t - ctx->last_pose_t > (double)cfg->release_grace_sec) {
        if (ctx->state == GV_ARMED && ctx->active->on_release != NULL) {
            /* The hand was seen after the last good frame: it changed shape.
             * Otherwise it just was not there. (This frame's own hand does not
             * count: last_hand_t was set above, so compare against the frame
             * before by asking whether ANY hand came after the last pose.) */
            int pose_ended = ctx->last_hand_before_t > ctx->last_pose_t;
            if (ctx->active->on_release(ctx, pose_ended)) {
                final_changed = 1;
                final_kind = ctx->active->kind;
                final_scale = ctx->reported_scale;
            }
        }
        let_go_this_frame = (ctx->state == GV_ARMED);
        go_idle(ctx, t);
        /* Having just let go, this frame is judged as a fresh start. */
        good = usable && find_arming_gesture(ctx, frame) != NULL;
        if (good) {
            raw_x = gv_palm_x(frame, cfg->aspect);
            raw_y = gv_palm_y(frame);
        }
    }

    if (good) {
        if (ctx->state == GV_IDLE)
            start_arming(ctx, find_arming_gesture(ctx, frame), t);
        ctx->last_pose_t = t;

        /* The filter runs through ARMING as well, so that by the time we arm
         * the anchor is a settled value rather than one raw sample. */
        float x = gv_filter_apply(&ctx->x_filter, raw_x, t);
        float y = gv_filter_apply(&ctx->y_filter, raw_y, t);

        if (ctx->state == GV_ARMING) {
            ctx->scale_sum += gv_hand_scale(frame, cfg->aspect);
            ctx->scale_count++;

            /* The frame that lets go of one gesture never arms the next, even
             * with no arm hold: callers tell gestures apart by seeing a frame
             * that is not ARMED between them (step.c lets go of one window and
             * takes hold of the next on exactly that). The next frame may arm. */
            if (!let_go_this_frame && t - ctx->arm_started_t >= (double)cfg->arm_hold_sec) {
                float scale = ctx->scale_sum / (float)ctx->scale_count;
                if (scale < MIN_HAND_SCALE) {
                    go_idle(ctx, t);
                } else {
                    ctx->state = GV_ARMED;
                    ctx->anchor_x = x;
                    ctx->anchor_y = y;
                    ctx->anchor_scale = scale;
                    ctx->believed_x = raw_x;
                    ctx->believed_y = raw_y;
                    ctx->believed_scale = gv_hand_scale(frame, cfg->aspect);
                    ctx->believed_t = t;
                    ctx->active->on_armed(ctx, frame, x, y, t);
                }
            }
        } else if (ctx->state == GV_ARMED) {
            const double since_t = ctx->believed_t; /* before it is overwritten below */
            ctx->believed_x = raw_x;
            ctx->believed_y = raw_y;
            ctx->believed_scale = gv_hand_scale(frame, cfg->aspect);
            ctx->believed_t = t;

            changed = ctx->active->on_frame(ctx, frame, x, y, t, since_t);
        }
    }

    /* The flick: a one-shot that never arms. Recorded on every usable frame,
     * so the history is there the moment the state is idle; judged only while
     * idle and only outside the refractory after any release, because lowering
     * the hand at the end of a slide is itself a fast downward motion.
     *
     * `!final_changed` keeps two answers off the same frame. It cannot bite
     * with the default refractory - a gesture that ends on this frame set
     * last_release_t to this frame's time - but flick_refractory_sec may be
     * set to zero, and a released gesture's last word owns its frame. */
    if (usable)
        flick_remember(ctx, frame, t);
    int flicked = 0;
    if (ctx->state == GV_IDLE && usable && !final_changed &&
        t - ctx->last_release_t >= (double)cfg->flick_refractory_sec &&
        t - ctx->last_flick_t   >= (double)cfg->flick_refractory_sec &&
        flick_seen(ctx, t)) {
        ctx->last_flick_t = t;
        /* Forget the motion that fired: as the window slides on it would
         * otherwise still look like a flick for another third of a second. */
        ctx->flick_count = 0;
        flicked = 1;
    }

    gv_output out = { ctx->state, ctx->reported_volume, changed, ctx->axis, active_kind(ctx), active_scale(ctx) };
    if (final_changed) { /* the ended gesture's last word takes this frame's kind, scale and changed */
        out.changed = 1;
        out.kind = final_kind;
        out.scale = final_scale;
    }
    if (flicked) { /* idle, so kind is NONE and scale 1.0: only these two move */
        out.kind = GV_KIND_MINIMIZE;
        out.changed = 1;
    }
    return out;
}
