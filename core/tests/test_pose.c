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

static const gv_pose_limits LIM = {
    .extend_reach = 1.20f, .extend_straight = 0.93f, .curl_reach = 1.00f, .curl_straight = 0.75f,
    .thumb_extend_reach = 1.05f, .thumb_extend_straight = 0.67f,
    .thumb_curl_reach = 0.55f, .thumb_curl_straight = 0.45f,
    .pinch_gap_min = 0.73f,
};
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

/* Flip a hand left to right. In 2D that is what turning the same hand over
 * looks like: the outline is the same, the thumb is on the other side. */
static gv_frame mirrored(const gv_frame *src)
{
    gv_frame out = *src;
    for (int i = 0; i < GV_NUM_LANDMARKS; i++)
        out.lm[i].x = 1.0f - src->lm[i].x;
    return out;
}

/* --- the thumb, and the resize poses ------------------------------------ */

static void the_thumb_reads_out_when_out_and_in_when_tucked(void)
{
    CHECK_NEAR(gv_thumb_reach(&fixture_real_pinch_open, REAL), 1.81f, 0.10);
    CHECK_NEAR(gv_thumb_straightness(&fixture_real_pinch_open, REAL), 0.97f, 0.03);
    CHECK(gv_thumb_is_out(&fixture_real_pinch_open, REAL, &LIM));
    CHECK(!gv_thumb_is_in(&fixture_real_pinch_open, REAL, &LIM));

    CHECK(gv_thumb_reach(&fixture_real_pointing_thumb_in, REAL) < 0.55f);
    CHECK(gv_thumb_is_in(&fixture_real_pointing_thumb_in, REAL, &LIM));
    CHECK(!gv_thumb_is_out(&fixture_real_pointing_thumb_in, REAL, &LIM));

    /* fully pinched, the thumb is still out: that is what the stay pose rests on */
    CHECK(gv_thumb_is_out(&fixture_real_pinch_closed, REAL, &LIM));
}

static void the_pinch_ready_hand_can_arm_a_resize(void)
{
    CHECK(gv_pose_can_arm_resize(&fixture_real_pinch_open, REAL, &LIM));
}

static void other_hand_shapes_cannot_arm_a_resize(void)
{
    CHECK(!gv_pose_can_arm_resize(&fixture_real_pointing_thumb_in, REAL, &LIM)); /* thumb in */
    CHECK(!gv_pose_can_arm_resize(&fixture_real_tight_two, REAL, &LIM));         /* middle up: that is volume */
    CHECK(!gv_pose_can_arm_resize(&fixture_real_relaxed_two, REAL, &LIM));
    CHECK(!gv_pose_can_arm_resize(&fixture_real_open_palm, REAL, &LIM));
    CHECK(!gv_pose_can_arm_resize(&fixture_real_fist, REAL, &LIM));
    CHECK(!gv_pose_can_arm_resize(&fixture_real_pinch_closed, REAL, &LIM));      /* gap too small to start from */
    CHECK(!gv_pose_can_arm_resize(&fixture_victory, FIXTURE_VICTORY_ASPECT, &LIM));
    CHECK(!gv_pose_can_arm_resize(&fixture_thumbs_up, FIXTURE_THUMBS_UP_ASPECT, &LIM)); /* thumb out, index curled */
}

/* One hand shape, one gesture. If a shape could arm two, which one it armed
 * would depend on the order of a table, and no user could predict it. */
static void no_hand_shape_arms_two_gestures(void)
{
    const gv_frame *all[] = {
        &fixture_victory, &fixture_pointing_up, &fixture_thumbs_up, &fixture_real_relaxed_two,
        &fixture_real_tight_two, &fixture_real_fist, &fixture_real_open_palm, &fixture_real_pinch_open,
        &fixture_real_pinch_closed, &fixture_real_pointing_thumb_in,
    };
    const float asp[] = { 1.0f, 1.0f, 1.0f, REAL, REAL, REAL, REAL, REAL, REAL, REAL };
    for (int i = 0; i < 10; i++)
        CHECK(!(gv_pose_can_arm(all[i], asp[i], &LIM) && gv_pose_can_arm_resize(all[i], asp[i], &LIM)));
}

static void a_pinched_hand_can_stay_but_a_fist_or_an_open_hand_cannot(void)
{
    CHECK(gv_pose_can_stay_resize(&fixture_real_pinch_open, REAL, &LIM));
    CHECK(gv_pose_can_stay_resize(&fixture_real_pinch_closed, REAL, &LIM)); /* index curled by the pinch: still fine */
    CHECK(!gv_pose_can_stay_resize(&fixture_real_pointing_thumb_in, REAL, &LIM)); /* thumb tucked */
    CHECK(!gv_pose_can_stay_resize(&fixture_real_open_palm, REAL, &LIM));         /* middle extended */

    gv_frame gone = fixture_real_pinch_open;
    gone.present = 0;
    CHECK(!gv_pose_can_stay_resize(&gone, REAL, &LIM));
    CHECK(!gv_pose_can_arm_resize(&gone, REAL, &LIM));
}

/* The real pinch-ready hand with ONE landmark moved, so a test can change one
 * thing at a time. */
