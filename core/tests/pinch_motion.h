/*
 * A pinching hand for tests: a blend between two REAL frames of the same hand,
 * the open pinch-ready pose and the fully pinched one. Blending moves every
 * landmark the way a real pinch does; moving only the thumb tip would bend the
 * thumb and look like a different gesture. Include after fixtures.h.
 */
#ifndef GV_TEST_PINCH_MOTION_H
#define GV_TEST_PINCH_MOTION_H

#include "fixtures.h"
#include "pose.h"

/* Move and resize `src` onto `ref` (same wrist-to-knuckle size, same palm
 * centre), so that going from one to the other is a change of shape only. */
static gv_frame aligned_to(const gv_frame *src, const gv_frame *ref)
{
    gv_frame out = *src;
    float k = gv_hand_scale(ref, FIXTURE_REAL_ASPECT) / gv_hand_scale(src, FIXTURE_REAL_ASPECT);
    for (int i = 0; i < GV_NUM_LANDMARKS; i++) {
        out.lm[i].x = src->lm[0].x + (src->lm[i].x - src->lm[0].x) * k;
        out.lm[i].y = src->lm[0].y + (src->lm[i].y - src->lm[0].y) * k;
    }
    float dx = (gv_palm_x(ref, FIXTURE_REAL_ASPECT) - gv_palm_x(&out, FIXTURE_REAL_ASPECT)) / FIXTURE_REAL_ASPECT;
    float dy = gv_palm_y(ref) - gv_palm_y(&out);
    for (int i = 0; i < GV_NUM_LANDMARKS; i++) {
        out.lm[i].x += dx;
        out.lm[i].y += dy;
    }
    return out;
}

/* k = 0: the open pinch-ready hand. k = 1: fully pinched. k < 0: spread wider. */
static gv_frame pinch(float k)
{
    const gv_frame *open = &fixture_real_pinch_open;
    gv_frame closed = aligned_to(&fixture_real_pinch_closed, open);
    gv_frame out = *open;
    for (int i = 0; i < GV_NUM_LANDMARKS; i++) {
        out.lm[i].x = open->lm[i].x + (closed.lm[i].x - open->lm[i].x) * k;
        out.lm[i].y = open->lm[i].y + (closed.lm[i].y - open->lm[i].y) * k;
    }
    return out;
}

#endif
