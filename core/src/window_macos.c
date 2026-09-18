/*
 * window_macos.c - window.h on top of the macOS Accessibility API.
 *
 * Two system services are involved, and they do different jobs:
 *
 *   Core Graphics' window list says WHAT is on screen, front to back, with the
 *   owning process of each window. It is read-only.
 *
 *   The Accessibility API (AX) can CHANGE another application's window, given
 *   that application's process id. The user has to allow it per app, in
 *   System Settings > Privacy & Security > Accessibility.
 *
 * So: find the front window's owner in the list, then ask AX for that
 * application's focused window. (Asking AX's system-wide element "which app is
 * focused?" looks like the direct route and fails on this machine with
 * kAXErrorCannotComplete, -25204.)
 *
 * Two things are done to that window, and both go through the same search
 * (take_front_window): resizing, which holds the window for as long as the
 * gesture lasts, and MINIMISING, which is one write and over. A minimised
 * window drops out of the window list, so the only way back to it is the
 * element win_minimize kept; that is what win_restore is for.
 *
 * Minimising is also the one place where this file does NOT check its work on
 * the spot. Measured here (Terminal and Slack): the write returns success in
 * under a millisecond, the attribute read straight back still says false, the
 * read 50 ms later times out because the app is busy animating, and it reads
 * true about 0.57 s after the write. The caller is a 30 fps camera loop, and
 * half a second of frozen preview is too much to pay for a status - so
 * win_minimize returns as soon as the write is accepted and the checking moves
 * to win_minimize_settled, one quick read per later frame until it knows. The
 * verification is not dropped, only moved off the hot path.
 *
 * Memory: Core Foundation objects are reference counted by hand. The rule is
 * in the function names: anything from a function with Copy or Create in its
 * name is ours and must be CFRelease'd; anything from a Get is borrowed and
 * must not be. Every Copy/Create below has its CFRelease on every path.
 */
#include "window.h"

#include <ApplicationServices/ApplicationServices.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* An app that is busy (or hung) does not answer AX calls, and the default
 * wait is six seconds. This runs inside a 30 fps loop, so give up far sooner:
 * a late resize is worth less than a frozen camera. */
#define AX_TIMEOUT_SEC 0.25f

/* How often win_restore looks to see whether the window is back. It is the one
 * call here that waits, because nothing time-critical ever makes it: the
 * deadline it waits to is WIN_SETTLE_DEADLINE_SEC, shared with the minimise. */
#define RESTORE_POLL_SEC 0.03

static AXUIElementRef g_window;          /* the held window, or NULL; owned (retained) */
static AXUIElementRef g_minimised;       /* what win_minimize last put in the Dock; owned */
static double         g_minimised_at;    /* when that write went in (monotonic seconds) */
static win_rect       g_original;        /* its frame at win_acquire */
static win_rect       g_current;         /* its frame as last read */
static win_rect       g_visible;         /* the visible frame of its screen */
static int            g_responded;       /* has any write since win_acquire changed the window? */
static char           g_owner_name[128];
static AXError        g_last_ax_error = kAXErrorSuccess;

/* --- small helpers ------------------------------------------------------- */

static win_rect from_cg(CGRect r)
{
    win_rect out = { r.origin.x, r.origin.y, r.size.width, r.size.height };
    return out;
}

static win_status ax_failed(AXError e)
{
    g_last_ax_error = e;
    /* The element outlived its window: the user closed it mid-gesture. */
    return e == kAXErrorInvalidUIElement ? WIN_ERR_NO_WINDOW : WIN_ERR_AX;
}

/* AX hands back positions and sizes boxed in an AXValue. */
static win_status read_frame(AXUIElementRef window, win_rect *out)
{
    CFTypeRef pos_value = NULL, size_value = NULL;
    CGPoint pos = CGPointZero;
    CGSize size = CGSizeZero;
    win_status status = WIN_OK;

    AXError e = AXUIElementCopyAttributeValue(window, kAXPositionAttribute, &pos_value);
    if (e == kAXErrorSuccess)
        e = AXUIElementCopyAttributeValue(window, kAXSizeAttribute, &size_value);
    if (e != kAXErrorSuccess)
        status = ax_failed(e);
    else if (!AXValueGetValue((AXValueRef)pos_value, kAXValueCGPointType, &pos) ||
             !AXValueGetValue((AXValueRef)size_value, kAXValueCGSizeType, &size))
        status = ax_failed(kAXErrorIllegalArgument);

    if (pos_value)  CFRelease(pos_value);
    if (size_value) CFRelease(size_value);
    if (status == WIN_OK) {
        out->x = pos.x;  out->y = pos.y;
        out->w = size.width;  out->h = size.height;
    }
    return status;
}

