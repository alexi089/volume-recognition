/*
 * gesture_internal.h - what the state machine (gesture.c) and the gestures
 * (the files in gestures/) share. Not part of the library's API: gv_ctx stays
 * an opaque type to everything that goes through gesture.h (app/bridge.py and
 * the ctypes side), while its fields are visible to the few files that
 * implement it.
 *
 * core/tools/tune_flick.c also includes it, for the two frame-level rules at
 * the bottom. That tool re-implements the flick offline so its constants can be
 * swept without rebuilding the core, and then checks itself against gv_update;
 * a check like that is worth nothing if the two sides disagree about which
 * frames are even admissible, so they share that decision rather than each
 * having their own copy of it.
 */
#ifndef GV_GESTURE_INTERNAL_H
#define GV_GESTURE_INTERNAL_H

#include <stddef.h> /* NULL, for a gesture that leaves an optional hook out */

#include "filter.h"
#include "gesture.h"

#define GV_SCALE_HISTORY 32

/* One and a third seconds at 30 fps. The whole flick - the 0.25 s hold plus the
 * 0.6 s drop window that follows it - is 0.85 s, so 26 frames would just cover
 * it and 40 leaves half as much again in hand. Strictly only the drop window is
 * walked (the hold's length is carried forward frame by frame in flick_prev,
 * not recounted from the buffer), so the buffer is generous on purpose rather
 * than exactly sized: above about 66 fps it stops covering even the drop window
 * and the search truncates to whatever it holds - which can only LOSE the frame
 * the drop would have been measured from, so the gesture then fails towards not
 * firing, never towards firing. Raise it with the window. */
#define GV_FLICK_HISTORY 40

/* The longest frame gap the slew limit gives credit for: about three frames.
 * With max_slew = 2 that bounds any single step to 0.2 of the range. */
#define GV_SLEW_MAX_CREDIT_SEC 0.1

struct gv_ctx {
    gv_config cfg;
    gv_state  state;
    const struct gv_gesture *active; /* the gesture arming or armed; NULL while idle */
    gv_filter x_filter;
    gv_filter y_filter;

    float  system_volume;   /* last value given to gv_set_current_volume */
    float  reported_volume; /* last value handed to the caller as `changed` */

    double arm_started_t;   /* when the current ARMING began */
    double last_pose_t;     /* last frame that showed the pose */
    double last_hand_t;     /* last usable frame with a hand in it, in any shape */
    double last_hand_before_t; /* the same, as it stood before the current frame */

    /* hand size is averaged over the arming window: one noisy frame should
     * not set the scale for the whole slide */
    float  scale_sum;
    int    scale_count;

    float  anchor_x;        /* filtered palm centre at the moment of arming */
    float  anchor_y;
    float  anchor_scale;    /* hand-length, in frame heights */

    /* The slider is an accumulator, not a function of position: each believed
     * frame adds gain(speed) * (travel since the last one). That is what lets
     * the gain depend on speed. */
    float  target;          /* where the hand says the volume should be, 0..1 */
    float  last_travel;     /* travel along the chosen axis at the last believed frame */

    float  believed_x;      /* raw palm centre of the last frame believed while armed */
    float  believed_y;
    float  believed_scale;  /* and the hand's size in that frame */
    double believed_t;

    double slew_t;          /* when the reported volume was last allowed to move */

    /* resize (gestures/resize.c) */
    gv_filter gap_filter;   /* smooths the thumb-to-index fingertip gap */
    float  gap_at_arm;      /* that gap, in hand-lengths, at the moment of arming */
    float  reported_scale;  /* last scale handed to the caller; 1.0 at arming */
    /* The unrounded integrator rate control accumulates into; reported_scale
     * only moves to match it once it has moved by scale_dead_band. The two
     * agree again the instant either one is reset (arming, idle, rollback). */
    float  scale_target;
    /* A short history of reported scales, for the rollback on release. A ring
     * buffer: a fixed array plus the index of the next slot to write, wrapping
     * round, so the newest GV_SCALE_HISTORY entries are always there and
     * nothing is ever allocated or shifted. 32 frames is about a second. */
    struct { double t; float scale; } scale_history[GV_SCALE_HISTORY];
    int    scale_history_next;
    int    scale_history_count;

