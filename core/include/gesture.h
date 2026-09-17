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
 *   IDLE --pose seen--> ARMING --pose held arm_hold_sec--> ARMED
 *     ^                    |                                  |
 *     +---- pose gone for release_grace_sec ------------------+
 */
typedef enum {
    GV_IDLE   = 0,
    GV_ARMING = 1,
    GV_ARMED  = 2
} gv_state;

typedef struct {
    gv_state state;
    float    volume;  /* the volume the system should be at, 0..1 */
    int      changed; /* 1 when `volume` moved enough that the caller should apply it */
} gv_output;

typedef struct {
    float aspect;            /* frame width / height */
    float finger_ratio;      /* finger-extended threshold, see pose.h */
    float arm_hold_sec;      /* pose must be held this long to arm */
    float release_grace_sec; /* pose may be missing this long before release */
    float gain;              /* volume change per hand-length of travel */
    float dead_band;         /* smallest volume change worth reporting */
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

/* cfg may be NULL for the defaults. Returns NULL if memory cannot be allocated. */
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
