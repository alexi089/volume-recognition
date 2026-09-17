#include "pose.h"

#include <math.h>

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
