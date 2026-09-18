/*
 * window.h - resizing the front window.
 *
 * Two halves. The geometry (this far down the file) is pure arithmetic and is
 * tested without a window. The part that talks to macOS comes after it.
 */
#ifndef GV_WINDOW_H
#define GV_WINDOW_H

#include <stddef.h>

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

/*
 * The part of `display` below a menu bar, if `bar` is this display's menu bar:
 * a strip along its top edge. Any other rectangle (another display's bar, or
 * an empty one when the bar is hidden) returns `display` unchanged.
 *
 * The bar's height is measured, not assumed: 37 pt on a notched built-in
 * display and 24 pt on an external one, and with "Displays have separate
 * Spaces" every display has its own.
 */
win_rect win_below_menu_bar(win_rect display, win_rect bar);

/*
 * Is an entry of the on-screen window list something a person would call a
 * window, and so a possible target? Layer 0 is where ordinary windows live, but
 * not everything on layer 0 is one: while the camera is in use the Window
 * Server keeps 10 x 18 pt indicator windows there, one per menu bar, IN FRONT of
 * every real window. The first live try picked one of those, and every resize
 * failed (Accessibility cannot talk to the Window Server: error -25204).
 * So: layer 0, not our own (the camera preview), and at least
 * WIN_CANDIDATE_MIN points on each side.
 */
#define WIN_CANDIDATE_MIN 100.0
int win_is_candidate(int layer, int is_own, win_rect bounds);

/*
 * After a write, the size is read back. Was the write IGNORED?
 *
 *   before     the size before this write
 *   want       the size asked for
 *   after      the size read back
 *   responded  1 if any earlier write since win_acquire changed the window
 *
 * Ignored means: a change was asked for, nothing changed, and this window has
 * never yet responded. A window that HAS responded and now stays put is at a
 * limit of its own (the first live try pinned a browser window at its minimum
 * size and every further pinch raised a false alarm); that is not an error.
 * Sizes within WIN_SIZE_TOLERANCE points count as equal.
 */
#define WIN_SIZE_TOLERANCE 2.0
int win_sizes_differ(win_rect a, win_rect b);
int win_write_was_ignored(win_rect before, win_rect want, win_rect after, int responded);

/* --- the part that talks to macOS (window_macos.c) ----------------------- */

/* The smallest a gesture may make a window. Apps enforce their own minimums on
 * top of this; this one only stops a window shrinking to a sliver. */
#define WIN_MIN_W 320.0
#define WIN_MIN_H 240.0

/*
 * Nearly the same shape as vol_status: zero is success, negatives are errors -
 * and there is now one POSITIVE value, which is neither. Minimising is not
 * finished when the write returns (see win_minimize), so "not yet" is a real
 * answer and must not read as failure: anything < 0 is a problem to report,
 * WIN_PENDING means ask again on the next frame.
 */
typedef enum {
    WIN_OK                 =  0,
    WIN_PENDING            =  1, /* not yet: the window is still on its way (win_minimize_settled) */
    WIN_ERR_NOT_TRUSTED    = -1, /* this process may not use the Accessibility API; see win_strerror */
    WIN_ERR_NO_WINDOW      = -2, /* nothing to resize: no front window, or the one held has closed */
    WIN_ERR_NOT_RESIZABLE  = -3, /* the front window's size cannot be set (fixed-size windows) */
    WIN_ERR_IGNORED        = -4, /* the write reported success and the window did not change */
    WIN_ERR_AX             = -5, /* an Accessibility call failed; see win_last_ax_error() */
    WIN_ERR_BAD_VALUE      = -6  /* a scale that is not a positive finite number, or a NULL buffer */
} win_status;

/* Take hold of the frontmost ordinary window that does not belong to this
 * process (which leaves out the camera preview), and remember its frame and
 * the visible frame of its screen. The window stays the target until
 * win_release, even if the user clicks elsewhere. Acquiring again lets go of
 * the previous window first. */
win_status win_acquire(void);

/* Resize the held window to `scale` times the frame it had at win_acquire,
 * about its centre, kept on its screen (win_scaled_frame). The size is read
 * back afterwards: CoreAudio taught this project that a success code is not
 * evidence. A window that settles on a DIFFERENT size than asked (an app's own
 * minimum, a terminal snapping to whole characters) is WIN_OK; one that did not
 * move at all when it should have is WIN_ERR_IGNORED. */
