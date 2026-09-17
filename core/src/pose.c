#include "pose.h"

#include <math.h>

/* Distance between two landmarks in frame heights (see pose.h on aspect).
 * Taking `const gv_point *` rather than `gv_point` avoids copying 12 bytes per
 * argument and, more to the point, says "I only read this". */
static float dist(const gv_point *a, const gv_point *b, float aspect)
{
    return hypotf((a->x - b->x) * aspect, a->y - b->y);
}

/* The PIP and TIP landmark of each finger, indexed by gv_finger. A lookup
 * table instead of a switch: adding a finger is one line, and there is no
 * default branch to get wrong. */
static const struct {
    int pip, tip;
} k_joints[] = {
    [GV_FINGER_INDEX]  = { GV_INDEX_PIP,  GV_INDEX_TIP  },
    [GV_FINGER_MIDDLE] = { GV_MIDDLE_PIP, GV_MIDDLE_TIP },
    [GV_FINGER_RING]   = { GV_RING_PIP,   GV_RING_TIP   },
    [GV_FINGER_PINKY]  = { GV_PINKY_PIP,  GV_PINKY_TIP  },
};

int gv_finger_extended(const gv_frame *f, gv_finger finger, float aspect, float ratio)
{
    const gv_point *wrist = &f->lm[GV_WRIST];
    float to_tip = dist(&f->lm[k_joints[finger].tip], wrist, aspect);
    float to_pip = dist(&f->lm[k_joints[finger].pip], wrist, aspect);
    return to_tip > ratio * to_pip;
}

int gv_pose_is_two_fingers(const gv_frame *f, float aspect, float ratio)
{
    if (!f->present)
        return 0;
    return  gv_finger_extended(f, GV_FINGER_INDEX,  aspect, ratio) &&
            gv_finger_extended(f, GV_FINGER_MIDDLE, aspect, ratio) &&
           !gv_finger_extended(f, GV_FINGER_RING,   aspect, ratio) &&
           !gv_finger_extended(f, GV_FINGER_PINKY,  aspect, ratio);
}

float gv_hand_scale(const gv_frame *f, float aspect)
{
    return dist(&f->lm[GV_WRIST], &f->lm[GV_MIDDLE_MCP], aspect);
}

float gv_palm_x(const gv_frame *f, float aspect)
{
    static const int k_palm[] = {
        GV_WRIST, GV_INDEX_MCP, GV_MIDDLE_MCP, GV_RING_MCP, GV_PINKY_MCP,
    };
    /* sizeof array / sizeof element = element count, computed at compile
     * time. It only works on a true array, never on a pointer to one. */
    const int n = (int)(sizeof k_palm / sizeof k_palm[0]);

    float sum = 0.0f;
    for (int i = 0; i < n; i++)
        sum += f->lm[k_palm[i]].x;
    return (sum / (float)n) * aspect;
}
