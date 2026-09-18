#include "step.h"

#include <math.h> /* INFINITY */

#include "gesture_internal.h" /* the step_ fields of gv_ctx */

gv_step_result gv_step(gv_ctx *ctx, const gv_frame *frame, const gv_backends *backends)
{
    const gv_volume_ops *volume = &backends->volume;
    const gv_window_ops *window = &backends->window;
    gv_step_result r = { { GV_IDLE, 0.0f, 0, GV_AXIS_NONE, GV_KIND_NONE, 1.0f }, VOL_OK, WIN_OK };

    /* 1. Tell the core where the system volume is, so arming starts from the
     *    truth even if the volume keys were pressed since the last slide. The
     *    core ignores this while armed; asking anyway keeps this function free
     *    of any knowledge about the core's state. */
    float current;
    vol_status get_status = volume->get(&current);
    if (get_status == VOL_OK)
        gv_set_current_volume(ctx, current);

    /* 2. The pure part. */
    r.out = gv_update(ctx, frame);

    /* 3. Apply. `changed` belongs to whichever gesture is active, so look at
     *    the kind before deciding what changed.
     *
     *    Volume: a failed write is the more useful thing to report on a frame
     *    that wrote; otherwise report how the read went. */
    if (r.out.changed && r.out.kind == GV_KIND_VOLUME)
        r.vol = volume->set(r.out.volume);
    else
        r.vol = get_status;

    /*    Resize: take hold of the window on the frame the gesture arms, and
     *    only then. Asking once means a window that cannot be had is not
     *    asked for thirty times a second. */
    const int armed_resize = r.out.state == GV_ARMED && r.out.kind == GV_KIND_RESIZE;
    if (armed_resize && !ctx->step_resize_active) {
        ctx->step_resize_active = 1;
        ctx->step_acquire_status = (int)window->acquire();
        ctx->step_window_held = (ctx->step_acquire_status == WIN_OK);
        /* -INFINITY, not 0: the first change after acquiring must write at
         * once, whatever win_write_interval_sec is, not wait out an interval
         * measured from a time before this gesture existed. */
        ctx->step_last_write_t = -INFINITY;
        ctx->step_write_pending = 0;
    }
    /* Whether the resize path speaks for `r.win` on this frame. Captured here
     * because the release block below clears step_resize_active, and the frame
     * a resize ends on still carries that resize's last word. */
    const int resize_reported = ctx->step_resize_active;
    if (ctx->step_resize_active) {
        /* Without a window, keep saying why for as long as the hand is up. */
        r.win = (win_status)ctx->step_acquire_status;
        /* Not `armed_resize &&`: a resize that rolls back says its last word
         * on the frame it goes idle, and the window must hear it. No check of
         * the kind is needed: from the frame a resize arms to the frame it
         * ends, the only `changed` the core can report is that resize's. */
        /* Writes are throttled: one costs about 14 ms and dropped the camera
         * loop to 16 fps when done every frame. A change inside the interval
         * is not lost, it is written on the first frame the interval allows;
         * a gesture's last word (state no longer ARMED) is written at once. */
        if (ctx->step_window_held && r.out.changed)
            ctx->step_write_pending = 1;
        const double t = frame->t_sec;
        const int due = t - ctx->step_last_write_t >= (double)ctx->cfg.win_write_interval_sec || !armed_resize;
        if (ctx->step_window_held && ctx->step_write_pending && due) {
            r.win = window->apply(r.out.scale);
            ctx->step_last_write_t = t;
            ctx->step_write_pending = 0;
        }
    }
    /*    Let go AFTER that last write, never before. */
    if (ctx->step_resize_active && !armed_resize) {
        if (ctx->step_window_held)
            window->release();
        ctx->step_resize_active = 0;
        ctx->step_window_held = 0; /* step_acquire_status is only read while active, and set afresh then */
    }

    /*    Minimise: a flick fires on one frame and is over, so the gesture has
     *    no state to arm, nothing to hold and nothing to let go. The backend
     *    finds the front window itself. Checking the kind as well as `changed`
     *    matters here in a way it does not inside the resize block above: this
     *    runs on every frame, including the ones where the `changed` on the
     *    result belongs to a volume slide or a resize. */
    if (r.out.kind == GV_KIND_MINIMIZE && r.out.changed) {
        r.win = window->minimize();
        /* The write is accepted long before the window reaches the Dock (about
         * half a second), and waiting for it here would freeze the camera. So
         * the question "did it actually go?" is carried into the frames that
         * follow. Only a write that succeeded has anything to wait for. */
        ctx->step_minimise_pending = (r.win == WIN_OK);
    } else if (ctx->step_minimise_pending && !resize_reported) {
        /* Not on a frame the resize spoke for. Both can be true at once - a
         * minimise settles in about 0.6 s and a resize arms in arm_hold_sec -
         * and then the resize's status wins, because a window that cannot be
         * resized is something the person can act on while a minimise still
         * on its way is not. The minimise is not abandoned by losing a frame:
         * the flag stays set and the asking resumes once the resize is over. */
        r.win = window->minimize_settled();
        /* WIN_PENDING is the one answer that means "ask me again"; anything
         * else, good or bad, is the end of it. */
        if (r.win != WIN_PENDING)
            ctx->step_minimise_pending = 0;
    }

    return r;
}

/* `unsigned` is plenty for a struct size and maps to a plain c_uint in ctypes;
 * size_t would work too but differs in width between platforms. */
unsigned gv_abi_frame_size(void)       { return (unsigned)sizeof(gv_frame); }
unsigned gv_abi_config_size(void)      { return (unsigned)sizeof(gv_config); }
unsigned gv_abi_step_result_size(void) { return (unsigned)sizeof(gv_step_result); }
