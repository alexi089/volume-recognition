/*
 * pose.h - geometry on one frame of hand landmarks (internal header).
 *
 * Every function takes `aspect` = frame width / frame height. Landmark x is a
 * fraction of the width and y a fraction of the height, so on a 16:9 frame one
 * unit of x is 1.78 times longer than one unit of y. Distances are therefore
 * measured as hypot(dx * aspect, dy), i.e. in units of frame height, which is
 * the same physical length in every direction. Without this, a hand's measured
 * size would change as it rotates.
 *
 * HOW A FINGER IS JUDGED
 *
 * Two measurements, because each one alone is fooled by something real:
 *
 *   reach         dist(tip, wrist) / dist(PIP, wrist). Above 1 the tip is
 *                 further out than the middle joint. Fooled by a RELAXED curl:
 *                 a loosely curled ring finger reads 1.1 - 1.17, close to an
 *                 extended finger's 1.2 - 1.5.
 *   straightness  dist(MCP, tip) / (length of the three bones). 1.0 is a
 *                 straight finger. Fooled by a finger curled TOWARDS the
 *                 camera, which projects to a short but straight line: a fist
 *                 seen side-on reads 0.9.
 *
 * So a finger is confidently EXTENDED only when it is long AND straight, and
 * confidently CURLED when it is short OR bent. Between the two lies a band
 * where a relaxed hand spends a lot of its time, and the pose rules below are
 * written so that nothing depends on which side of a single line a finger in
 * that band happens to fall.
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

float gv_finger_reach(const gv_frame *f, gv_finger finger, float aspect);
float gv_finger_straightness(const gv_frame *f, gv_finger finger, float aspect);

/* Long AND straight. */
int gv_finger_is_extended(const gv_frame *f, gv_finger finger, float aspect,
                          const gv_pose_limits *lim);

/* Short OR bent. Not the negation of is_extended: a finger in the ambiguous
 * band is neither. */
int gv_finger_is_curled(const gv_frame *f, gv_finger finger, float aspect,
                        const gv_pose_limits *lim);

/* Strict, for starting: index and middle confidently extended, ring and pinky
 * NOT confidently extended. The thumb is ignored. "Not extended" rather than
 * "curled" for ring and pinky is deliberate: a relaxed curl sits in the
 * ambiguous band and must still be able to arm, while an open palm must not. */
int gv_pose_can_arm(const gv_frame *f, float aspect, const gv_pose_limits *lim);

/* Lenient, for carrying on: index and middle not confidently curled, whatever
 * ring and pinky are doing. Closing the hand ends it. Requiring the full pose
 * on every frame is what made the first version drop out mid-slide. */
int gv_pose_can_stay(const gv_frame *f, float aspect, const gv_pose_limits *lim);

/* --- the thumb, and the resize poses ------------------------------------ */

/* As gv_finger_reach / gv_finger_straightness, for the thumb: reach is tip to
 * wrist over knuckle (MCP) to wrist; straightness runs from the base joint
 * (CMC) to the tip. Tip-to-index-knuckle distance looked like the obvious
 * measure of "thumb out" and is not: it is LARGER with the thumb tucked across
 * the curled fingers than in an open pinch. These two were measured instead. */
float gv_thumb_reach(const gv_frame *f, float aspect);
float gv_thumb_straightness(const gv_frame *f, float aspect);
int   gv_thumb_is_out(const gv_frame *f, float aspect, const gv_pose_limits *lim); /* long AND straight */
int   gv_thumb_is_in(const gv_frame *f, float aspect, const gv_pose_limits *lim);  /* short OR bent */

/* Thumb tip to index tip, in hand-lengths: what the resize gesture measures. */
float gv_pinch_gap(const gv_frame *f, float aspect);

/* Strict, for starting a resize: thumb out, index extended, the other three
 * not extended, and the two fingertips far enough apart to start from. The
 * middle finger is what separates this from the volume pose (extended there). */
int gv_pose_can_arm_resize(const gv_frame *f, float aspect, const gv_pose_limits *lim);

/* Lenient, for carrying on: thumb not in, and middle not extended. The index
 * finger is deliberately absent: pinching curls it (its reach falls to 0.57),
 * and a rule that needed it "not curled" would let go exactly when the user
 * pinches. Tucking the thumb (a fist) or opening the hand ends it. */
int gv_pose_can_stay_resize(const gv_frame *f, float aspect, const gv_pose_limits *lim);

/* Which way the hand is turned, as one signed number: the 2D cross product of
 * (index knuckle - wrist) and (pinky knuckle - wrist), divided by the hand's
 * size squared. Its SIGN says on which side of the hand the thumb is, which
 * for a given hand (left or right) says whether the palm or the back faces the
 * camera; its size says how flat-on the hand is (near zero = seen edge-on).
 * Independent of hand size, position, and rotation in the image plane.
 * Turning the hand over, or swapping left for right, flips the sign; this
 * function does not know which hand it is looking at. */
float gv_palm_turn(const gv_frame *f, float aspect);

/* Wrist to middle-finger knuckle, in frame heights. A stable measure of how
 * big the hand appears, used to make slider travel independent of distance
 * from the camera. */
float gv_hand_scale(const gv_frame *f, float aspect);

/* Position of the palm centre (mean of the wrist and the four knuckles), in
 * frame heights. Steadier than any fingertip. x drives the slider; y is only
 * used to judge whether a frame is plausible. */
float gv_palm_x(const gv_frame *f, float aspect);
float gv_palm_y(const gv_frame *f);

#endif /* GV_POSE_H */
