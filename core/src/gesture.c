/*
 * gesture.c - the recogniser: one state machine for every gesture.
 *
 *   IDLE    nothing happening. A gesture's arm pose appearing starts ARMING it.
 *   ARMING  pose seen, not yet trusted. Held for arm_hold_sec -> ARMED.
 *   ARMED   the gesture is live; its definition turns each believed frame into
 *           a value (gestures/volume.c: a volume; more to come).
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
                               .curl_reach = 1.00f, .curl_straight = 0.75f },
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
                  p->curl_straight <= p->extend_straight;
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

    return pose_ok && gate_ok &&
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

static void go_idle(gv_ctx *ctx)
{
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

gv_output gv_update(gv_ctx *ctx, const gv_frame *frame)
{
    const gv_config *cfg = &ctx->cfg;
    const double t = frame->t_sec;
    int changed = 0;

    /* Every decision below compares times. A frame without a usable timestamp
     * cannot be placed in the sequence at all, so it is dropped whole: state
     * and volume are reported as they were. */
    if (!isfinite(t)) {
        gv_output unchanged = { ctx->state, ctx->reported_volume, 0, ctx->axis, active_kind(ctx), 1.0f };
        return unchanged;
    }

    const int usable = frame_is_usable(frame);
    float raw_x = 0.0f, raw_y = 0.0f;

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
        go_idle(ctx);
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

            if (t - ctx->arm_started_t >= (double)cfg->arm_hold_sec) {
                float scale = ctx->scale_sum / (float)ctx->scale_count;
                if (scale < MIN_HAND_SCALE) {
                    go_idle(ctx);
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

    gv_output out = { ctx->state, ctx->reported_volume, changed, ctx->axis, active_kind(ctx), 1.0f };
    return out;
}
