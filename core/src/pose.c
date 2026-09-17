#include "pose.h"

#include <math.h>
#include <stddef.h> /* size_t */

/* Distance between two landmarks in frame heights (see pose.h on aspect).
 * Taking `const gv_point *` rather than `gv_point` avoids copying 12 bytes per
 * argument and, more to the point, says "I only read this". */
static float dist(const gv_point *a, const gv_point *b, float aspect)
{
    return hypotf((a->x - b->x) * aspect, a->y - b->y);
}

/* The four joints of each finger, base to tip, indexed by gv_finger. A lookup
 * table instead of a switch: adding a finger is one line, and there is no
 * default branch to get wrong. */
static const struct {
    int mcp, pip, dip, tip;
} k_joints[] = {
    [GV_FINGER_INDEX]  = { GV_INDEX_MCP,  GV_INDEX_PIP,  GV_INDEX_DIP,  GV_INDEX_TIP  },
    [GV_FINGER_MIDDLE] = { GV_MIDDLE_MCP, GV_MIDDLE_PIP, GV_MIDDLE_DIP, GV_MIDDLE_TIP },
    [GV_FINGER_RING]   = { GV_RING_MCP,   GV_RING_PIP,   GV_RING_DIP,   GV_RING_TIP   },
    [GV_FINGER_PINKY]  = { GV_PINKY_MCP,  GV_PINKY_PIP,  GV_PINKY_DIP,  GV_PINKY_TIP  },
};

/* Guards the two divisions below. A real finger is never this small; landmarks
 * collapsed onto one point are garbage, and garbage should read as "no
 * evidence" (0) rather than as infinity or NaN. */
#define TINY 1e-6f

float gv_finger_reach(const gv_frame *f, gv_finger finger, float aspect)
{
    const gv_point *wrist = &f->lm[GV_WRIST];
    float to_pip = dist(&f->lm[k_joints[finger].pip], wrist, aspect);
    float to_tip = dist(&f->lm[k_joints[finger].tip], wrist, aspect);
    return to_pip > TINY ? to_tip / to_pip : 0.0f;
}

float gv_finger_straightness(const gv_frame *f, gv_finger finger, float aspect)
{
    const gv_point *mcp = &f->lm[k_joints[finger].mcp];
    const gv_point *pip = &f->lm[k_joints[finger].pip];
    const gv_point *dip = &f->lm[k_joints[finger].dip];
    const gv_point *tip = &f->lm[k_joints[finger].tip];

    /* Straight-line distance over the distance along the bones. By the
     * triangle inequality this can never exceed 1. */
    float along = dist(mcp, pip, aspect) + dist(pip, dip, aspect) + dist(dip, tip, aspect);
    return along > TINY ? dist(mcp, tip, aspect) / along : 0.0f;
}

int gv_finger_is_extended(const gv_frame *f, gv_finger finger, float aspect,
                          const gv_pose_limits *lim)
{
    return gv_finger_reach(f, finger, aspect) > lim->extend_reach &&
           gv_finger_straightness(f, finger, aspect) > lim->extend_straight;
}

int gv_finger_is_curled(const gv_frame *f, gv_finger finger, float aspect,
                        const gv_pose_limits *lim)
{
    return gv_finger_reach(f, finger, aspect) < lim->curl_reach ||
           gv_finger_straightness(f, finger, aspect) < lim->curl_straight;
}

int gv_pose_can_arm(const gv_frame *f, float aspect, const gv_pose_limits *lim)
{
    if (!f->present)
        return 0;
    return  gv_finger_is_extended(f, GV_FINGER_INDEX,  aspect, lim) &&
            gv_finger_is_extended(f, GV_FINGER_MIDDLE, aspect, lim) &&
           !gv_finger_is_extended(f, GV_FINGER_RING,   aspect, lim) &&
           !gv_finger_is_extended(f, GV_FINGER_PINKY,  aspect, lim);
}

int gv_pose_can_stay(const gv_frame *f, float aspect, const gv_pose_limits *lim)
{
    if (!f->present)
        return 0;
    return !gv_finger_is_curled(f, GV_FINGER_INDEX,  aspect, lim) &&
           !gv_finger_is_curled(f, GV_FINGER_MIDDLE, aspect, lim);
}

float gv_thumb_reach(const gv_frame *f, float aspect)
{
    const gv_point *wrist = &f->lm[GV_WRIST];
    float to_mcp = dist(&f->lm[GV_THUMB_MCP], wrist, aspect);
    float to_tip = dist(&f->lm[GV_THUMB_TIP], wrist, aspect);
    return to_mcp > TINY ? to_tip / to_mcp : 0.0f;
}

float gv_thumb_straightness(const gv_frame *f, float aspect)
{
    /* The thumb has one bone fewer than a finger, but MediaPipe still gives it
     * four landmarks (CMC, MCP, IP, TIP), so the same three-segment sum works. */
    const gv_point *cmc = &f->lm[GV_THUMB_CMC];
    const gv_point *mcp = &f->lm[GV_THUMB_MCP];
    const gv_point *ip  = &f->lm[GV_THUMB_IP];
    const gv_point *tip = &f->lm[GV_THUMB_TIP];

    float along = dist(cmc, mcp, aspect) + dist(mcp, ip, aspect) + dist(ip, tip, aspect);
    return along > TINY ? dist(cmc, tip, aspect) / along : 0.0f;
}

