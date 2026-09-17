/*
 * window.h - resizing the front window.
 *
 * Two halves. The geometry (this far down the file) is pure arithmetic and is
 * tested without a window. The part that talks to macOS comes after it.
 */
#ifndef GV_WINDOW_H
#define GV_WINDOW_H

/* A rectangle in the Accessibility API's space: points, origin at the TOP-left
 * of the main display, y growing downwards. Displays above or to the left of
 * the main one have negative coordinates. Doubles, because CGFloat is one. */
typedef struct { double x, y, w, h; } win_rect;

/*
 * The frame a window should have at `scale`, given the frame it had when the
 * gesture armed and the visible frame of its screen.
 *
 *  1. Size = original size x scale, clamped to [min_w x min_h, visible size].
 *     If the screen is smaller than the minimum, the screen wins.
 *  2. Placed so the centre stays where it was.
 *  3. Then MOVED, never shrunk further, to lie inside `visible`.
 *
 * A scale that is not a positive finite number returns `original` untouched.
 */
win_rect win_scaled_frame(win_rect original, double scale, win_rect visible, double min_w, double min_h);

#endif /* GV_WINDOW_H */
