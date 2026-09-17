/*
 * window_math.c - where a window goes when it is scaled. No macOS in here.
 */
#include "window.h"

#include <math.h>

/* `lo` first, then `hi`: when the two disagree (a screen smaller than the
 * minimum window), the upper limit wins. */
static double clamp(double v, double lo, double hi)
{
    return fmin(fmax(v, lo), hi);
}

int win_sizes_differ(win_rect a, win_rect b)
{
    return fabs(a.w - b.w) > WIN_SIZE_TOLERANCE || fabs(a.h - b.h) > WIN_SIZE_TOLERANCE;
}

int win_write_was_ignored(win_rect before, win_rect want, win_rect after, int responded)
{
    int asked_for_a_change = win_sizes_differ(want, before);
    int nothing_changed = !win_sizes_differ(after, before);
    return asked_for_a_change && nothing_changed && !responded;
}

int win_is_candidate(int layer, int is_own, win_rect bounds)
{
    return layer == 0 && !is_own &&
           bounds.w >= WIN_CANDIDATE_MIN && bounds.h >= WIN_CANDIDATE_MIN;
}

win_rect win_below_menu_bar(win_rect display, win_rect bar)
{
    /* Display and window-list rectangles are whole numbers of points, so these
     * comparisons are exact; half a point of slack costs nothing. */
    const double slack = 0.5;
    int along_the_top = fabs(bar.x - display.x) <= slack && fabs(bar.y - display.y) <= slack;
    int full_width    = fabs(bar.w - display.w) <= slack;
    int a_strip       = bar.h > 0.0 && bar.h < display.h / 2.0;
    if (!along_the_top || !full_width || !a_strip)
        return display;

    win_rect below = { display.x, display.y + bar.h, display.w, display.h - bar.h };
    return below;
}

win_rect win_scaled_frame(win_rect original, double scale, win_rect visible, double min_w, double min_h)
{
    if (!isfinite(scale) || scale <= 0.0)
        return original;

    /* Size first. The origin is worked out FROM the clamped size: do it the
     * other way round and a clamped window sits off-centre by half of whatever
     * the clamp took away. */
    win_rect r;
    r.w = clamp(original.w * scale, min_w, visible.w);
    r.h = clamp(original.h * scale, min_h, visible.h);
    r.x = original.x + original.w / 2.0 - r.w / 2.0;
    r.y = original.y + original.h / 2.0 - r.h / 2.0;

    /* Then slide it inside the screen. It fits, because its size was clamped
     * to the screen's, so the range below is never empty. */
    r.x = clamp(r.x, visible.x, visible.x + visible.w - r.w);
    r.y = clamp(r.y, visible.y, visible.y + visible.h - r.h);
    return r;
}
