/*
 * gesture.c - the arm / slide / release state machine.
 *
 *   IDLE    nothing happening. The arm pose appearing starts ARMING.
 *   ARMING  arm pose seen, not yet trusted. Held for arm_hold_sec -> ARMED.
 *   ARMED   the hand is a slider: volume follows its horizontal position
 *           relative to where it was when it armed.
 *
 * Starting is strict and carrying on is lenient (see pose.h): two fingers up
 * and the other two not to arm, then only "index and middle have not closed"
 * to stay armed. A relaxed hand's ring and pinky wander in and out of looking
 * extended, and demanding the full pose on every frame made slides drop out.
 *
 * Three rules keep accidental movement from changing the volume:
 *   - only frames that show the right pose can move the slider; every other
 *     frame freezes it, so the act of lowering the hand cannot jerk the volume;
 *   - while armed, a frame must also be plausible: the hand about the size it
 *     was, and not further away than a hand can travel in the time since the
 *     last believed frame. Anything else is a detector glitch and is ignored;
 *   - good frames may be missing for release_grace_sec before the slider lets
 *     go, because landmark models lose a frame now and then.
 */
#include "gesture.h"

#include <math.h>
#include <stdlib.h>

#include "filter.h"
#include "pose.h"

/* A hand this small (in frame heights) is noise, not a hand, and dividing
 * travel by it would blow the slider up. */
#define MIN_HAND_SCALE 1e-3f

/* The longest frame gap the slew limit gives credit for: about three frames.
 * With max_slew = 2 that bounds any single step to 0.2 of the range. */
#define SLEW_MAX_CREDIT_SEC 0.1

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

    float  believed_x;      /* raw palm centre of the last frame believed while armed */
    float  believed_y;
    float  believed_scale;  /* and the hand's size in that frame */
    double believed_t;

    double slew_t;          /* when the reported volume was last allowed to move */
};

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
        /* Five hand-lengths for the full range. The first live session used
         * three (a third of the frame width) and was too twitchy to be precise. */
        .gain              = 0.2f,
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
                  c->max_slew > 0.0f && isfinite(c->max_slew);

    return pose_ok && gate_ok &&
           c->aspect > 0.0f && isfinite(c->aspect) &&
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

    const int usable = frame_is_usable(frame);
    float raw_x = 0.0f, raw_y = 0.0f;

    /* Which frames count as "the gesture is still going" depends on the state:
     * the strict arm pose until armed, then the lenient stay pose plus the
     * plausibility gate. */
    int good;
    if (ctx->state == GV_ARMED) {
        good = usable && gv_pose_can_stay(frame, cfg->aspect, &cfg->pose);
        if (good) {
            raw_x = gv_palm_x(frame, cfg->aspect);
            raw_y = gv_palm_y(frame);
            good = frame_is_plausible(ctx, frame, raw_x, raw_y, t);
        }
    } else {
        good = usable && gv_pose_can_arm(frame, cfg->aspect, &cfg->pose);
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
        good = usable && gv_pose_can_arm(frame, cfg->aspect, &cfg->pose);
        if (good) {
            raw_x = gv_palm_x(frame, cfg->aspect);
            raw_y = gv_palm_y(frame);
        }
    }

    if (good) {
        if (ctx->state == GV_IDLE)
            start_arming(ctx, t);
        ctx->last_pose_t = t;

        /* The filter runs through ARMING as well, so that by the time we arm
         * the anchor is a settled value rather than one raw sample. */
        float x = gv_filter_apply(&ctx->x_filter, raw_x, t);

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
                    ctx->believed_x = raw_x;
                    ctx->believed_y = raw_y;
                    ctx->believed_scale = gv_hand_scale(frame, cfg->aspect);
                    ctx->believed_t = t;
                    ctx->slew_t = t;
                }
            }
        } else if (ctx->state == GV_ARMED) {
            ctx->believed_x = raw_x;
            ctx->believed_y = raw_y;
            ctx->believed_scale = gv_hand_scale(frame, cfg->aspect);
            ctx->believed_t = t;

            float hands = (x - ctx->anchor_x) / ctx->anchor_scale;
            float target = clamp01(ctx->anchor_volume + cfg->gain * hands);

            /* Slew limit: move towards where the hand says, but no faster than
             * max_slew. The allowance scales with the time since the last
             * believed frame, so a slow frame rate does not slow the slider
             * down; but that time is capped, so a run of lost frames cannot
             * bank up permission for one big jump. The moment after a gap is
             * exactly when the detector is most likely to be wrong. */
            double since = t - ctx->slew_t;
            if (since > SLEW_MAX_CREDIT_SEC)
                since = SLEW_MAX_CREDIT_SEC;
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
        }
    }

    gv_output out = { ctx->state, ctx->reported_volume, changed };
    return out;
}
