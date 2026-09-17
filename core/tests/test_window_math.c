/*
 * Where a window goes when it is scaled: pure arithmetic, no window needed.
 * Coordinates are the Accessibility API's: top-left origin, y downwards.
 */
#include "test.h"

#include "window.h"

#define MIN_W 320.0
#define MIN_H 240.0

static const win_rect SCREEN = { 0.0, 25.0, 2560.0, 1415.0 }; /* a 2560x1440 display under a 25 pt menu bar */

static double centre_x(win_rect r) { return r.x + r.w / 2.0; }
static double centre_y(win_rect r) { return r.y + r.h / 2.0; }

static win_rect centred_at(double cx, double cy, double w, double h)
{
    win_rect r = { cx - w / 2.0, cy - h / 2.0, w, h };
    return r;
}

static void check_rect(win_rect got, win_rect want)
{
    CHECK_NEAR(got.x, want.x, 1e-9);
    CHECK_NEAR(got.y, want.y, 1e-9);
    CHECK_NEAR(got.w, want.w, 1e-9);
    CHECK_NEAR(got.h, want.h, 1e-9);
}

static int inside(win_rect r, win_rect box)
{
    const double eps = 1e-9;
    return r.x >= box.x - eps && r.y >= box.y - eps &&
           r.x + r.w <= box.x + box.w + eps && r.y + r.h <= box.y + box.h + eps;
}

static void scale_one_changes_nothing(void)
{
    win_rect w = centred_at(1000.0, 700.0, 800.0, 600.0);
    check_rect(win_scaled_frame(w, 1.0, SCREEN, MIN_W, MIN_H), w);
}

static void growing_keeps_the_centre(void)
{
    win_rect w = centred_at(1000.0, 700.0, 800.0, 600.0);
    win_rect r = win_scaled_frame(w, 1.5, SCREEN, MIN_W, MIN_H);
    check_rect(r, centred_at(1000.0, 700.0, 1200.0, 900.0));
}

static void shrinking_keeps_the_centre(void)
{
    win_rect w = centred_at(1000.0, 700.0, 800.0, 600.0);
    win_rect r = win_scaled_frame(w, 0.5, SCREEN, MIN_W, MIN_H);
    check_rect(r, centred_at(1000.0, 700.0, 400.0, 300.0));
}

static void a_window_never_gets_smaller_than_the_minimum(void)
{
    win_rect w = centred_at(1000.0, 700.0, 800.0, 600.0);
    win_rect r = win_scaled_frame(w, 0.1, SCREEN, MIN_W, MIN_H);
    check_rect(r, centred_at(1000.0, 700.0, MIN_W, MIN_H));
}

/* The two sides clamp separately: a wide, short window hits the height floor
 * long before the width floor. */
static void each_side_has_its_own_floor(void)
{
    win_rect w = centred_at(1000.0, 700.0, 1600.0, 300.0);
    win_rect r = win_scaled_frame(w, 0.5, SCREEN, MIN_W, MIN_H);
    check_rect(r, centred_at(1000.0, 700.0, 800.0, MIN_H));
}

static void a_window_never_gets_bigger_than_the_screen(void)
{
    win_rect w = centred_at(1000.0, 700.0, 800.0, 600.0);
    win_rect r = win_scaled_frame(w, 4.0, SCREEN, MIN_W, MIN_H);
    check_rect(r, SCREEN); /* 3200x2400 asked for: both sides clamp, so it IS the visible frame */
}

/* Growing near an edge: the size is what was asked for, and the window moves
 * over to fit. Only the axis that overflows moves. */
static void growing_near_the_right_edge_shifts_left(void)
{
    win_rect w = { 1900.0, 400.0, 600.0, 400.0 }; /* right edge at 2500, 60 pt from the screen's */
    win_rect r = win_scaled_frame(w, 1.5, SCREEN, MIN_W, MIN_H);
    CHECK_NEAR(r.w, 900.0, 1e-9);
    CHECK_NEAR(r.h, 600.0, 1e-9);
    CHECK_NEAR(r.x + r.w, SCREEN.x + SCREEN.w, 1e-9); /* flush with the right edge */
    CHECK_NEAR(centre_y(r), centre_y(w), 1e-9);       /* vertically it still fits: centre kept */
    CHECK(inside(r, SCREEN));
}

static void growing_near_the_bottom_shifts_up(void)
{
    win_rect w = { 800.0, 1000.0, 600.0, 400.0 }; /* bottom edge at 1400, 40 pt from the screen's */
    win_rect r = win_scaled_frame(w, 1.5, SCREEN, MIN_W, MIN_H);
    CHECK_NEAR(r.h, 600.0, 1e-9);
    CHECK_NEAR(r.y + r.h, SCREEN.y + SCREEN.h, 1e-9);
    CHECK_NEAR(centre_x(r), centre_x(w), 1e-9); /* sideways it still fits: centre kept */
    CHECK(inside(r, SCREEN));
}

