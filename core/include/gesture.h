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

#endif /* GESTURE_H */
