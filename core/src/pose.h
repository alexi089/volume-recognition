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

/* Thumb tip to the nearest joint (PIP, DIP or tip) of the middle, ring and
 * pinky fingers, in hand-lengths: is the thumb held out, or lying on the fist? */
float gv_thumb_clearance(const gv_frame *f, float aspect);

/* Strict, for starting a resize: thumb out and clear of the curled fingers,
 * index extended, the other three not extended, and the two fingertips far
 * enough apart to start from. The
 * middle finger is what separates this from the volume pose (extended there). */
int gv_pose_can_arm_resize(const gv_frame *f, float aspect, const gv_pose_limits *lim);

/* Lenient, for carrying on: thumb not in, index not curled, middle not
 * extended. The index matters because a full pinch and a fist are nearly the
 * same shape in 2D (measured: thumb reach 1.57 vs 1.27, index curled in both,
 * fingertip gap 0.04 vs 0.07), and a fist must never be read as "shrink to the
 * minimum". The cost is range: near the pinched end the index curls (below a
 * gap of about 0.66 hand-lengths on the real pinch clip), so one pinch reaches
 * roughly a third of the starting size and then lets go; further means
 * re-arming. A fist, an open hand or leaving the frame ends it. */
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

/* --- the open palm, for the minimise flick ------------------------------- */

/* All four fingers extended (long AND straight, as gv_finger_is_extended
 * judges each one) and the thumb out (gv_thumb_is_out). One finger down, or
 * the thumb tucked or bent, is enough to say no: this is the same "confidently
 * extended" bar the volume and resize arm poses use, not its relaxed cousin.
 *
 * This is a POSE ONLY. Like the arm and stay poses above, it says what shape
 * the hand is, never what to do about it: a hand can sit open in front of the
 * camera for as long as it likes (the waving and talking-hands recordings do
 * this constantly) without anything firing. It is what the flick's HOLD is made
 * of - an open palm kept still, which must then be dropped before anything
 * happens (see the design spec); on its own it must never arm, stay, or change
 * anything. */
int gv_pose_is_open_palm(const gv_frame *f, float aspect, const gv_pose_limits *lim);

/* All four fingers confidently CURLED (gv_finger_is_curled judges each one):
 * a closed hand. The thumb is not consulted, so a full pinch counts as one
 * too - the two are nearly the same shape in 2D, and nothing that asks this
 * question cares which it is looking at.
 *
 * It is not the negation of gv_pose_is_open_palm, and that is the whole point.
 * Between the two lies the band where fast motion puts a hand: a fast drop
 * blurs the fingers and they stop reading as "long AND straight" without ever
 * reading as short or bent. The flick wants a confident open palm through its
 * HOLD and only "not closed" at the end of the DROP, because seven of the
 * sixteen downward open-palm motions in the first live session had lost the
 * open palm by their last sample, and only two of those had closed the hand. */
int gv_pose_is_fist(const gv_frame *f, float aspect, const gv_pose_limits *lim);

/* How far down the picture the hand reaches: the LARGEST y of any landmark,
 * y being a fraction of the frame height that grows downwards. Not the palm
 * centre and not one chosen landmark, because which part of the hand leaves
 * the bottom of the frame first depends on how it is held.
 *
 * MediaPipe keeps reporting landmarks for a hand that is partly out of the
 * picture, with coordinates outside 0..1, so a reading above 1.0 means the
 * hand is already over the edge rather than pressed against it. */
float gv_hand_lowest_y(const gv_frame *f);

#endif /* GV_POSE_H */
