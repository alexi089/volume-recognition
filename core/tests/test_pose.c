#include "test.h"

#include "fixtures.h"
#include "pose.h"

#define RATIO 1.15f
#define WIDE  (16.0f / 9.0f)

/* --- helpers that build variants of a real hand ------------------------- */

/* Rotate a hand about the frame centre by `deg`, in physical (undistorted)
 * space, for a frame of the given aspect. */
static gv_frame rotated(const gv_frame *src, float aspect, float deg)
{
    gv_frame out = *src; /* struct assignment copies all 21 points */
    float rad = deg * 3.14159265f / 180.0f;
    float c = cosf(rad), s = sinf(rad);
    float cx = 0.5f * aspect, cy = 0.5f;
    for (int i = 0; i < GV_NUM_LANDMARKS; i++) {
        float px = src->lm[i].x * aspect - cx;
        float py = src->lm[i].y - cy;
        out.lm[i].x = (px * c - py * s + cx) / aspect;
        out.lm[i].y = px * s + py * c + cy;
    }
    return out;
}

static gv_frame scaled(const gv_frame *src, float factor)
{
    gv_frame out = *src;
    for (int i = 0; i < GV_NUM_LANDMARKS; i++) {
        out.lm[i].x = 0.5f + (src->lm[i].x - 0.5f) * factor;
        out.lm[i].y = 0.5f + (src->lm[i].y - 0.5f) * factor;
    }
    return out;
}

/* The same physical hand as seen in a frame `aspect` times wider than tall:
 * y is unchanged, x shrinks because it is a fraction of a larger width. */
static gv_frame in_wide_frame(const gv_frame *square_src, float aspect)
{
    gv_frame out = *square_src;
    for (int i = 0; i < GV_NUM_LANDMARKS; i++)
        out.lm[i].x = square_src->lm[i].x / aspect;
    return out;
}

/* --- tests -------------------------------------------------------------- */

static void victory_has_index_and_middle_extended_only(void)
{
    const gv_frame *f = &fixture_victory;
    float a = FIXTURE_VICTORY_ASPECT;
    CHECK(gv_finger_extended(f, GV_FINGER_INDEX, a, RATIO));
    CHECK(gv_finger_extended(f, GV_FINGER_MIDDLE, a, RATIO));
    CHECK(!gv_finger_extended(f, GV_FINGER_RING, a, RATIO));
    CHECK(!gv_finger_extended(f, GV_FINGER_PINKY, a, RATIO));
}

static void victory_is_the_arming_pose(void)
{
    CHECK(gv_pose_is_two_fingers(&fixture_victory, FIXTURE_VICTORY_ASPECT, RATIO));
}

static void one_finger_is_not_the_arming_pose(void)
{
    const gv_frame *f = &fixture_pointing_up;
    CHECK(gv_finger_extended(f, GV_FINGER_INDEX, FIXTURE_POINTING_UP_ASPECT, RATIO));
    CHECK(!gv_pose_is_two_fingers(f, FIXTURE_POINTING_UP_ASPECT, RATIO));
}

static void fist_with_thumb_is_not_the_arming_pose(void)
{
    CHECK(!gv_pose_is_two_fingers(&fixture_thumbs_up, FIXTURE_THUMBS_UP_ASPECT, RATIO));
}

static void absent_hand_is_never_a_pose(void)
{
    gv_frame f = fixture_victory; /* valid landmarks, but flagged absent */
    f.present = 0;
    CHECK(!gv_pose_is_two_fingers(&f, FIXTURE_VICTORY_ASPECT, RATIO));
}

static void pose_survives_rotation(void)
{
    for (int deg = -90; deg <= 90; deg += 45) {
        gv_frame f = rotated(&fixture_victory, FIXTURE_VICTORY_ASPECT, (float)deg);
        CHECK(gv_pose_is_two_fingers(&f, FIXTURE_VICTORY_ASPECT, RATIO));
    }
}

static void pose_survives_distance_from_camera(void)
{
    gv_frame f = scaled(&fixture_victory, 0.4f);
    CHECK(gv_pose_is_two_fingers(&f, FIXTURE_VICTORY_ASPECT, RATIO));
}

static void hand_scale_matches_measured_value(void)
{
    /* 0.160 was measured independently in Python on the same photo. */
    CHECK_NEAR(gv_hand_scale(&fixture_victory, FIXTURE_VICTORY_ASPECT), 0.160f, 0.005);
}

static void hand_scale_follows_apparent_size(void)
{
    gv_frame f = scaled(&fixture_victory, 0.5f);
    float full = gv_hand_scale(&fixture_victory, FIXTURE_VICTORY_ASPECT);
    CHECK(full > 0.1f);
    CHECK_NEAR(gv_hand_scale(&f, FIXTURE_VICTORY_ASPECT), 0.5f * full, 1e-4);
}

/* The reason `aspect` exists: on a 16:9 frame the same physical hand must
 * measure the same size upright and lying on its side. */
static void hand_scale_ignores_rotation_on_a_wide_frame(void)
{
    gv_frame upright = in_wide_frame(&fixture_victory, WIDE);
    gv_frame sideways = rotated(&upright, WIDE, 90.0f);
    float a = gv_hand_scale(&upright, WIDE);
    float b = gv_hand_scale(&sideways, WIDE);
    CHECK(a > 0.1f);
    CHECK_NEAR(b, a, 1e-4);
    /* and it is the same hand as in the square photo */
    CHECK_NEAR(a, gv_hand_scale(&fixture_victory, FIXTURE_VICTORY_ASPECT), 1e-4);
}

static void palm_x_lies_within_the_palm(void)
{
    const gv_frame *f = &fixture_victory;
    float x = gv_palm_x(f, FIXTURE_VICTORY_ASPECT);
    float lo = f->lm[GV_WRIST].x, hi = lo;
    int idx[] = { GV_INDEX_MCP, GV_MIDDLE_MCP, GV_RING_MCP, GV_PINKY_MCP };
    for (int i = 0; i < 4; i++) {
        float v = f->lm[idx[i]].x;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    CHECK(x > lo && x < hi);
}

static void palm_x_is_in_frame_heights(void)
{
    gv_frame wide = in_wide_frame(&fixture_victory, WIDE);
    gv_frame moved = wide;
    for (int i = 0; i < GV_NUM_LANDMARKS; i++)
        moved.lm[i].x += 0.1f; /* a tenth of the frame WIDTH */
    float delta = gv_palm_x(&moved, WIDE) - gv_palm_x(&wide, WIDE);
    CHECK_NEAR(delta, 0.1f * WIDE, 1e-5);
}

int main(void)
{
    fprintf(stderr, "test_pose\n");
    RUN(victory_has_index_and_middle_extended_only);
    RUN(victory_is_the_arming_pose);
    RUN(one_finger_is_not_the_arming_pose);
    RUN(fist_with_thumb_is_not_the_arming_pose);
    RUN(absent_hand_is_never_a_pose);
    RUN(pose_survives_rotation);
    RUN(pose_survives_distance_from_camera);
    RUN(hand_scale_matches_measured_value);
    RUN(hand_scale_follows_apparent_size);
    RUN(hand_scale_ignores_rotation_on_a_wide_frame);
    RUN(palm_x_lies_within_the_palm);
    RUN(palm_x_is_in_frame_heights);
    return TEST_SUMMARY();
}
