/*
 * gesture_internal.h - what the state machine (gesture.c) and the gestures
 * (the files in gestures/) share. Private to core/src: nothing outside includes it, so
 * gv_ctx stays an opaque type to callers while its fields are visible to the
 * few files that implement it.
 */
#ifndef GV_GESTURE_INTERNAL_H
#define GV_GESTURE_INTERNAL_H

#include "filter.h"
#include "gesture.h"

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

    gv_axis axis;           /* which way this slide runs; NONE until it is clear */
    float   axis_offset;    /* signed lock distance; travel is counted from there */
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
} gv_gesture;

extern const gv_gesture gv_gesture_volume;

float gv_clamp01(float v);

#endif /* GV_GESTURE_INTERNAL_H */
