/*
 * gestures/resize.c - pinch to resize: thumb and index out, the other three
 * curled. The window scales by (fingertip gap now) / (gap when it armed), the
 * way pinch-zoom works on a phone. This file turns landmarks into that scale;
 * what is done with it (the Accessibility calls) is a backend's business.
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
    ctx->slew_t = t;
    ctx->scale_history_next = 0;
    ctx->scale_history_count = 0;
    remember_scale(ctx, t);
}

static int resize_on_frame(gv_ctx *ctx, const gv_frame *f, float x, float y, double t, double since_t)
{
    (void)x; (void)y; (void)since_t;
    const gv_config *cfg = &ctx->cfg;

    float gap = gv_filter_apply(&ctx->gap_filter, gv_pinch_gap(f, cfg->aspect), t);
    float target = gap / ctx->gap_at_arm;
    if (target < cfg->scale_min) target = cfg->scale_min;
    if (target > cfg->scale_max) target = cfg->scale_max;

    /* Slew limit, in log space. A scale is a ratio, so "how fast" means octaves
     * (doublings) per second, not units per second: going from 1x to 2x and
     * from 2x to 4x are the same size of change and get the same allowance.
     * As for volume, time across lost frames is credited only up to a cap, so
     * a gap in tracking cannot bank up permission for one big jump. */
    double since = t - ctx->slew_t;
    if (since > GV_SLEW_MAX_CREDIT_SEC)
        since = GV_SLEW_MAX_CREDIT_SEC;
    ctx->slew_t = t;
    float allowance = cfg->scale_max_slew * (float)since;
    float wanted = log2f(target / ctx->reported_scale);
    if (wanted > allowance)
        target = ctx->reported_scale * exp2f(allowance);
    else if (wanted < -allowance)
        target = ctx->reported_scale * exp2f(-allowance);

    /* The ends of the range bypass the dead-band, as they do for volume, and
     * the comparison is exact for the same reason: the clamp above assigns the
     * config values themselves. (After the slew step `target` is only equal to
     * an end if the slew did not shorten it.) */
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
    ctx->reported_scale = rolled;
    return 1;
}

static void resize_on_idle(gv_ctx *ctx)
{
    ctx->reported_scale = 1.0f;
    gv_filter_reset(&ctx->gap_filter);
}

const gv_gesture gv_gesture_resize = {
    GV_KIND_RESIZE,
    resize_can_arm, resize_can_stay, resize_on_armed, resize_on_frame, resize_on_idle,
    resize_on_release,
};
