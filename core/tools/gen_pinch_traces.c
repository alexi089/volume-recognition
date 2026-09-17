/*
 * gen_pinch_traces - write the synthetic pinch recordings that `make replay`
 * checks, the resize counterpart of gen_traces.py.
 *
 *   gen_pinch_traces ../traces
 *
 * Real hand shapes (the fixtures lifted from the measurement clips), scripted
 * motion, no jitter. Written in C, unlike the volume generator, so that it
 * shares the pinch blend with the tests (tests/pinch_motion.h) instead of
 * repeating that arithmetic in Python.
 *
 * What they cover is the path from a trace file to a verdict. They are NOT
 * evidence that a real pinch works: that needs real recordings.
 */
#include <stdio.h>

#include "fixtures.h"
#include "pinch_motion.h"

#define DT (1.0 / 30.0)

static FILE  *g_out;
static double g_t;

static int begin(const char *dir, const char *name)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name); /* snprintf never overruns, it truncates */
    g_out = fopen(path, "w");
    if (g_out == NULL) {
        perror(path);
        return -1;
    }
    g_t = 0.0;
    fprintf(g_out, "# aspect=%.6f\n", (double)FIXTURE_REAL_ASPECT);
    fputs("t_sec,present", g_out);
    for (int i = 0; i < GV_NUM_LANDMARKS; i++)
        fprintf(g_out, ",x%d,y%d,z%d", i, i, i);
    fputs(",hand\n", g_out);
    printf("wrote %s\n", path);
    return 0;
}

/* One row. hand == NULL is a frame with no hand in it. */
static void row(const gv_frame *hand)
{
    fprintf(g_out, "%.6f,%d", g_t, hand != NULL);
    for (int i = 0; i < GV_NUM_LANDMARKS; i++) {
        if (hand != NULL)
            fprintf(g_out, ",%.6f,%.6f,%.6f", (double)hand->lm[i].x, (double)hand->lm[i].y, (double)hand->lm[i].z);
        else
            fputs(",0,0,0", g_out);
    }
    fputs(",0\n", g_out); /* handedness: unknown */
    g_t += DT;
}

static int frames_in(double seconds) { return (int)(seconds / DT + 0.5); }

static void hold_pinch(float k, double seconds)
{
    gv_frame f = pinch(k);
    for (int i = 0; i < frames_in(seconds); i++)
        row(&f);
}

static void ramp_pinch(float from, float to, double seconds)
{
    int n = frames_in(seconds);
    for (int i = 1; i <= n; i++) {
        gv_frame f = pinch(from + (to - from) * (float)i / (float)n);
        row(&f);
    }
}

static void no_hand(double seconds)
{
    for (int i = 0; i < frames_in(seconds); i++)
        row(NULL);
}

/* From the pinch at blend k into the recorded fist, in `seconds`, then held. */
static void close_into_fist(float k, double seconds, double held)
{
    gv_frame from = pinch(k), fist = aligned_to(&fixture_real_fist, &fixture_real_pinch_open);
    int n = frames_in(seconds);
    for (int i = 1; i <= n + frames_in(held); i++) {
        float a = i <= n ? (float)i / (float)n : 1.0f;
        gv_frame f = from;
        for (int j = 0; j < GV_NUM_LANDMARKS; j++) {
            f.lm[j].x = from.lm[j].x + (fist.lm[j].x - from.lm[j].x) * a;
            f.lm[j].y = from.lm[j].y + (fist.lm[j].y - from.lm[j].y) * a;
        }
        row(&f);
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fputs("usage: gen_pinch_traces <traces directory>\n", stderr);
        return 2;
    }
    const char *dir = argv[1];

    /* Pinch in, hold, take the hand away: the size stands. */
    if (begin(dir, "synthetic_pinch_shrink.csv") != 0) return 1;
    hold_pinch(0.0f, 0.6); ramp_pinch(0.0f, 0.3f, 1.0); hold_pinch(0.3f, 0.6); no_hand(0.6);
    fclose(g_out);

    /* Arm with the fingers part-way together, spread wide, take the hand away. */
    if (begin(dir, "synthetic_pinch_grow.csv") != 0) return 1;
    hold_pinch(0.25f, 0.6); ramp_pinch(0.25f, -0.2f, 1.0); hold_pinch(-0.2f, 0.6); no_hand(0.6);
    fclose(g_out);

    /* Pinch in, hold, then END BY CLOSING THE HAND, which drags the gap down
     * on the way to a fist. The rollback must leave the held size standing. */
    if (begin(dir, "synthetic_pinch_then_fist.csv") != 0) return 1;
    hold_pinch(0.0f, 0.6); ramp_pinch(0.0f, 0.2f, 0.6); hold_pinch(0.2f, 0.8); close_into_fist(0.2f, 0.2, 0.6);
    fclose(g_out);

    /* A quarter-second glimpse of the pinch-ready hand, shorter than the arm
     * hold, then a fist: nothing may happen. */
    if (begin(dir, "synthetic_pinch_glimpse.csv") != 0) return 1;
    no_hand(0.3); hold_pinch(0.0f, 0.2); close_into_fist(0.0f, 0.2, 0.6); no_hand(0.3);
    fclose(g_out);
    return 0;
}
