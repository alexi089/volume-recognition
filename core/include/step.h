/*
 * step.h - one call per camera frame: the glue between the pure gesture core
 * and the modules with side effects (the volume, the front window).
 *
 * The backends are passed in as function pointers instead of being called
 * directly. In production they are the real CoreAudio and Accessibility
 * functions (gv_system_backends); in tests they are fakes that record calls,
 * so the glue can be tested without touching the machine's volume or anyone's
 * windows. This is C's version of dependency injection.
 */
#ifndef STEP_H
#define STEP_H

#include "gesture.h"
#include "volume.h"
#include "window.h"

typedef struct {
    /* `vol_status (*get)(float *)` reads: get is a pointer to a function
     * taking float* and returning vol_status. */
    vol_status (*get)(float *out);
    vol_status (*set)(float v);
} gv_volume_ops;

typedef struct {
    win_status (*acquire)(void);      /* take hold of the front window */
    win_status (*apply)(float scale); /* relative to its frame at acquire */
    void       (*release)(void);
    /* Minimise the front window. Unlike the three above it takes its own hold
     * and lets go again inside the call: a flick is over in one frame, so
     * there is nothing for the glue to keep. */
    win_status (*minimize)(void);
} gv_window_ops;

typedef struct {
    gv_volume_ops volume;
    gv_window_ops window;
} gv_backends;

/* The real ones: { { vol_get, vol_set },
 *                  { win_acquire, win_apply, win_release, win_minimize } }. */
extern const gv_backends gv_system_backends;

typedef struct {
    gv_output  out;
    vol_status vol; /* result of this frame's volume call; VOL_OK if none failed */
    /* The window side of a resize: while one is armed, how taking hold of the
     * window went, or how this frame's write went if there was one. WIN_OK
     * outside a resize. (Appended: the fields above keep their offsets.) */
    win_status win;
} gv_step_result;

/*
 * Sync the core with the current system volume, feed it the frame, and act on
 * what comes back:
 *   volume:  write the new volume when it changed.
 *   resize:  take hold of the front window on the frame the gesture arms,
 *            write each changed scale, let go when the gesture ends. A gesture
 *            that ends by rolling back sends one last changed scale on its
 *            final frame; that is written BEFORE the window is let go.
 *            Writes are throttled to at most one per cfg.win_write_interval_sec
 *            while armed: a change inside that interval is not dropped, it is
 *            written on the first frame the interval allows; the gesture's
 *            last word (the frame the state goes no longer ARMED) is always
 *            written at once, whatever the interval.
 *   minimise: on the one frame a flick fires, minimise the front window. The
 *            backend takes its own hold, so nothing is acquired or released
 *            around it, and the status is on that frame's result alone.
 */
gv_step_result gv_step(gv_ctx *ctx, const gv_frame *frame, const gv_backends *backends);

/* gv_step with the real backend; what the Python side calls. */
gv_step_result gv_step_system(gv_ctx *ctx, const gv_frame *frame);

/* sizeof the structs that cross the FFI boundary. The Python side compares
 * these with its own ctypes definitions at load time (bridge.py, _check_abi),
 * because a layout mismatch between the two otherwise shows up as silently
 * corrupted values. That is the only place the comparison means anything: a C
 * test comparing these to sizeof would be the same compiler agreeing with
 * itself. */
unsigned gv_abi_frame_size(void);
unsigned gv_abi_config_size(void);
unsigned gv_abi_step_result_size(void);

#endif /* STEP_H */
