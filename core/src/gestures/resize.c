/*
 * gestures/resize.c - pinch to resize: thumb and index out, the other three
 * curled. Rate control, like a joystick: how far the fingertip gap sits from
 * the gap at arming sets the SPEED the window resizes at, not the size
 * itself. A hand at rest (inside the dead zone) moves nothing; growing needs
 * no pinch first, since offset - not gap ratio - drives the rate, and either
 * sign of offset is just as reachable from any arming gap. This file turns
 * landmarks into that speed and integrates it into a scale; what is done with
 * the scale (the Accessibility calls) is a backend's business.
 */
#include <math.h>

#include "../gesture_internal.h"
#include "../pose.h"

static int resize_can_arm(const gv_frame *f, const gv_config *cfg)
{
    return gv_pose_can_arm_resize(f, cfg->aspect, &cfg->pose);
}

static int resize_can_stay(const gv_frame *f, const gv_config *cfg)
{
    return gv_pose_can_stay_resize(f, cfg->aspect, &cfg->pose);
}

static void remember_scale(gv_ctx *ctx, double t)
{
    ctx->scale_history[ctx->scale_history_next].t = t;
    ctx->scale_history[ctx->scale_history_next].scale = ctx->reported_scale;
    ctx->scale_history_next = (ctx->scale_history_next + 1) % GV_SCALE_HISTORY;
    if (ctx->scale_history_count < GV_SCALE_HISTORY)
        ctx->scale_history_count++;
}

static void resize_on_armed(gv_ctx *ctx, const gv_frame *f, float x, float y, double t)
{
    (void)x; (void)y;
    /* The filter starts here, so its first output IS this frame's gap, and the
     * scale is exactly 1 at the moment of arming. The arm pose guarantees the
     * gap is at least pinch_gap_min, so the division in on_frame is safe. */
    gv_filter_reset(&ctx->gap_filter);
    ctx->gap_at_arm = gv_filter_apply(&ctx->gap_filter, gv_pinch_gap(f, ctx->cfg.aspect), t);
    ctx->reported_scale = 1.0f;
    ctx->scale_target = 1.0f;
    ctx->slew_t = t;
    ctx->scale_history_next = 0;
    ctx->scale_history_count = 0;
    remember_scale(ctx, t);
}

static int resize_on_frame(gv_ctx *ctx, const gv_frame *f, float x, float y, double t, double since_t)
{
    (void)x; (void)y; (void)since_t;
    const gv_config *cfg = &ctx->cfg;

    /* Rate control, like a joystick: how far the fingers sit from where they
     * were at arming sets how FAST the size changes, not the size itself. So
     * a few centimetres of finger travel reach any size, growing needs no
     * pinch first, and a hand at rest (inside the dead zone) moves nothing,
     * whatever the fingertip tracking is doing. */
    float gap = gv_filter_apply(&ctx->gap_filter, gv_pinch_gap(f, cfg->aspect), t);
    float offset = gap / ctx->gap_at_arm - 1.0f;
    float beyond = fabsf(offset) - cfg->resize_dead_zone;
    float rate = 0.0f;                                     /* octaves per second */
    if (beyond > 0.0f) {
        rate = cfg->resize_rate_gain * beyond;
        if (rate > cfg->resize_rate_max)
            rate = cfg->resize_rate_max;
        if (offset < 0.0f)
            rate = -rate;
    }

    /* Time since the last frame, credited only up to a cap: a gap in tracking
     * cannot bank up permission for one big jump, whatever rate is in force at
     * the moment tracking resumes. */
    double since = t - ctx->slew_t;
    if (since > GV_SLEW_MAX_CREDIT_SEC)
        since = GV_SLEW_MAX_CREDIT_SEC;
    ctx->slew_t = t;

    /* Integrate in log space and clamp. `scale_target` tracks the unrounded
     * value; `reported_scale` is what the caller last heard, and only catches
     * up once the two have drifted apart by scale_dead_band. */
    float target = ctx->scale_target * exp2f(rate * (float)since);
    if (target < cfg->scale_min) target = cfg->scale_min;
    if (target > cfg->scale_max) target = cfg->scale_max;
    ctx->scale_target = target;

    /* The ends of the range bypass the dead-band, as they do for volume, and
     * the comparison is exact for the same reason: the clamp above assigns the
     * config values themselves. */
    int at_end = (target == cfg->scale_min || target == cfg->scale_max) && target != ctx->reported_scale;

    int changed = 0;
    if (at_end || fabsf(target / ctx->reported_scale - 1.0f) >= cfg->scale_dead_band) {
        ctx->reported_scale = target;
        changed = 1;
    }
    remember_scale(ctx, t);
    return changed;
}

/*
 * Closing the hand is how every gesture here ends, and for this one it is a
 * trap: on the way to a fist the fingertips pass through every smaller gap, so
 * the last few good frames shrink the window before the pose reads as over.
 * When the gesture ends that way, go back to what was reported
 * release_rollback_sec before the last good frame, which is before the closing
 * motion began. A hand that simply leaves the picture is not suspect, and
 * nothing is undone.
 */
static int resize_on_release(gv_ctx *ctx, int pose_ended)
{
    if (!pose_ended || ctx->scale_history_count == 0)
        return 0;

    double cutoff = ctx->last_pose_t - (double)ctx->cfg.release_rollback_sec;
    /* Walk back from the newest entry to the first one old enough. If the
     * whole gesture was shorter than the rollback, that is its oldest entry:
     * the scale at arming. */
    float rolled = ctx->reported_scale;
    for (int back = 1; back <= ctx->scale_history_count; back++) {
        int i = (ctx->scale_history_next - back + GV_SCALE_HISTORY) % GV_SCALE_HISTORY;
        rolled = ctx->scale_history[i].scale;
        if (ctx->scale_history[i].t <= cutoff)
            break;
    }
    if (rolled == ctx->reported_scale)
        return 0;
    /* The integrator must land exactly where the reported value does: if only
     * reported_scale moved, the next arming's on_armed reset would be fine,
     * but nothing else reads scale_target between here and idle, so leaving
     * it stale would just be a landmine for the next person to touch this. */
    ctx->reported_scale = rolled;
    ctx->scale_target = rolled;
    return 1;
}

static void resize_on_idle(gv_ctx *ctx)
{
    ctx->reported_scale = 1.0f;
    ctx->scale_target = 1.0f;
    gv_filter_reset(&ctx->gap_filter);
}

const gv_gesture gv_gesture_resize = {
    GV_KIND_RESIZE,
    resize_can_arm, resize_can_stay, resize_on_armed, resize_on_frame, resize_on_idle,
    resize_on_release,
};