win_status win_apply(float scale);

/* Let go. Safe to call with nothing held. The window stays as it is. A window
 * minimised by win_minimize is not let go: see win_restore. */
void win_release(void);

/*
 * How long a minimise is given to actually happen before it counts as ignored.
 *
 * Measured on this machine, on Terminal and on Slack: the write returns
 * kAXErrorSuccess in well under a millisecond, the attribute read straight back
 * still says false, the read 50 ms later times out after the full messaging
 * timeout (the app is busy animating its window into the Dock), and it finally
 * reads true about 0.57 s after the write. One second is that, with margin.
 */
#define WIN_SETTLE_DEADLINE_SEC 1.0

/*
 * How long ONE win_minimize_settled read waits for an answer, as opposed to how
 * long the window has in total.
 *
 * These are different questions and want different numbers. Measured: an
 * application that is ready to answer does so in under a millisecond, and one
 * that is busy animating does not answer at all - it blocks the read for the
 * whole timeout and then fails. At the usual AX_TIMEOUT_SEC that cost the
 * camera loop a quarter of a second per asking frame (measured: polls of 0, 251
 * and 210 ms). But a read that times out already MEANS "not yet", so there is
 * nothing to be gained by waiting for it: ask briefly, take silence for an
 * answer, and come back next frame. WIN_SETTLE_DEADLINE_SEC above, not this, is
 * what eventually decides that a window is never going to move.
 */
#define WIN_SETTLE_READ_TIMEOUT_SEC 0.03

/* Minimise the front window - the same one win_acquire would take, by the same
 * rule, except that a window whose size is fixed is minimised happily.
 *
 * RETURNS AT ONCE, and WIN_OK here means only that the WRITE was accepted: the
 * window will not have gone anywhere yet. Verifying it is win_minimize_settled's
 * job, on a later frame, and the split exists because the wait is ~0.57 s and
 * the caller is a 30 fps camera loop - half a second of frozen preview and HUD
 * is too high a price for a status.
 *
 * It does NOT disturb a resize in progress: win_acquire's window and this one
 * are kept apart. */
win_status win_minimize(void);

/* Did the last win_minimize actually take? Reads the attribute back ONCE and
 * returns immediately:
 *
 *   WIN_OK           the window is minimised. Done; stop asking.
 *   WIN_PENDING      not yet, and there is still time (WIN_SETTLE_DEADLINE_SEC
 *                    since the write). Ask again on the next frame. A read that
 *                    times out counts as this: the application being too busy
 *                    to answer IS what the minimise animation looks like.
 *   WIN_ERR_IGNORED  the deadline passed and the window is still where it was.
 *   WIN_ERR_NO_WINDOW  nothing has been minimised (or it was restored).
 *
 * Safe to call as often as a caller likes; it never blocks for longer than one
 * Accessibility call's timeout. */
win_status win_minimize_settled(void);

/* Un-minimise the window win_minimize last minimised, and forget it. The way
 * back for the command-line tool and for a probe; no gesture calls it, which is
 * why this one is allowed to block until the window is back. A minimised window
 * is gone from the screen and from the window list, so the only handle on it is
 * the one win_minimize kept - which means this only ever works inside the same
 * process, and only until the next win_minimize. WIN_ERR_NO_WINDOW if nothing
 * was minimised (or it has already been restored). */
win_status win_restore(void);

/* The held window's frame as last read (after win_acquire or win_apply). */
win_status win_current_frame(win_rect *out);

/* The name of the application that owns the held window, NUL-terminated.
 * The name is filled in by any attempt to take a window, win_acquire or
 * win_minimize, and the status says only whether one is HELD: after a failed
 * win_acquire, and after any win_minimize, it returns WIN_ERR_NO_WINDOW and
 * still names the app that was tried (empty if none was found), which is what
 * an error message - or a tool reporting what it just minimised - needs.
 * win_release clears it. */
win_status win_target_name(char *buf, size_t len);

/* Human-readable text for a win_status. Never returns NULL. */
const char *win_strerror(win_status s);

/* The raw AXError from the most recent failing Accessibility call. One
 * unsynchronised global, like vol_last_os_status: single thread only. */
int win_last_ax_error(void);

#endif /* GV_WINDOW_H */