int gv_thumb_is_out(const gv_frame *f, float aspect, const gv_pose_limits *lim)
{
    return gv_thumb_reach(f, aspect) > lim->thumb_extend_reach &&
           gv_thumb_straightness(f, aspect) > lim->thumb_extend_straight;
}

int gv_thumb_is_in(const gv_frame *f, float aspect, const gv_pose_limits *lim)
{
    return gv_thumb_reach(f, aspect) < lim->thumb_curl_reach ||
           gv_thumb_straightness(f, aspect) < lim->thumb_curl_straight;
}

float gv_pinch_gap(const gv_frame *f, float aspect)
{
    float scale = gv_hand_scale(f, aspect);
    if (scale < TINY)
        return 0.0f;
    return dist(&f->lm[GV_THUMB_TIP], &f->lm[GV_INDEX_TIP], aspect) / scale;
}

float gv_thumb_clearance(const gv_frame *f, float aspect)
{
    /* The joints a resting thumb lies on. The knuckles (MCP) are left out: they
     * sit in the palm, close to the thumb's own base whatever the thumb does. */
    static const int on_the_fist[] = {
        GV_MIDDLE_PIP, GV_MIDDLE_DIP, GV_MIDDLE_TIP,
        GV_RING_PIP,   GV_RING_DIP,   GV_RING_TIP,
        GV_PINKY_PIP,  GV_PINKY_DIP,  GV_PINKY_TIP,
    };
    float scale = gv_hand_scale(f, aspect);
    if (scale < TINY)
        return 0.0f;
    float nearest = dist(&f->lm[GV_THUMB_TIP], &f->lm[on_the_fist[0]], aspect);
    for (size_t i = 1; i < sizeof on_the_fist / sizeof on_the_fist[0]; i++) {
        float d = dist(&f->lm[GV_THUMB_TIP], &f->lm[on_the_fist[i]], aspect);
        if (d < nearest)
            nearest = d;
    }
    return nearest / scale;
}

int gv_pose_can_arm_resize(const gv_frame *f, float aspect, const gv_pose_limits *lim)
{
    if (!f->present)
        return 0;
    return  gv_thumb_is_out(f, aspect, lim) &&
            gv_finger_is_extended(f, GV_FINGER_INDEX,  aspect, lim) &&
           !gv_finger_is_extended(f, GV_FINGER_MIDDLE, aspect, lim) &&
           !gv_finger_is_extended(f, GV_FINGER_RING,   aspect, lim) &&
           !gv_finger_is_extended(f, GV_FINGER_PINKY,  aspect, lim) &&
            gv_pinch_gap(f, aspect) >= lim->pinch_gap_min &&
            gv_thumb_clearance(f, aspect) >= lim->thumb_clear_min;
}

int gv_pose_can_stay_resize(const gv_frame *f, float aspect, const gv_pose_limits *lim)
{
    if (!f->present)
        return 0;
    return !gv_thumb_is_in(f, aspect, lim) &&
           !gv_finger_is_curled(f, GV_FINGER_INDEX, aspect, lim) &&
           !gv_finger_is_extended(f, GV_FINGER_MIDDLE, aspect, lim);
}

float gv_palm_turn(const gv_frame *f, float aspect)
{
    const gv_point *w = &f->lm[GV_WRIST];
    const gv_point *i = &f->lm[GV_INDEX_MCP];
    const gv_point *p = &f->lm[GV_PINKY_MCP];

    /* Two vectors from the wrist, in undistorted units (see pose.h on aspect). */
    float ax = (i->x - w->x) * aspect, ay = i->y - w->y;
    float bx = (p->x - w->x) * aspect, by = p->y - w->y;

    /* The z component of a x b: positive when b is anticlockwise from a, as
     * seen on screen, negative when clockwise, zero when they line up. Its size
     * is |a||b|sin(angle), so dividing by the hand's size squared leaves a pure
     * number that does not change with distance from the camera. */
    float scale = gv_hand_scale(f, aspect);
    if (scale < TINY)
        return 0.0f;
    return (ax * by - ay * bx) / (scale * scale);
}

float gv_hand_scale(const gv_frame *f, float aspect)
{
    return dist(&f->lm[GV_WRIST], &f->lm[GV_MIDDLE_MCP], aspect);
}

/* The landmarks that make up the palm. File scope, because two functions
 * share it. */
static const int k_palm[] = {
    GV_WRIST, GV_INDEX_MCP, GV_MIDDLE_MCP, GV_RING_MCP, GV_PINKY_MCP,
};
/* sizeof array / sizeof element = element count, computed at compile time. It
 * only works on a true array, never on a pointer to one. */
#define PALM_COUNT ((int)(sizeof k_palm / sizeof k_palm[0]))

float gv_palm_x(const gv_frame *f, float aspect)
{
    float sum = 0.0f;
    for (int i = 0; i < PALM_COUNT; i++)
        sum += f->lm[k_palm[i]].x;
    return (sum / (float)PALM_COUNT) * aspect;
}

float gv_palm_y(const gv_frame *f)
{
    float sum = 0.0f;
    for (int i = 0; i < PALM_COUNT; i++)
        sum += f->lm[k_palm[i]].y;
    return sum / (float)PALM_COUNT; /* y is already a fraction of the height */
}