static AXError write_position(AXUIElementRef window, double x, double y)
{
    CGPoint p = { x, y };
    AXValueRef v = AXValueCreate(kAXValueCGPointType, &p);
    AXError e = AXUIElementSetAttributeValue(window, kAXPositionAttribute, v);
    CFRelease(v);
    return e;
}

static AXError write_size(AXUIElementRef window, double w, double h)
{
    CGSize s = { w, h };
    AXValueRef v = AXValueCreate(kAXValueCGSizeType, &s);
    AXError e = AXUIElementSetAttributeValue(window, kAXSizeAttribute, v);
    CFRelease(v);
    return e;
}

/* Read a yes/no attribute (kAXMinimized here) back after writing it. A write
 * that returns success is not evidence that anything happened - CoreAudio
 * taught this project that, and the window writes have shown it too. */
static win_status read_bool(AXUIElementRef window, CFStringRef attribute, int *out)
{
    CFTypeRef value = NULL;
    AXError e = AXUIElementCopyAttributeValue(window, attribute, &value);
    if (e != kAXErrorSuccess || value == NULL) {
        if (value)
            CFRelease(value);
        return ax_failed(e == kAXErrorSuccess ? kAXErrorNoValue : e);
    }
    /* CFBooleanGetValue on something that is not a CFBoolean is undefined, and
     * what comes back is whatever the other application put there. */
    win_status status = WIN_OK;
    if (CFGetTypeID(value) != CFBooleanGetTypeID())
        status = ax_failed(kAXErrorIllegalArgument);
    else
        *out = CFBooleanGetValue((CFBooleanRef)value) ? 1 : 0;
    CFRelease(value);
    return status;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); /* monotonic: unaffected by the clock being set */
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* The display holding `centre`, minus its menu bar. The Dock is NOT taken off:
 * its window covers the whole display whatever the Dock's real size, so the
 * window list cannot say how big it is. A resized window can therefore end up
 * partly behind the Dock, as it can when dragged there by hand. */
static win_rect visible_frame_at(CGPoint centre, CFArrayRef window_list)
{
    CGDirectDisplayID display = CGMainDisplayID();
    uint32_t found = 0;
    CGDirectDisplayID hit;
    if (CGGetDisplaysWithPoint(centre, 1, &hit, &found) == kCGErrorSuccess && found > 0)
        display = hit; /* else: a window centred off every screen; the main display will do */
    win_rect frame = from_cg(CGDisplayBounds(display));

    const int menu_level = CGWindowLevelForKey(kCGMainMenuWindowLevelKey);
    for (CFIndex i = 0; i < CFArrayGetCount(window_list); i++) {
        CFDictionaryRef info = CFArrayGetValueAtIndex(window_list, i);
        CFNumberRef layer_ref = CFDictionaryGetValue(info, kCGWindowLayer);
        CFDictionaryRef bounds_ref = CFDictionaryGetValue(info, kCGWindowBounds);
        int layer = 0;
        CGRect bounds;
        if (!layer_ref || !bounds_ref || !CFNumberGetValue(layer_ref, kCFNumberIntType, &layer) ||
            layer != menu_level || !CGRectMakeWithDictionaryRepresentation(bounds_ref, &bounds))
            continue;
        win_rect below = win_below_menu_bar(frame, from_cg(bounds));
        if (below.h != frame.h)
            return below; /* that was this display's bar */
    }
    return frame; /* no bar: hidden, or a full-screen space */
}

/* The owner of the frontmost window a person would call a window; what counts
 * is win_is_candidate's decision (window_math.c), where it is tested. */
