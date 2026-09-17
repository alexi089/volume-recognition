/*
 * replay - feed a recorded landmark trace through the gesture core and check
 * what it did to the volume.
 *
 *   replay traces/slide_up.csv up        volume must end clearly higher
 *   replay traces/slide_down.csv down    ... clearly lower
 *   replay traces/nose_scratch.csv none  no volume change at all
 *   replay traces/session.csv stats      no verdict, just numbers, for tuning
 *
 * Exit status: 0 expectation met (always, for stats), 1 not met, 2 usage or
 * unreadable trace.
 * The point is regression testing on real hands: record a session once with
 * `make run ARGS="--record traces/x.csv"`, list it in traces/manifest.txt, and
 * every future change to the core is checked against it.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gesture.h"
#include "trace.h"

#define START_VOLUME 0.5f
#define CLEAR_CHANGE 0.05f /* "clearly" higher or lower */

/* Lengths of the armed sessions seen so far. The count is not known up
 * front, so the array grows: realloc either extends the block in place or
 * moves it, and either way returns where it now lives. Doubling the capacity
 * each time keeps the number of reallocs logarithmic. */
typedef struct {
    double *sec;
    int     count, capacity;
} durations;

static int durations_push(durations *d, double v)
{
    if (d->count == d->capacity) {
        int cap = d->capacity ? d->capacity * 2 : 64;
        /* Assign to a temporary first: if realloc fails it returns NULL and
         * leaves the old block alone, and writing NULL straight into d->sec
         * would leak it. */
        double *grown = realloc(d->sec, (size_t)cap * sizeof *grown);
        if (grown == NULL)
            return -1;
        d->sec = grown;
        d->capacity = cap;
    }
    d->sec[d->count++] = v;
    return 0;
}

/* qsort's comparator gets pointers to two elements as void *, and returns
 * negative, zero or positive. (a > b) - (a < b) does that without the overflow
 * that `return a - b` has for ints, or the truncation it has for doubles. */
static int by_value(const void *pa, const void *pb)
{
    double a = *(const double *)pa, b = *(const double *)pb;
    return (a > b) - (a < b);
}

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[2], "up") != 0 && strcmp(argv[2], "down") != 0 &&
                      strcmp(argv[2], "none") != 0 && strcmp(argv[2], "stats") != 0)) {
        fputs("usage: replay <trace.csv> <up|down|none|stats>\n", stderr);
        return 2;
    }
    const char *path = argv[1];
    const char *expect = argv[2];

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        perror(path); /* prints "<path>: No such file or directory" etc. */
        return 2;
    }

    gv_config cfg = gv_default_config();
    gv_ctx *ctx = NULL;
    gv_output out = { GV_IDLE, START_VOLUME, 0, GV_AXIS_NONE, GV_KIND_NONE, 1.0f };
    int frames = 0, armed_frames = 0, changes = 0, line_no = 0, status = 0;

    durations sessions = { NULL, 0, 0 };
    gv_state prev_state = GV_IDLE;
    double armed_since = 0.0, last_t = 0.0;
    float prev_volume = START_VOLUME, biggest_step = 0.0f, travel = 0.0f;

    /* A full row is about 700 characters. fgets never writes past the buffer;
     * if a line is longer it comes back in pieces, which is detected below by
     * the missing newline. */
    char line[4096];
    while (fgets(line, sizeof line, fp) != NULL) {
        line_no++;
        if (strchr(line, '\n') == NULL && !feof(fp)) {
            fprintf(stderr, "%s:%d: line too long\n", path, line_no);
            status = 2;
            break;
        }

        /* The aspect comment has to arrive before the first data row, since
         * the context is created from the config on first use. */
        if (ctx == NULL && gv_trace_parse_aspect(line, &cfg.aspect))
            continue;

        gv_frame frame;
        gv_trace_result r = gv_trace_parse_line(line, &frame);
        if (r == GV_TRACE_SKIP)
            continue;
        if (r == GV_TRACE_MALFORMED) {
            fprintf(stderr, "%s:%d: malformed row\n", path, line_no);
            status = 2;
            break;
        }

        if (ctx == NULL) {
            ctx = gv_create(&cfg);
            if (ctx == NULL) {
                fputs("replay: out of memory\n", stderr);
                status = 2;
                break;
            }
            gv_set_current_volume(ctx, START_VOLUME);
        }

        out = gv_update(ctx, &frame);
        frames++;
        armed_frames += (out.state == GV_ARMED);
        changes += out.changed;

        if (out.state == GV_ARMED && prev_state != GV_ARMED)
            armed_since = frame.t_sec;
        if (out.state != GV_ARMED && prev_state == GV_ARMED &&
            durations_push(&sessions, frame.t_sec - armed_since) != 0) {
            fputs("replay: out of memory\n", stderr);
            status = 2;
            break;
        }
        float step = fabsf(out.volume - prev_volume);
        if (step > biggest_step)
            biggest_step = step;
        travel += step;
        prev_volume = out.volume;
        prev_state = out.state;
        last_t = frame.t_sec;
    }
    if (status == 0 && prev_state == GV_ARMED && /* still armed when the trace ended */
        durations_push(&sessions, last_t - armed_since) != 0)
        status = 2;

    /* One exit path releases everything, whatever happened above. */
    fclose(fp);
    gv_destroy(ctx);
    if (status == 0 && frames == 0) {
        fprintf(stderr, "%s: no frames\n", path);
        status = 2;
    }
    if (status != 0) {
        free(sessions.sec);
        return status;
    }

    if (strcmp(expect, "stats") == 0) {
        int short_ones = 0;
        double armed_total = 0.0;
        for (int i = 0; i < sessions.count; i++) {
            short_ones += sessions.sec[i] < 1.0;
            armed_total += sessions.sec[i];
        }
        qsort(sessions.sec, (size_t)sessions.count, sizeof *sessions.sec, by_value);
        double median = sessions.count ? sessions.sec[sessions.count / 2] : 0.0;

        printf("%s\n", path);
        printf("  frames %d over %.0f s, armed %.0f s in %d sessions\n", frames, last_t, armed_total,
               sessions.count);
        printf("  session length: median %.1f s, shortest %.2f s, %d under 1 s\n", median,
               sessions.count ? sessions.sec[0] : 0.0, short_ones);
        printf("  volume: %d changes, total travel %.2f, biggest single-frame step %.3f\n", changes,
               travel, biggest_step);
        free(sessions.sec);
        return 0;
    }
    free(sessions.sec);

    float delta = out.volume - START_VOLUME;
    int ok;
    if (strcmp(expect, "up") == 0)
        ok = delta >= CLEAR_CHANGE;
    else if (strcmp(expect, "down") == 0)
        ok = delta <= -CLEAR_CHANGE;
    else
        ok = (changes == 0);

    printf("%-4s %-40s expect %-4s frames %4d  armed %4d  changes %3d  volume %.2f -> %.2f\n",
           ok ? "ok" : "FAIL", path, expect, frames, armed_frames, changes, START_VOLUME, out.volume);
    return ok ? 0 : 1;
}
