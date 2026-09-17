/*
 * gestures/volume.c - the volume slider: index and middle up, slide sideways
 * or up and down. Everything here is particular to this gesture; arming,
 * timing, the plausibility gate and release live in gesture.c.
 */
#include <math.h>

#include "../gesture_internal.h"
#include "../pose.h"

/* Decide which way this slide runs, if it is clear yet.
 *
 * The two axes are deliberately not added together. Measured on real slides, a
 * sideways sweep wanders 0.7 - 1.6 hand-lengths vertically, because an arm
 * swings in an arc and because the hand is often still rising into place as it
 * arms. Added in, that would be 13 - 32% of unasked-for volume. So one axis is
 * chosen per slide, by the first movement that is both far enough to be
 * deliberate and clearly ahead of the movement on the other axis; a diagonal
 * chooses nothing, however far it goes. */
static int choose_axis(gv_ctx *ctx, float right, float up)
{
    const gv_config *cfg = &ctx->cfg;
    float ax = fabsf(right), ay = fabsf(up);

    /* The offset is the lock distance itself (signed), not wherever the hand
     * happens to be on the frame that settles it. Frames are discrete, so the
     * hand is always somewhat past the line by then, and after a fast move it
     * can be far past it; using its position would throw that travel away. */
    if (ax >= cfg->axis_lock_hands && ax >= cfg->axis_dominance * ay) {
        ctx->axis = GV_AXIS_X;
        ctx->axis_offset = copysignf(cfg->axis_lock_hands, right);
    } else if (ay >= cfg->axis_lock_hands && ay >= cfg->axis_dominance * ax) {
        ctx->axis = GV_AXIS_Y;
        ctx->axis_offset = copysignf(cfg->axis_lock_hands, up);
    }
    return ctx->axis != GV_AXIS_NONE;
}

/* Is the hand up against the edge of the picture that this slide is heading
 * for? Returns +1 (keep getting louder), -1 (keep getting quieter) or 0.
 *
 * "Heading for" is judged by the travel so far, not by position alone: a hand
 * that armed beside the left edge and is sliding right is not being pushed
 * left, and a raised hand's wrist is near the bottom of the picture most of
 * the time without meaning anything by it.
 *
 * Which landmark counts depends on the edge, because the hand is not a point:
 * sideways it is whichever landmark sticks out furthest; upwards it is the two
 * raised fingertips, which leave the picture first; downwards it is the wrist. */
static int edge_push(const gv_config *cfg, const gv_frame *f, gv_axis axis, float travel)
{
    const float lo = cfg->edge_margin, hi = 1.0f - cfg->edge_margin;

    if (axis == GV_AXIS_X) {
        float min_x = f->lm[0].x, max_x = f->lm[0].x;
        for (int i = 1; i < GV_NUM_LANDMARKS; i++) {
            if (f->lm[i].x < min_x) min_x = f->lm[i].x;
            if (f->lm[i].x > max_x) max_x = f->lm[i].x;
        }
        if (travel > 0.0f && max_x > hi) return +1;
        if (travel < 0.0f && min_x < lo) return -1;
    } else if (axis == GV_AXIS_Y) {
        float tip = fminf(f->lm[GV_INDEX_TIP].y, f->lm[GV_MIDDLE_TIP].y);
        if (travel > 0.0f && tip < lo) return +1;                 /* y grows downwards */
        if (travel < 0.0f && f->lm[GV_WRIST].y > hi) return -1;
    }
    return 0;
}

/* Gain as a function of hand speed (hand-lengths per second): flat at `gain`
 * up to accel_speed_lo, flat at `gain_fast` from accel_speed_hi, a straight
 * line between. The same idea as pointer acceleration on a mouse: one fixed
 * gain is either too twitchy to set a level or too slow to get anywhere. */
static float gain_at(const gv_config *cfg, float speed)
{
    float k = (speed - cfg->accel_speed_lo) / (cfg->accel_speed_hi - cfg->accel_speed_lo);
    k = gv_clamp01(k);
    return cfg->gain + k * (cfg->gain_fast - cfg->gain);
}

static int volume_can_arm(const gv_frame *f, const gv_config *cfg)
{
    return gv_pose_can_arm(f, cfg->aspect, &cfg->pose);
}