static int front_window_owner(CFArrayRef window_list, pid_t *pid_out)
{
    const pid_t self = getpid();
    for (CFIndex i = 0; i < CFArrayGetCount(window_list); i++) { /* front to back */
        CFDictionaryRef info = CFArrayGetValueAtIndex(window_list, i);
        CFNumberRef layer_ref = CFDictionaryGetValue(info, kCGWindowLayer);
        CFNumberRef pid_ref = CFDictionaryGetValue(info, kCGWindowOwnerPID);
        CFDictionaryRef bounds_ref = CFDictionaryGetValue(info, kCGWindowBounds);
        int layer = -1, pid = 0;
        CGRect bounds;
        if (!layer_ref || !pid_ref || !bounds_ref || !CFNumberGetValue(layer_ref, kCFNumberIntType, &layer) ||
            !CFNumberGetValue(pid_ref, kCFNumberIntType, &pid) ||
            !CGRectMakeWithDictionaryRepresentation(bounds_ref, &bounds))
            continue;
        if (!win_is_candidate(layer, pid == self, from_cg(bounds)))
            continue;

        g_owner_name[0] = '\0';
        CFStringRef name = CFDictionaryGetValue(info, kCGWindowOwnerName);
        if (name && !CFStringGetCString(name, g_owner_name, sizeof g_owner_name, kCFStringEncodingUTF8))
            g_owner_name[0] = '\0';
        *pid_out = (pid_t)pid;
        return 1;
    }
    return 0;
}

/*
 * Take hold of the frontmost ordinary window that is not ours (which leaves
 * out the camera preview): the part win_acquire and win_minimize share.
 *
 *   need_resizable  1 to refuse a window whose size cannot be set, with
 *                   WIN_ERR_NOT_RESIZABLE. Resizing wants to find that out now
 *                   rather than on the first pinch; minimising does not care -
 *                   plenty of fixed-size windows minimise perfectly well.
 *   *out            on WIN_OK, the window's AX element, RETAINED: it is the
 *                   caller's, to release or to keep (win_acquire keeps it in
 *                   g_window, win_minimize in g_minimised).
 *   list_out        NULL, or somewhere to leave the on-screen window list that
 *                   was searched - also retained, also the caller's to
 *                   release. win_acquire needs that same list a second time,
 *                   to find the menu bar of the display the window sits on;
 *                   asking the Window Server twice would both cost a second
 *                   call and let the two answers disagree.
 *
 * Nothing is left behind on failure: *out and *list_out are NULL, and
 * g_owner_name holds the app that was tried, so an error can name it.
 */
static win_status take_front_window(int need_resizable, AXUIElementRef *out, CFArrayRef *list_out)
{
    *out = NULL;
    if (list_out)
        *list_out = NULL;

    if (!AXIsProcessTrusted())
        return WIN_ERR_NOT_TRUSTED;

    CFArrayRef list = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (list == NULL)
        return WIN_ERR_NO_WINDOW;

    win_status status = WIN_OK;
    AXUIElementRef app = NULL;
    CFTypeRef window = NULL;
    pid_t pid = 0;

    if (!front_window_owner(list, &pid)) {
        status = WIN_ERR_NO_WINDOW;
        goto done;
    }

    app = AXUIElementCreateApplication(pid);
    AXUIElementSetMessagingTimeout(app, AX_TIMEOUT_SEC);
    AXError e = AXUIElementCopyAttributeValue(app, kAXFocusedWindowAttribute, &window);
    if (e != kAXErrorSuccess || window == NULL)
        e = AXUIElementCopyAttributeValue(app, kAXMainWindowAttribute, &window);
    if (e != kAXErrorSuccess || window == NULL) {
        /* The app is in front but has no window AX will name (or none at all). */
        g_last_ax_error = e;
        status = (e == kAXErrorNoValue || e == kAXErrorAttributeUnsupported || e == kAXErrorSuccess)
                     ? WIN_ERR_NO_WINDOW : WIN_ERR_AX;
        goto done;
    }
    AXUIElementSetMessagingTimeout((AXUIElementRef)window, AX_TIMEOUT_SEC);

    if (need_resizable) {
        Boolean settable = false;
        e = AXUIElementIsAttributeSettable((AXUIElementRef)window, kAXSizeAttribute, &settable);
        if (e != kAXErrorSuccess) {
            status = ax_failed(e);
            goto done;
        }
        if (!settable) {
            status = WIN_ERR_NOT_RESIZABLE;
            goto done;
        }
    }

    *out = (AXUIElementRef)window; /* ownership moves to the caller */
    window = NULL;

done:
    /* `goto done` is C's idiom for "one exit that cleans up": every early
     * return above would otherwise need its own copy of these releases. */
    if (window) CFRelease(window);
    if (app)    CFRelease(app);
    if (status == WIN_OK && list_out)
        *list_out = list; /* handed on, still retained */
    else
        CFRelease(list);
    return status;
}