    /* The flick (gesture.c). A ring buffer like scale_history above: every
     * usable frame's palm centre, hand size, hand shape, how far down the
     * picture the hand reached, and how long an open palm has been held nearly
     * still ending at that frame - so that the moment the machine is idle the
     * last second or so of motion is already there to judge. x, y and `low` are
     * in frame heights (see pose.h), so dividing x or y by `scale` gives
     * hand-lengths. `held` is in seconds and is negative on a frame that is not
     * part of such a run. `fist` is asked only of the newest frame; it is not
     * the negation of the open palm the hold wants (pose.h says why). */
    struct { double t; float x, y, scale, low, held; int fist; } flick_hist[GV_FLICK_HISTORY];
    int    flick_next, flick_count;
    /* The previous usable frame, kept OUT of the ring buffer: the hold's length
     * is carried forward from one frame to the next, and clearing the ring when
     * a flick fires must not restart a hold that is still going on. `valid` is
     * 0 until the first usable frame (gv_create zeroes the context). */
    struct { double t; float x, y, held; int valid; } flick_prev;
    double last_release_t;  /* when any gesture last let go (ARMING or ARMED -> IDLE) */
    double last_flick_t;    /* when a flick last fired */

    gv_axis axis;           /* which way this slide runs; NONE until it is clear */
    float   axis_offset;    /* signed lock distance; travel is counted from there */

    /* Not the recogniser's: gv_step's memory between frames (step.c). It lives
     * here because the context is the one object the caller already carries
     * from frame to frame; gesture.c never reads it. gv_create zeroes it. */
    int step_resize_active;  /* a resize is armed and the window was asked for */
    int step_window_held;    /* ... and we got it, so it must be let go */
    int step_acquire_status; /* how asking went (a win_status), repeated while armed */
    double step_last_write_t;  /* frame time of the last window write; -INFINITY on acquire */
    int    step_write_pending; /* a changed scale is waiting for the next allowed frame */
    int    step_minimise_pending; /* a minimise was written; ask on later frames whether it took */
};

/*
 * A gesture is a definition: two poses and three callbacks. gesture.c owns
 * everything gestures have in common (which frames are usable, how long a pose
 * must hold, the plausibility gate, release); a definition supplies only what
 * is particular to it. A struct of function pointers is how C does what other
 * languages call an interface: the state machine calls through the pointers
 * and never needs to know which gesture it is driving.
 */
typedef struct gv_gesture {
    gv_kind kind; /* what gv_output reports while this gesture is arming or armed */
    /* Strict: may this frame start (or continue) arming this gesture? */
    int  (*can_arm)(const gv_frame *f, const gv_config *cfg);
    /* Lenient: while armed, does this frame still show the gesture going on? */
    int  (*can_stay)(const gv_frame *f, const gv_config *cfg);
    /* The moment it arms. x, y: filtered palm centre. Capture anchors here. */
    void (*on_armed)(gv_ctx *ctx, const gv_frame *f, float x, float y, double t);
    /* One believed frame while armed. since_t: time of the previous believed
     * frame. Returns 1 if the value reported to the caller changed. */
    int  (*on_frame)(gv_ctx *ctx, const gv_frame *f, float x, float y, double t, double since_t);
    /* Reset the gesture's own state (called on arming and on release). */
    void (*on_idle)(gv_ctx *ctx);
    /* Optional (may be NULL). Called once as an ARMED gesture ends, before
     * on_idle. pose_ended is 1 if the hand was still in view and changed shape,
     * 0 if it simply was not there any more. Returns 1 if the gesture has a
     * final value to report; it is sent out, with `changed`, on that frame. */
    int  (*on_release)(gv_ctx *ctx, int pose_ended);
} gv_gesture;

extern const gv_gesture gv_gesture_volume;
extern const gv_gesture gv_gesture_resize;

float gv_clamp01(float v);

/* --- what counts as a frame at all -------------------------------------- */

/* MediaPipe should never emit NaN, but gv_update is the boundary where outside
 * data enters and one NaN would poison the filter for good. A frame that fails
 * this is dropped whole. */
int gv_frame_is_usable(const gv_frame *f);

/* Below this, in frame heights, a "hand" is a detector glitch rather than a
 * hand: landmark models now and then collapse one onto a point, and
 * frame_is_plausible's comment records a measured 0.01x. Everything that
 * divides by gv_hand_scale checks this first, because dividing by such a scale
 * turns any wobble into a gesture. */
#define GV_MIN_HAND_SCALE 1e-3f

#endif /* GV_GESTURE_INTERNAL_H */
