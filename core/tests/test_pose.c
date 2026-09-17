#include "test.h"

#include "fixtures.h"
#include "pose.h"

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

/* Fold one finger of a hand flat against its own knuckle: tip onto MCP, DIP
 * onto PIP. Crude, but it is a curled finger by both measurements, and it lets
 * a test change exactly one finger of a real hand. */
static gv_frame with_finger_folded(const gv_frame *src, int mcp)
{
    gv_frame out = *src;
    out.lm[mcp + 3] = src->lm[mcp];     /* TIP -> MCP */
    out.lm[mcp + 2] = src->lm[mcp + 1]; /* DIP -> PIP */
    return out;
}

/* --- tests -------------------------------------------------------------- */

static const gv_pose_limits LIM = { 1.20f, 0.93f, 1.00f, 0.75f };
#define REAL FIXTURE_REAL_ASPECT

static void posed_photo_fingers_are_judged_correctly(void)
{
    const gv_frame *f = &fixture_victory;
    float a = FIXTURE_VICTORY_ASPECT;
    CHECK(gv_finger_is_extended(f, GV_FINGER_INDEX, a, &LIM));
    CHECK(gv_finger_is_extended(f, GV_FINGER_MIDDLE, a, &LIM));
    CHECK(gv_finger_is_curled(f, GV_FINGER_RING, a, &LIM));
    CHECK(gv_finger_is_curled(f, GV_FINGER_PINKY, a, &LIM));
}

/* The measurement that broke the first rule: a loosely curled ring finger has
 * a reach of about 1.12, neither clearly up nor clearly down. */
static void a_relaxed_curl_is_neither_extended_nor_curled(void)
{
    const gv_frame *f = &fixture_real_relaxed_two;
    CHECK_NEAR(gv_finger_reach(f, GV_FINGER_RING, REAL), 1.12f, 0.02);
    CHECK_NEAR(gv_finger_straightness(f, GV_FINGER_RING, REAL), 0.86f, 0.02);
    CHECK(!gv_finger_is_extended(f, GV_FINGER_RING, REAL, &LIM));
    CHECK(!gv_finger_is_curled(f, GV_FINGER_RING, REAL, &LIM));
}

/* A fist seen side-on: the curled fingers point at the camera and project to
 * short straight lines. Straightness alone calls them extended. */
static void a_finger_curled_towards_the_camera_is_not_extended(void)
{
    const gv_frame *f = &fixture_thumbs_up;
    float a = FIXTURE_THUMBS_UP_ASPECT;
    CHECK(gv_finger_straightness(f, GV_FINGER_RING, a) > 0.90f); /* fooled */
    CHECK(gv_finger_reach(f, GV_FINGER_RING, a) < 0.80f);        /* not fooled */
    CHECK(!gv_finger_is_extended(f, GV_FINGER_RING, a, &LIM));
    CHECK(gv_finger_is_curled(f, GV_FINGER_RING, a, &LIM));
}

/* Each finger is checked on its own: with an open palm the pinky alone would
 * block arming, which would hide a rule that forgot to look at the ring. */
static void three_fingers_up_cannot_arm(void)
{
    gv_frame ring_up = with_finger_folded(&fixture_real_open_palm, GV_PINKY_MCP);
    CHECK(gv_finger_is_extended(&ring_up, GV_FINGER_RING, REAL, &LIM));
    CHECK(gv_finger_is_curled(&ring_up, GV_FINGER_PINKY, REAL, &LIM));
    CHECK(!gv_pose_can_arm(&ring_up, REAL, &LIM));

    gv_frame pinky_up = with_finger_folded(&fixture_real_open_palm, GV_RING_MCP);
    CHECK(gv_finger_is_curled(&pinky_up, GV_FINGER_RING, REAL, &LIM));
    CHECK(!gv_pose_can_arm(&pinky_up, REAL, &LIM));

    /* and folding both leaves exactly the arm pose */
    gv_frame two_up = with_finger_folded(&ring_up, GV_RING_MCP);
    CHECK(gv_pose_can_arm(&two_up, REAL, &LIM));
}

/* Extended means long AND straight. With the straightness bar lowered to
 * where the side-on fist clears it easily, reach alone must still say no. */
static void straight_but_short_is_not_extended(void)
{
    gv_pose_limits easy = LIM;
    easy.extend_straight = 0.85f;
    const gv_frame *f = &fixture_thumbs_up;
    float a = FIXTURE_THUMBS_UP_ASPECT;
    CHECK(gv_finger_straightness(f, GV_FINGER_RING, a) > easy.extend_straight);
    CHECK(!gv_finger_is_extended(f, GV_FINGER_RING, a, &easy));
    CHECK(!gv_finger_is_extended(f, GV_FINGER_PINKY, a, &easy));
}

