/*
 * gesture.c - the arm / slide / release state machine.
 *
 *   IDLE    nothing happening. The pose appearing starts ARMING.
 *   ARMING  pose seen, not yet trusted. Held for arm_hold_sec -> ARMED.
 *   ARMED   the hand is a slider: volume follows its horizontal position
 *           relative to where it was when it armed.
 *
 * Two rules keep accidental movement from changing the volume:
 *   - only frames that show the pose can move the slider; every other frame
 *     freezes it, so the act of lowering the hand cannot jerk the volume;
 *   - the pose may drop out for release_grace_sec before the slider lets go,
 *     because landmark models lose a frame now and then.
 */
#include "gesture.h"

#include <math.h>
#include <stdlib.h>

#include "filter.h"
#include "pose.h"

/* A hand this small (in frame heights) is noise, not a hand, and dividing
 * travel by it would blow the slider up. */
#define MIN_HAND_SCALE 1e-3f

struct gv_ctx {
    gv_config cfg;
    gv_state  state;
    gv_filter x_filter;

    float  system_volume;   /* last value given to gv_set_current_volume */
    float  reported_volume; /* last value handed to the caller as `changed` */

    double arm_started_t;   /* when the current ARMING began */
    double last_pose_t;     /* last frame that showed the pose */

    /* hand size is averaged over the arming window: one noisy frame should
     * not set the scale for the whole slide */
    float  scale_sum;
    int    scale_count;

    float  anchor_x;        /* filtered palm x at the moment of arming */
    float  anchor_scale;    /* hand-length, in frame heights */
    float  anchor_volume;   /* system volume at the moment of arming */
};

gv_config gv_default_config(void)
{
    /* Designated initialisers: fields are named, order does not matter, and
     * anything left out would be zero. */
    gv_config cfg = {
        .aspect            = 16.0f / 9.0f,
        .finger_ratio      = 1.15f,
        .arm_hold_sec      = 0.25f,
        .release_grace_sec = 0.15f,
        .gain              = 1.0f / 3.0f, /* three hand-lengths = full range */
        .dead_band         = 0.01f,
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
    return c->aspect > 0.0f && isfinite(c->aspect) &&
           c->finger_ratio > 0.0f && isfinite(c->finger_ratio) &&
           c->arm_hold_sec >= 0.0f && isfinite(c->arm_hold_sec) &&
           c->release_grace_sec >= 0.0f && isfinite(c->release_grace_sec) &&
           isfinite(c->gain) && /* negative is allowed: it reverses the slider */
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
    return ctx;
}

void gv_destroy(gv_ctx *ctx)
{
    free(ctx); /* free(NULL) is defined to do nothing */
}

static float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

void gv_set_current_volume(gv_ctx *ctx, float volume)
{
    if (ctx->state == GV_ARMED || !isfinite(volume))
        return;
    ctx->system_volume = clamp01(volume);
    ctx->reported_volume = ctx->system_volume;
}

static void go_idle(gv_ctx *ctx)
{
    ctx->state = GV_IDLE;
    gv_filter_reset(&ctx->x_filter);
}

static void start_arming(gv_ctx *ctx, double t)
{
    ctx->state = GV_ARMING;
    ctx->arm_started_t = t;
    ctx->scale_sum = 0.0f;
    ctx->scale_count = 0;
    gv_filter_reset(&ctx->x_filter);
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

gv_output gv_update(gv_ctx *ctx, const gv_frame *frame)
{
    const gv_config *cfg = &ctx->cfg;
    const double t = frame->t_sec;
    int changed = 0;

    /* Every decision below compares times. A frame without a usable timestamp
     * cannot be placed in the sequence at all, so it is dropped whole: state
     * and volume are reported as they were. */
    if (!isfinite(t)) {
        gv_output unchanged = { ctx->state, ctx->reported_volume, 0 };
        return unchanged;
    }

    const int has_pose = frame_is_usable(frame) &&
                         gv_pose_is_two_fingers(frame, cfg->aspect, cfg->finger_ratio);

    /* Release first. This also covers a stalled app: if the previous pose
     * frame is older than the grace period, whatever we were tracking is
     * stale, even if this frame happens to show the pose again. */
    if (ctx->state != GV_IDLE && t - ctx->last_pose_t > (double)cfg->release_grace_sec)
        go_idle(ctx);

    if (has_pose) {
        if (ctx->state == GV_IDLE)
            start_arming(ctx, t);
        ctx->last_pose_t = t;

        /* The filter runs through ARMING as well, so that by the time we arm
         * the anchor is a settled value rather than one raw sample. */
        float x = gv_filter_apply(&ctx->x_filter, gv_palm_x(frame, cfg->aspect), t);

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
                    ctx->anchor_scale = scale;
                    ctx->anchor_volume = ctx->system_volume;
                }
            }
        } else if (ctx->state == GV_ARMED) {
            float hands = (x - ctx->anchor_x) / ctx->anchor_scale;
            float target = clamp01(ctx->anchor_volume + cfg->gain * hands);

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
        }
    }

    gv_output out = { ctx->state, ctx->reported_volume, changed };
    return out;
}
