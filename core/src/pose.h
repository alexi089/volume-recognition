/*
 * pose.h - geometry on one frame of hand landmarks (internal header).
 *
 * Every function takes `aspect` = frame width / frame height. Landmark x is a
 * fraction of the width and y a fraction of the height, so on a 16:9 frame one
 * unit of x is 1.78 times longer than one unit of y. Distances are therefore
 * measured as hypot(dx * aspect, dy), i.e. in units of frame height, which is
 * the same physical length in every direction. Without this, a hand's measured
 * size would change as it rotates.
 */
#ifndef GV_POSE_H
#define GV_POSE_H

#include "gesture.h"

typedef enum {
    GV_FINGER_INDEX,
    GV_FINGER_MIDDLE,
    GV_FINGER_RING,
    GV_FINGER_PINKY
} gv_finger;

/* A finger counts as extended when its tip is more than `ratio` times as far
 * from the wrist as its PIP joint is. Measured on real hands: extended fingers
 * give 1.32 - 1.48, curled fingers 0.29 - 0.70. Comparing two distances from
 * the same point makes the test independent of hand size, distance from the
 * camera, and rotation in the image plane. */
int gv_finger_extended(const gv_frame *f, gv_finger finger, float aspect, float ratio);

/* The arming pose: index and middle extended, ring and pinky curled. The thumb
 * is ignored; people hold it every which way. Returns 0 if no hand is present. */
int gv_pose_is_two_fingers(const gv_frame *f, float aspect, float ratio);

/* Wrist to middle-finger knuckle, in frame heights. A stable measure of how
 * big the hand appears, used to make slider travel independent of distance
 * from the camera. */
float gv_hand_scale(const gv_frame *f, float aspect);

/* Horizontal position of the palm centre (mean of the wrist and the four
 * knuckles), in frame heights. Steadier than any fingertip. */
float gv_palm_x(const gv_frame *f, float aspect);

#endif /* GV_POSE_H */
