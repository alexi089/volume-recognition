/*
 * gesture.h - public interface of the gesture core.
 *
 * Portable C11 with no dependencies and no side effects: landmarks in, state
 * and a target volume out. It does not know about cameras, MediaPipe, Python
 * or CoreAudio, which is what lets it be tested from recorded files.
 */
#ifndef GESTURE_H
#define GESTURE_H

#define GV_NUM_LANDMARKS 21

/* MediaPipe's hand landmark numbering. Each finger runs base to tip:
 * MCP (knuckle), PIP (middle joint), DIP (last joint), TIP. */
enum {
    GV_WRIST      = 0,
    GV_THUMB_CMC  = 1,  GV_THUMB_MCP  = 2,  GV_THUMB_IP   = 3,  GV_THUMB_TIP  = 4,
    GV_INDEX_MCP  = 5,  GV_INDEX_PIP  = 6,  GV_INDEX_DIP  = 7,  GV_INDEX_TIP  = 8,
    GV_MIDDLE_MCP = 9,  GV_MIDDLE_PIP = 10, GV_MIDDLE_DIP = 11, GV_MIDDLE_TIP = 12,
    GV_RING_MCP   = 13, GV_RING_PIP   = 14, GV_RING_DIP   = 15, GV_RING_TIP   = 16,
    GV_PINKY_MCP  = 17, GV_PINKY_PIP  = 18, GV_PINKY_DIP  = 19, GV_PINKY_TIP  = 20
};

/* x and y are normalised to the frame: 0..1 across the width and 0..1 down the
 * height. That makes them NOT the same physical unit on a non-square frame;
 * see aspect handling in pose.h. z is relative depth and is currently unused. */
typedef struct {
    float x, y, z;
} gv_point;

typedef struct {
    gv_point lm[GV_NUM_LANDMARKS];
    int      present; /* 0 when no hand was found; lm is then ignored */
    double   t_sec;   /* capture time in seconds, from a monotonic clock */
} gv_frame;

/*
 *   IDLE --arm pose seen--> ARMING --arm pose held arm_hold_sec--> ARMED
 *     ^                        |                                     |
 *     +-- arm pose gone -------+        stay pose gone, or frames ---+
 *         for release_grace_sec         implausible, for release_grace_sec
 *
 * The arm pose is strict (two fingers up, the other two not); the stay pose is
 * lenient (index and middle not curled). See src/pose.h.
 */
typedef enum {
    GV_IDLE   = 0,
    GV_ARMING = 1,
    GV_ARMED  = 2
} gv_state;

/* Which way the current slide runs. Chosen by the first clearly dominant
 * movement after arming, and kept until release. */
typedef enum {
    GV_AXIS_NONE = 0, /* not armed, or armed and not yet moved decisively */
    GV_AXIS_X    = 1, /* right is louder */
    GV_AXIS_Y    = 2  /* up is louder */
} gv_axis;

typedef struct {
    gv_state state;
    float    volume;  /* the volume the system should be at, 0..1 */
    int      changed; /* 1 when `volume` moved enough that the caller should apply it */
    gv_axis  axis;
} gv_output;

/* Thresholds for judging one finger; src/pose.h explains the two measurements.
 * The gap between the extend and curl values is the ambiguous band. */
typedef struct {
    float extend_reach;    /* extended needs reach above this ...            */
    float extend_straight; /* ... AND straightness above this                */
    float curl_reach;      /* curled needs reach below this ...              */
    float curl_straight;   /* ... OR straightness below this                 */
} gv_pose_limits;

typedef struct {
    float aspect;            /* frame width / height */
    gv_pose_limits pose;
    float arm_hold_sec;      /* arm pose must be held this long to arm */
    float release_grace_sec; /* stay pose may be missing this long before release */
    /* Volume change per hand-length of travel. It depends on how fast the hand
     * is moving, like a mouse pointer: `gain` at or below accel_speed_lo (slow
     * and precise), `gain_fast` at or above accel_speed_hi, a straight line in
     * between. Set the two gains equal for a plain fixed gain. Speeds are in
     * hand-lengths per second. */
    float gain;
    float gain_fast;
    float accel_speed_lo;
    float accel_speed_hi;
    /* Running out of picture: when part of the hand is within edge_margin (a
     * fraction of the frame) of the edge the slide is heading for, the volume
     * keeps moving that way at edge_rate per second. 0 turns it off. */
    float edge_margin;
    float edge_rate;

    float axis_lock_hands;   /* travel along one axis, in hand-lengths, that picks it ... */
    float axis_dominance;    /* ... provided it is this many times the travel along the other */
    float dead_band;         /* smallest volume change worth reporting */
    float max_slew;          /* fastest the volume may move, per second. A safety
                              * net: whatever the detector does, no jump to full */

    /* While armed, a frame is believed only if the hand it shows could be the
     * same hand as a moment ago. Landmark models now and then emit a frame
     * where the hand collapses to a dot or jumps across the image; one such
     * frame moved the volume by 0.57 in the first live session. */
    float gate_scale_min;    /* hand size, relative to the last believed frame, */
    float gate_scale_max;    /* must stay within [min, max]                     */
    float gate_max_speed;    /* palm speed limit, hand-lengths per second      */

    float filter_min_cutoff; /* One Euro filter parameters, see filter.h */
    float filter_beta;
    float filter_d_cutoff;
} gv_config;

/* The tuned defaults. Start from these and override single fields. */
gv_config gv_default_config(void);

/*
 * gv_ctx is an "opaque type": this header declares that the struct exists but
 * not what is in it, so callers can only hold a pointer and must go through
 * the functions below. That keeps the state machine's internals private, at
 * the cost of a heap allocation: gv_create mallocs, gv_destroy frees, and
 * whoever calls gv_create owns the context until they destroy it.
 */
typedef struct gv_ctx gv_ctx;

/* cfg may be NULL for the defaults. Returns NULL if the config is invalid (a
 * non-finite field; a non-positive aspect, pose limit, gate, slew or filter cutoff;
 * a negative time, dead-band or axis lock distance; an axis dominance below
 * 1; accel_speed_hi not above accel_speed_lo; an edge margin outside
 * [0, 0.5) or a negative edge rate; curl limits above the extend limits; or a scale gate that excludes 1.0)
 * or if memory cannot be allocated. */
gv_ctx *gv_create(const gv_config *cfg);

/* Safe to call with NULL, like free(). */
void gv_destroy(gv_ctx *ctx);

/* Tell the core what the system volume currently is. Call it whenever the
 * slider is not armed, so that arming starts from the real volume even if it
 * was changed with the keyboard in the meantime. Ignored while armed. */
void gv_set_current_volume(gv_ctx *ctx, float volume);

/* Feed one frame; get the state and target volume back. */
gv_output gv_update(gv_ctx *ctx, const gv_frame *frame);

#endif /* GESTURE_H */
