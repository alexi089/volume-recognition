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