static gv_frame with_thumb_tip_towards_index(const gv_frame *src, float keep)
{
    gv_frame out = *src; /* keep = 1 leaves it; 0.2 leaves a fifth of the gap */
    out.lm[GV_THUMB_TIP].x = src->lm[GV_INDEX_TIP].x + (src->lm[GV_THUMB_TIP].x - src->lm[GV_INDEX_TIP].x) * keep;
    out.lm[GV_THUMB_TIP].y = src->lm[GV_INDEX_TIP].y + (src->lm[GV_THUMB_TIP].y - src->lm[GV_INDEX_TIP].y) * keep;
    return out;
}

static gv_frame with_thumb_bent(const gv_frame *src, float aspect)
{
    /* Push the thumb's middle joint a hand-length out to the side of the
     * thumb's own line: the tip stays where it was (so reach is unchanged) and
     * the path along the bones gets much longer (so straightness collapses). */
    gv_frame out = *src;
    float dx = (src->lm[GV_THUMB_TIP].x - src->lm[GV_THUMB_CMC].x) * aspect;
    float dy = src->lm[GV_THUMB_TIP].y - src->lm[GV_THUMB_CMC].y;
    float len = hypotf(dx, dy), side = gv_hand_scale(src, aspect);
    out.lm[GV_THUMB_IP].x += (-dy / len) * side / aspect; /* perpendicular, back to normalised x */
    out.lm[GV_THUMB_IP].y += (dx / len) * side;
    return out;
}

/* Thumb out and index up, but the fingertips nearly touching: everything about
 * the pose is right except the gap, so only the gap can be what refuses it. */
static void a_resize_cannot_start_from_a_near_pinch(void)
{
    gv_frame f = with_thumb_tip_towards_index(&fixture_real_pinch_open, 0.2f);
    CHECK(gv_thumb_is_out(&f, REAL, &LIM));
    CHECK(gv_finger_is_extended(&f, GV_FINGER_INDEX, REAL, &LIM));
    CHECK(gv_pinch_gap(&f, REAL) < LIM.pinch_gap_min);
    CHECK(!gv_pose_can_arm_resize(&f, REAL, &LIM));
}

/* Out means long AND straight; in means short OR bent. A long, bent thumb is
 * the case that tells those apart from their opposites. */
static void a_long_but_bent_thumb_is_in_not_out(void)
{
    gv_frame f = with_thumb_bent(&fixture_real_pinch_open, REAL);
    CHECK_NEAR(gv_thumb_reach(&f, REAL), gv_thumb_reach(&fixture_real_pinch_open, REAL), 1e-5);
    CHECK(gv_thumb_reach(&f, REAL) > LIM.thumb_extend_reach);          /* long */
    CHECK(gv_thumb_straightness(&f, REAL) < LIM.thumb_curl_straight);  /* bent */
    CHECK(!gv_thumb_is_out(&f, REAL, &LIM));
    CHECK(gv_thumb_is_in(&f, REAL, &LIM));
}

static void pinch_gap_is_size_and_rotation_free(void)
{
    float g = gv_pinch_gap(&fixture_real_pinch_open, REAL);
    CHECK_NEAR(g, 1.73f, 0.05); /* measured in Python on the same frame */
    gv_frame small = scaled(&fixture_real_pinch_open, 0.5f);
    gv_frame turned = rotated(&fixture_real_pinch_open, REAL, 60.0f);
    CHECK_NEAR(gv_pinch_gap(&small, REAL), g, 0.01);
    CHECK_NEAR(gv_pinch_gap(&turned, REAL), g, 0.01);
    CHECK(gv_pinch_gap(&fixture_real_pinch_closed, REAL) < 0.10f);
}

/* --- which way the palm faces ------------------------------------------- */

static void palm_turn_is_a_size_free_rotation_free_number(void)
{
    const gv_frame *f = &fixture_real_tight_two;
    float turn = gv_palm_turn(f, REAL);
    CHECK(fabsf(turn) > 0.25f); /* a hand seen flat on is clearly one way or the other */

    gv_frame small = scaled(f, 0.5f);
    CHECK_NEAR(gv_palm_turn(&small, REAL), turn, 0.01);
    for (int deg = -90; deg <= 90; deg += 45) {
        gv_frame r = rotated(f, REAL, (float)deg);
        CHECK_NEAR(gv_palm_turn(&r, REAL), turn, 0.01);
    }
}

static void turning_the_hand_over_flips_the_sign(void)
{
    const gv_frame *f = &fixture_real_tight_two;
    gv_frame over = mirrored(f);
    CHECK_NEAR(gv_palm_turn(&over, REAL), -gv_palm_turn(f, REAL), 0.01);
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
    RUN(the_thumb_reads_out_when_out_and_in_when_tucked);
    RUN(the_pinch_ready_hand_can_arm_a_resize);
    RUN(other_hand_shapes_cannot_arm_a_resize);
    RUN(no_hand_shape_arms_two_gestures);
    RUN(a_pinched_hand_can_stay_but_a_fist_or_an_open_hand_cannot);
    RUN(a_resize_cannot_start_from_a_near_pinch);
    RUN(a_long_but_bent_thumb_is_in_not_out);
    RUN(pinch_gap_is_size_and_rotation_free);
    RUN(palm_turn_is_a_size_free_rotation_free_number);
    RUN(turning_the_hand_over_flips_the_sign);
    RUN(hand_scale_matches_measured_value);
    RUN(hand_scale_follows_apparent_size);
    RUN(hand_scale_ignores_rotation_on_a_wide_frame);
    RUN(palm_x_lies_within_the_palm);
    RUN(palm_x_is_in_frame_heights);
    return TEST_SUMMARY();
}
