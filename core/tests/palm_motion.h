/*
 * A hand travelling across the frame, for tests: a fixture slid by whole
 * hand-lengths. Include after fixtures.h.
 *
 * Distances are in hand-lengths of the OPEN PALM fixture, in frame heights,
 * whichever fixture is being moved - so one motion means the same distance
 * even when the hand changes shape half way through it. (The fist fixture's
 * own hand measures 6% smaller; nothing that uses this turns on the
 * difference.) `dy` is measured DOWN, the direction image y already grows in,
 * because everything the flick cares about is falling.
 *
 * Shared by test_flick.c, which drives the pure core with gv_update, and
 * test_step.c, which drives the glue with gv_step and fake backends: one copy
 * of the arithmetic, a driver loop of each file's own on top of it.
 */
#ifndef GV_TEST_PALM_MOTION_H
#define GV_TEST_PALM_MOTION_H

#include "fixtures.h"
#include "pose.h"

/* One hand-length, in frame heights. */
static float hand_length(void)
{
    return gv_hand_scale(&fixture_real_open_palm, FIXTURE_REAL_ASPECT);
}

/* The fixture, moved by dx hand-lengths right and dy hand-lengths DOWN. */
static gv_frame moved(const gv_frame *hand, float dx, float dy)
{
    gv_frame f = *hand;
    float hl = hand_length();
    for (int j = 0; j < GV_NUM_LANDMARKS; j++) {
        /* Landmark x is a fraction of the WIDTH and y of the HEIGHT, while a
         * hand-length (gv_hand_scale) is in frame heights. Dividing the x step
         * by the aspect leaves both offsets the same physical length. */
        f.lm[j].x += dx * hl / FIXTURE_REAL_ASPECT;
        f.lm[j].y += dy * hl;
    }
    return f;
}

#endif /* GV_TEST_PALM_MOTION_H */