static int volume_can_stay(const gv_frame *f, const gv_config *cfg)
{
    return gv_pose_can_stay(f, cfg->aspect, &cfg->pose);
}

static void volume_on_armed(gv_ctx *ctx, const gv_frame *f, float x, float y, double t)
{
    (void)f; (void)x; (void)y;
    ctx->target = ctx->system_volume;
    ctx->slew_t = t;
}

static int volume_on_frame(gv_ctx *ctx, const gv_frame *frame, float x, float y, double t,
                           double since_t)
{
    const gv_config *cfg = &ctx->cfg;
    int changed = 0;

    /* Travel from the anchor in hand-lengths, louder-positive on both
     * axes. Image y grows downwards, so "up" is the negative direction. */
    float right = (x - ctx->anchor_x) / ctx->anchor_scale;
    float up = -(y - ctx->anchor_y) / ctx->anchor_scale;

    if (ctx->axis == GV_AXIS_NONE && choose_axis(ctx, right, up))
        ctx->last_travel = 0.0f; /* travel is counted from the lock distance */

    /* Until an axis is chosen the volume stays where it was. After
     * that, travel is counted from the lock distance, so the slider
     * starts from zero instead of jumping, and a small adjustment
     * stays possible. */
    if (ctx->axis != GV_AXIS_NONE) {
        float travel = (ctx->axis == GV_AXIS_X ? right : up) - ctx->axis_offset;

        /* A 16:9 picture has 1.78x less room top to bottom than side to
         * side, so vertical travel counts that much more: the same
         * fraction of the picture gives the same change on either axis. */
        if (ctx->axis == GV_AXIS_Y)
            travel *= cfg->aspect;

        float moved = travel - ctx->last_travel;
        ctx->last_travel = travel;

        double dt = t - since_t;
        float speed = dt > 0.0 ? fabsf(moved) / (float)dt : 0.0f;
        float step = gain_at(cfg, speed) * moved;

        /* Held against the edge the slide was heading for: keep going.
         * The time credited is capped like the slew limit's, so a run
         * of lost frames cannot turn into one big push. */
        double held = dt > GV_SLEW_MAX_CREDIT_SEC ? GV_SLEW_MAX_CREDIT_SEC : dt;
        if (held > 0.0)
            step += (float)edge_push(cfg, frame, ctx->axis, travel) *
                    copysignf(cfg->edge_rate, cfg->gain) * (float)held;

        /* Clamped as it accumulates: pushing past an end must not
         * build up a dead zone to come back through. */
        ctx->target = gv_clamp01(ctx->target + step);
    }
    float target = ctx->target;

    /* Slew limit: move towards where the hand says, but no faster than
     * max_slew. The allowance scales with the time since the last
     * believed frame, so a slow frame rate does not slow the slider
     * down; but that time is capped, so a run of lost frames cannot
     * bank up permission for one big jump. The moment after a gap is
     * exactly when the detector is most likely to be wrong. */
    double since = t - ctx->slew_t;
    if (since > GV_SLEW_MAX_CREDIT_SEC)
        since = GV_SLEW_MAX_CREDIT_SEC;
    float allowance = cfg->max_slew * (float)since;
    float wanted = target - ctx->reported_volume;
    if (wanted > allowance)
        target = ctx->reported_volume + allowance;
    else if (wanted < -allowance)
        target = ctx->reported_volume - allowance;
    ctx->slew_t = t;

    /* The ends of the range bypass the dead-band: without this the
     * last step to 0 or 1 is swallowed and the slider can never quite
     * reach silence or full volume. Exact float comparison is sound
     * here because clamp01 returns the literals 0.0f and 1.0f. */
    int at_end = (target == 0.0f || target == 1.0f) && target != ctx->reported_volume;

    if (at_end || fabsf(target - ctx->reported_volume) >= cfg->dead_band) {
        ctx->reported_volume = target;
        ctx->system_volume = target;
        changed = 1;
    }
    return changed;
}

static void volume_on_idle(gv_ctx *ctx)
{
    ctx->axis = GV_AXIS_NONE;
}

const gv_gesture gv_gesture_volume = {
    GV_KIND_VOLUME,
    volume_can_arm, volume_can_stay, volume_on_armed, volume_on_frame, volume_on_idle,
};