/* ... and the other way round: long but bent. The relaxed ring finger clears a
 * lowered reach bar, and straightness must still say no. */
static void long_but_bent_is_not_extended(void)
{
    gv_pose_limits easy = LIM;
    easy.extend_reach = 1.05f;
    const gv_frame *f = &fixture_real_relaxed_two;
    CHECK(gv_finger_reach(f, GV_FINGER_RING, REAL) > easy.extend_reach);
    CHECK(!gv_finger_is_extended(f, GV_FINGER_RING, REAL, &easy));
}

static void two_fingers_can_arm_posed_tight_or_relaxed(void)
{
    CHECK(gv_pose_can_arm(&fixture_victory, FIXTURE_VICTORY_ASPECT, &LIM));
    CHECK(gv_pose_can_arm(&fixture_real_tight_two, REAL, &LIM));
    CHECK(gv_pose_can_arm(&fixture_real_relaxed_two, REAL, &LIM));
}

static void other_hand_shapes_cannot_arm(void)
{
    CHECK(!gv_pose_can_arm(&fixture_pointing_up, FIXTURE_POINTING_UP_ASPECT, &LIM));
    CHECK(!gv_pose_can_arm(&fixture_thumbs_up, FIXTURE_THUMBS_UP_ASPECT, &LIM));
    CHECK(!gv_pose_can_arm(&fixture_real_fist, REAL, &LIM));
    CHECK(!gv_pose_can_arm(&fixture_real_open_palm, REAL, &LIM));
}

static void staying_armed_only_needs_index_and_middle(void)
{
    CHECK(gv_pose_can_stay(&fixture_real_tight_two, REAL, &LIM));
    CHECK(gv_pose_can_stay(&fixture_real_relaxed_two, REAL, &LIM));
    CHECK(gv_pose_can_stay(&fixture_real_open_palm, REAL, &LIM)); /* ring/pinky ignored */
}

static void closing_the_hand_ends_the_stay_pose(void)
{
    CHECK(!gv_pose_can_stay(&fixture_real_fist, REAL, &LIM));
    CHECK(!gv_pose_can_stay(&fixture_thumbs_up, FIXTURE_THUMBS_UP_ASPECT, &LIM));
    CHECK(!gv_pose_can_stay(&fixture_pointing_up, FIXTURE_POINTING_UP_ASPECT, &LIM)); /* middle down */

    /* either of the two fingers going down is enough */
    gv_frame index_down = with_finger_folded(&fixture_real_tight_two, GV_INDEX_MCP);
    CHECK(!gv_pose_can_stay(&index_down, REAL, &LIM));
    gv_frame middle_down = with_finger_folded(&fixture_real_tight_two, GV_MIDDLE_MCP);
    CHECK(!gv_pose_can_stay(&middle_down, REAL, &LIM));
}

static void absent_hand_is_never_a_pose(void)
{
    gv_frame f = fixture_victory; /* valid landmarks, but flagged absent */
    f.present = 0;
    CHECK(!gv_pose_can_arm(&f, FIXTURE_VICTORY_ASPECT, &LIM));
    CHECK(!gv_pose_can_stay(&f, FIXTURE_VICTORY_ASPECT, &LIM));
}

static void pose_survives_rotation(void)
{
    for (int deg = -90; deg <= 90; deg += 45) {
        gv_frame f = rotated(&fixture_victory, FIXTURE_VICTORY_ASPECT, (float)deg);
        CHECK(gv_pose_can_arm(&f, FIXTURE_VICTORY_ASPECT, &LIM));
        gv_frame r = rotated(&fixture_real_relaxed_two, REAL, (float)deg);
        CHECK(gv_pose_can_arm(&r, REAL, &LIM));
    }
}

static void pose_survives_distance_from_camera(void)
{
    gv_frame f = scaled(&fixture_victory, 0.4f);
    CHECK(gv_pose_can_arm(&f, FIXTURE_VICTORY_ASPECT, &LIM));
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
    RUN(posed_photo_fingers_are_judged_correctly);
    RUN(a_relaxed_curl_is_neither_extended_nor_curled);
    RUN(a_finger_curled_towards_the_camera_is_not_extended);
    RUN(three_fingers_up_cannot_arm);
    RUN(straight_but_short_is_not_extended);
    RUN(long_but_bent_is_not_extended);
    RUN(two_fingers_can_arm_posed_tight_or_relaxed);
    RUN(other_hand_shapes_cannot_arm);
    RUN(staying_armed_only_needs_index_and_middle);
    RUN(closing_the_hand_ends_the_stay_pose);
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