/* --- window.h ------------------------------------------------------------ */

win_status win_acquire(void)
{
    win_release();

    AXUIElementRef window = NULL;
    CFArrayRef list = NULL;
    win_status status = take_front_window(1, &window, &list);
    if (status != WIN_OK)
        return status; /* g_owner_name stays set on failure: the error should name the app */

    status = read_frame(window, &g_original);
    if (status == WIN_OK) {
        g_current = g_original;
        g_responded = 0;
        CGPoint centre = { g_original.x + g_original.w / 2.0, g_original.y + g_original.h / 2.0 };
        g_visible = visible_frame_at(centre, list);
        g_window = window; /* keep it: ownership moves to g_window */
        window = NULL;
    }

    if (window) CFRelease(window); /* the frame could not be read: nothing is held */
    CFRelease(list);
    return status;
}

win_status win_apply(float scale)
{
    if (!isfinite(scale) || scale <= 0.0f)
        return WIN_ERR_BAD_VALUE;
    if (g_window == NULL)
        return WIN_ERR_NO_WINDOW;

    const win_rect before = g_current;
    const win_rect want = win_scaled_frame(g_original, (double)scale, g_visible, WIN_MIN_W, WIN_MIN_H);

    /* Two writes, and the window is drawn between them. Growing: move first,
     * so the bigger window opens up from its new corner. Shrinking: shrink
     * first, then move. Either way the in-between frame lies inside the
     * bigger of the two, so the window never pokes off the screen mid-step. */
    const int growing = want.w * want.h >= before.w * before.h;
    AXError e = growing ? write_position(g_window, want.x, want.y) : write_size(g_window, want.w, want.h);
    if (e == kAXErrorSuccess)
        e = growing ? write_size(g_window, want.w, want.h) : write_position(g_window, want.x, want.y);
    if (e != kAXErrorSuccess)
        return ax_failed(e);

    win_status status = read_frame(g_window, &g_current);
    if (status != WIN_OK)
        return status;

    /* Not what was asked is normal: apps have minimum sizes, terminals snap to
     * whole characters, video windows keep their shape. A window that never
     * moves AT ALL is the failure worth reporting; one that moved earlier and
     * now stays put has reached a limit of its own. The rule is
     * win_write_was_ignored (window_math.c), where it is tested. */
    if (win_write_was_ignored(before, want, g_current, g_responded))
        return WIN_ERR_IGNORED;
    if (win_sizes_differ(g_current, before))
        g_responded = 1;
    return WIN_OK;
}

void win_release(void)
{
    /* g_minimised is deliberately NOT touched: it belongs to win_minimize,
     * which never holds a window for the glue to let go of, and the resize
     * gesture ending has nothing to do with a window in the Dock. */
    if (g_window != NULL) {
        CFRelease(g_window);
        g_window = NULL;
    }
    g_owner_name[0] = '\0';
}

win_status win_minimize(void)
{
    AXUIElementRef window = NULL;
    win_status status = take_front_window(0, &window, NULL);
    if (status != WIN_OK)
        return status;

    AXError e = AXUIElementSetAttributeValue(window, kAXMinimizedAttribute, kCFBooleanTrue);
    if (e != kAXErrorSuccess) {
        CFRelease(window);
        return ax_failed(e);
    }

    /* The write went through, so keep the element and note the time. The window
     * is on its way to the Dock, where the window list cannot find it again, so
     * this is the only handle left on it; win_minimize_settled needs the clock
     * to know when "not yet" has become "never". Nothing here waits. */
    if (g_minimised)
        CFRelease(g_minimised);
    g_minimised = window;
    g_minimised_at = now_sec();
    return WIN_OK;
}

