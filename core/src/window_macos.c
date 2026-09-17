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
 * Memory: Core Foundation objects are reference counted by hand. The rule is
 * in the function names: anything from a function with Copy or Create in its
 * name is ours and must be CFRelease'd; anything from a Get is borrowed and
 * must not be. Every Copy/Create below has its CFRelease on every path.
 */
#include "window.h"

#include <ApplicationServices/ApplicationServices.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

/* An app that is busy (or hung) does not answer AX calls, and the default
 * wait is six seconds. This runs inside a 30 fps loop, so give up far sooner:
 * a late resize is worth less than a frozen camera. */
#define AX_TIMEOUT_SEC 0.25f

static AXUIElementRef g_window;          /* the held window, or NULL; owned (retained) */
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

/* --- window.h ------------------------------------------------------------ */

win_status win_acquire(void)
{
    win_release();

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

    /* Found out now, not on the first pinch. */
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

    status = read_frame((AXUIElementRef)window, &g_original);
    if (status != WIN_OK)
        goto done;

    g_current = g_original;
    g_responded = 0;
    CGPoint centre = { g_original.x + g_original.w / 2.0, g_original.y + g_original.h / 2.0 };
    g_visible = visible_frame_at(centre, list);
    g_window = (AXUIElementRef)window; /* keep it: ownership moves to g_window */
    window = NULL;

done:
    /* `goto done` is C's idiom for "one exit that cleans up": every early
     * return above would otherwise need its own copy of these releases. */
    if (window) CFRelease(window);
    if (app)    CFRelease(app);
    CFRelease(list);
    return status; /* g_owner_name stays set on failure: the error should name the app */
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
    if (g_window != NULL) {
        CFRelease(g_window);
        g_window = NULL;
    }
    g_owner_name[0] = '\0';
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
    case WIN_ERR_NOT_TRUSTED:   return "not allowed to control windows: add the app that launched this "
                                       "(Terminal, VS Code, ...) under System Settings > Privacy & Security > Accessibility";
    case WIN_ERR_NO_WINDOW:     return "no window to resize";
    case WIN_ERR_NOT_RESIZABLE: return "the front window has a fixed size";
    case WIN_ERR_IGNORED:       return "the window accepted a new size and did not change (at its limit, or it ignores resizing)";
    case WIN_ERR_AX:            return "an Accessibility call failed";
    case WIN_ERR_BAD_VALUE:     return "bad value";
    }
    return "unknown window status";
}

int win_last_ax_error(void)
{
    return (int)g_last_ax_error;
}
