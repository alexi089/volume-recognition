/*
 * The rule that turns a replayed recording into "ok" or "FAIL".
 */
#include "test.h"

#include "verdict.h"

static gv_replay_summary nothing_happened(void)
{
    gv_replay_summary s = { 0.0f, 0, 0, 1.0f };
    return s;
}

static void the_five_words_parse_and_nothing_else_does(void)
{
    gv_expectation e;
    CHECK(gv_expectation_parse("up", &e) && e == GV_EXPECT_UP);
    CHECK(gv_expectation_parse("down", &e) && e == GV_EXPECT_DOWN);
    CHECK(gv_expectation_parse("grow", &e) && e == GV_EXPECT_GROW);
    CHECK(gv_expectation_parse("shrink", &e) && e == GV_EXPECT_SHRINK);
    CHECK(gv_expectation_parse("none", &e) && e == GV_EXPECT_NONE);
    CHECK(!gv_expectation_parse("stats", &e)); /* replay's own mode, not an expectation */
    CHECK(!gv_expectation_parse("UP", &e));
    CHECK(!gv_expectation_parse("", &e));
    CHECK(!gv_expectation_parse("upward", &e));
}

static void up_and_down_need_a_clear_volume_change(void)
{
    gv_replay_summary s = nothing_happened();
    s.volume_changes = 12;
    s.volume_delta = 0.20f;
    CHECK(gv_verdict(GV_EXPECT_UP, &s));
    CHECK(!gv_verdict(GV_EXPECT_DOWN, &s));
    s.volume_delta = -0.20f;
    CHECK(gv_verdict(GV_EXPECT_DOWN, &s));
    CHECK(!gv_verdict(GV_EXPECT_UP, &s));
    s.volume_delta = 0.04f; /* moved, but not clearly */
    CHECK(!gv_verdict(GV_EXPECT_UP, &s));
    s.volume_delta = -0.04f;
    CHECK(!gv_verdict(GV_EXPECT_DOWN, &s));
    s.volume_delta = 0.05f; /* exactly the threshold counts */
    CHECK(gv_verdict(GV_EXPECT_UP, &s));
}

static void grow_and_shrink_need_a_clear_scale_change(void)
{
    gv_replay_summary s = nothing_happened();
    s.resize_changes = 20;
    s.final_scale = 1.40f;
    CHECK(gv_verdict(GV_EXPECT_GROW, &s));
    CHECK(!gv_verdict(GV_EXPECT_SHRINK, &s));
    s.final_scale = 0.70f;
    CHECK(gv_verdict(GV_EXPECT_SHRINK, &s));
    CHECK(!gv_verdict(GV_EXPECT_GROW, &s));
    s.final_scale = 1.03f;
    CHECK(!gv_verdict(GV_EXPECT_GROW, &s));
    s.final_scale = 0.97f;
    CHECK(!gv_verdict(GV_EXPECT_SHRINK, &s));
}

/* A clip that is supposed to move one thing must not move the other: a slide
 * that also resized a window is a failure, however good the slide was. */
static void the_other_gesture_must_stay_out_of_it(void)
{
    gv_replay_summary slide = nothing_happened();
    slide.volume_changes = 12;
    slide.volume_delta = 0.20f;
    slide.resize_changes = 1;
    CHECK(!gv_verdict(GV_EXPECT_UP, &slide));
    slide.volume_delta = -0.20f;
    CHECK(!gv_verdict(GV_EXPECT_DOWN, &slide));

    gv_replay_summary pinch = nothing_happened();
    pinch.resize_changes = 20;
    pinch.final_scale = 0.70f;
    pinch.volume_changes = 1;
    CHECK(!gv_verdict(GV_EXPECT_SHRINK, &pinch));
    pinch.final_scale = 1.40f;
    CHECK(!gv_verdict(GV_EXPECT_GROW, &pinch));
}

/* "none" is about changes, not totals: a volume that went up and came back, or
 * a window that was rolled back to 1.0, still did something on the way. */
static void none_means_no_change_of_either_kind(void)
{
    gv_replay_summary s = nothing_happened();
    CHECK(gv_verdict(GV_EXPECT_NONE, &s));
    s.volume_changes = 2; /* delta still 0 */
    CHECK(!gv_verdict(GV_EXPECT_NONE, &s));
    s = nothing_happened();
    s.resize_changes = 2; /* final scale still 1 */
    CHECK(!gv_verdict(GV_EXPECT_NONE, &s));
}

int main(void)
{
    fprintf(stderr, "test_verdict\n");
    RUN(the_five_words_parse_and_nothing_else_does);
    RUN(up_and_down_need_a_clear_volume_change);
    RUN(grow_and_shrink_need_a_clear_scale_change);
    RUN(the_other_gesture_must_stay_out_of_it);
    RUN(none_means_no_change_of_either_kind);
    return TEST_SUMMARY();
}