static void growing_near_the_top_left_shifts_down_and_right(void)
{
    win_rect w = { 10.0, 30.0, 600.0, 400.0 };
    win_rect r = win_scaled_frame(w, 1.5, SCREEN, MIN_W, MIN_H);
    CHECK_NEAR(r.w, 900.0, 1e-9);
    CHECK_NEAR(r.h, 600.0, 1e-9);
    CHECK_NEAR(r.x, SCREEN.x, 1e-9);
    CHECK_NEAR(r.y, SCREEN.y, 1e-9); /* below the menu bar, not under it */
}

/* A display above and to the left of the main one has negative coordinates
 * (the owner's second display sits at y = -611). Nothing may assume 0,0. */
static void a_display_at_negative_coordinates_works_the_same(void)
{
    win_rect above = { -400.0, -611.0, 1728.0, 611.0 };
    win_rect w = centred_at(300.0, -300.0, 600.0, 300.0);
    check_rect(win_scaled_frame(w, 1.2, above, MIN_W, MIN_H), centred_at(300.0, -300.0, 720.0, 360.0));

    win_rect big = win_scaled_frame(w, 4.0, above, MIN_W, MIN_H);
    check_rect(big, above);

    win_rect low = { 800.0, -250.0, 600.0, 240.0 }; /* bottom edge at -10 */
    win_rect r = win_scaled_frame(low, 1.5, above, MIN_W, MIN_H);
    CHECK_NEAR(r.h, 360.0, 1e-9);
    CHECK_NEAR(r.y + r.h, 0.0, 1e-9); /* the display's bottom edge is y = 0 */
    CHECK(inside(r, above));
}

/* A screen smaller than the minimum size: the screen wins, or the window could
 * never be fully visible. */
static void the_screen_beats_the_minimum(void)
{
    win_rect tiny = { 0.0, 0.0, 300.0, 200.0 };
    win_rect w = { 10.0, 10.0, 280.0, 180.0 };
    check_rect(win_scaled_frame(w, 0.5, tiny, MIN_W, MIN_H), tiny);
}

static void a_nonsense_scale_changes_nothing(void)
{
    win_rect w = { 1900.0, 400.0, 900.0, 400.0 }; /* even one hanging off the screen is left alone */
    check_rect(win_scaled_frame(w, NAN, SCREEN, MIN_W, MIN_H), w);
    check_rect(win_scaled_frame(w, INFINITY, SCREEN, MIN_W, MIN_H), w);
    check_rect(win_scaled_frame(w, 0.0, SCREEN, MIN_W, MIN_H), w);
    check_rect(win_scaled_frame(w, -2.0, SCREEN, MIN_W, MIN_H), w);
}

/* Whatever is asked, the result is on the screen and within the size limits. */
static void every_result_is_on_screen(void)
{
    static const double scales[] = { 0.05, 0.25, 0.5, 0.99, 1.0, 1.01, 1.7, 4.0, 40.0 };
    static const win_rect windows[] = {
        { 0.0, 25.0, 400.0, 300.0 }, { 2200.0, 1100.0, 360.0, 340.0 }, { -200.0, -100.0, 900.0, 700.0 },
        { 2400.0, 1300.0, 800.0, 600.0 }, { 500.0, 300.0, 2560.0, 1415.0 },
    };
    for (size_t i = 0; i < sizeof windows / sizeof windows[0]; i++) {
        for (size_t j = 0; j < sizeof scales / sizeof scales[0]; j++) {
            win_rect r = win_scaled_frame(windows[i], scales[j], SCREEN, MIN_W, MIN_H);
            CHECK(inside(r, SCREEN));
            CHECK(r.w >= MIN_W - 1e-9 && r.h >= MIN_H - 1e-9);
        }
    }
}

int main(void)
{
    fprintf(stderr, "test_window_math\n");
    RUN(scale_one_changes_nothing);
    RUN(growing_keeps_the_centre);
    RUN(shrinking_keeps_the_centre);
    RUN(a_window_never_gets_smaller_than_the_minimum);
    RUN(each_side_has_its_own_floor);
    RUN(a_window_never_gets_bigger_than_the_screen);
    RUN(growing_near_the_right_edge_shifts_left);
    RUN(growing_near_the_bottom_shifts_up);
    RUN(growing_near_the_top_left_shifts_down_and_right);
    RUN(a_display_at_negative_coordinates_works_the_same);
    RUN(the_screen_beats_the_minimum);
    RUN(a_nonsense_scale_changes_nothing);
    RUN(every_result_is_on_screen);
    return TEST_SUMMARY();
}
