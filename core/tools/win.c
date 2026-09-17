/*
 * win - tiny command-line front end to window.h.
 *
 *   win get            print the front window's app, frame and screen
 *   win scale 0.9      resize it to 0.9x about its centre, wait a second, put it back
 *   win bench          60 small resizes and back, to time one win_apply call
 *
 * Exists so the Accessibility code can be exercised, and stepped through in
 * the debugger, without the camera or Python anywhere in the picture. Run it
 * from a terminal and the "front window" is that terminal's.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "window.h"

static int fail(const char *what, win_status s)
{
    fprintf(stderr, "win: %s: %s", what, win_strerror(s));
    if (s == WIN_ERR_AX)
        fprintf(stderr, " [AXError %d]", win_last_ax_error());
    fputc('\n', stderr);
    return 1;
}

static int usage(void)
{
    fputs("usage: win get | win scale <factor> | win bench\n", stderr);
    return 2;
}

static void print_frame(const char *label)
{
    win_rect r;
    if (win_current_frame(&r) == WIN_OK)
        printf("%-10s %.0f x %.0f at %.0f, %.0f\n", label, r.w, r.h, r.x, r.y);
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage();

    win_status s = win_acquire();
    if (s != WIN_OK)
        return fail("acquire", s);
    char name[128];
    win_target_name(name, sizeof name);
    printf("target     %s\n", name);
    print_frame("frame");

    int rc = 0;
    if (argc == 2 && strcmp(argv[1], "get") == 0) {
        /* nothing more */
    } else if (argc == 3 && strcmp(argv[1], "scale") == 0) {
        errno = 0;
        char *end;
        float factor = strtof(argv[2], &end);
        if (errno != 0 || end == argv[2] || *end != '\0') {
            win_release();
            return usage();
        }
        s = win_apply(factor);
        print_frame("scaled");
        if (s != WIN_OK)
            rc = fail("apply", s);
        sleep(1);
        s = win_apply(1.0f); /* always put it back, even if the first write complained */
        print_frame("restored");
        if (s != WIN_OK)
            rc = fail("restore", s);
    } else if (argc == 2 && strcmp(argv[1], "bench") == 0) {
        const int n = 60;
        double total = 0.0, worst = 0.0;
        for (int i = 1; i <= n && rc == 0; i++) {
            float factor = 1.0f - 0.1f * (float)(i <= n / 2 ? i : n - i) / (float)(n / 2); /* 1.0 -> 0.9 -> 1.0 */
            double t0 = now_sec();
            s = win_apply(factor);
            double dt = now_sec() - t0;
            total += dt;
            if (dt > worst)
                worst = dt;
            if (s != WIN_OK && s != WIN_ERR_IGNORED) /* a step under the tolerance may read as ignored */
                rc = fail("apply", s);
        }
        s = win_apply(1.0f);
        print_frame("restored");
        printf("win_apply  mean %.1f ms, worst %.1f ms over %d calls\n", total / n * 1e3, worst * 1e3, n);
    } else {
        win_release();
        return usage();
    }

    win_release();
    return rc;
}