win_status win_minimize_settled(void)
{
    if (g_minimised == NULL)
        return WIN_ERR_NO_WINDOW;

    int minimised = 0;
    win_status status = read_bool(g_minimised, kAXMinimizedAttribute, &minimised);
    if (status == WIN_OK && minimised)
        return WIN_OK; /* it is in the Dock: done */

    /* An application too busy to answer is what the animation looks like from
     * out here, so that timeout means "not yet" like a plain false does. Any
     * other Accessibility error is a real one and is reported as itself. */
    if (status != WIN_OK && g_last_ax_error != kAXErrorCannotComplete)
        return status; /* read_bool left the AXError in g_last_ax_error */

    return now_sec() - g_minimised_at < WIN_SETTLE_DEADLINE_SEC ? WIN_PENDING : WIN_ERR_IGNORED;
}

win_status win_restore(void)
{
    if (g_minimised == NULL)
        return WIN_ERR_NO_WINDOW;

    win_status status = WIN_OK;
    AXError e = AXUIElementSetAttributeValue(g_minimised, kAXMinimizedAttribute, kCFBooleanFalse);
    if (e != kAXErrorSuccess) {
        status = ax_failed(e);
    } else {
        /* Coming back out of the Dock is the same animation in reverse and
         * reads back just as slowly. This one WAITS for it, unlike the
         * minimise: no camera loop ever calls win_restore, and a way back that
         * cannot say whether it worked would not be worth having. */
        const double deadline = now_sec() + WIN_SETTLE_DEADLINE_SEC;
        for (;;) {
            int minimised = 1;
            status = read_bool(g_minimised, kAXMinimizedAttribute, &minimised);
            if (status == WIN_OK && !minimised)
                break;
            if (status != WIN_OK && g_last_ax_error != kAXErrorCannotComplete)
                break;
            if (now_sec() >= deadline) {
                status = WIN_ERR_IGNORED;
                break;
            }
            usleep((useconds_t)(RESTORE_POLL_SEC * 1e6));
        }
    }

    /* Let go whatever happened: one attempt at the way back is all this
     * offers, and a window that refused to come out of the Dock is not worth
     * holding a reference to. */
    CFRelease(g_minimised);
    g_minimised = NULL;
    return status;
}

win_status win_current_frame(win_rect *out)
{
    if (out == NULL)
        return WIN_ERR_BAD_VALUE;
    if (g_window == NULL)
        return WIN_ERR_NO_WINDOW;
    *out = g_current;
    return WIN_OK;
}

win_status win_target_name(char *buf, size_t len)
{
    if (buf == NULL || len == 0)
        return WIN_ERR_BAD_VALUE;
    /* The name is copied out even when no window is held: after a failed
     * win_acquire it is the app that refused, which is what an error message
     * needs. The status still says whether a window is held. */
    strncpy(buf, g_owner_name, len - 1);
    buf[len - 1] = '\0'; /* strncpy does not terminate when the source fills the buffer */
    return g_window != NULL ? WIN_OK : WIN_ERR_NO_WINDOW;
}

const char *win_strerror(win_status s)
{
    switch (s) {
    case WIN_OK:                return "ok";
    case WIN_PENDING:           return "not yet: the window is still on its way";
    case WIN_ERR_NOT_TRUSTED:   return "not allowed to control windows: add the app that launched this "
                                       "(Terminal, VS Code, ...) under System Settings > Privacy & Security > Accessibility";
    case WIN_ERR_NO_WINDOW:     return "no window to work on";
    case WIN_ERR_NOT_RESIZABLE: return "the front window has a fixed size";
    case WIN_ERR_IGNORED:       return "the window accepted the change and did not change (at its limit, or it ignores being resized or minimised)";
    case WIN_ERR_AX:            return "an Accessibility call failed";
    case WIN_ERR_BAD_VALUE:     return "bad value";
    }
    return "unknown window status";
}

int win_last_ax_error(void)
{
    return (int)g_last_ax_error;
}
