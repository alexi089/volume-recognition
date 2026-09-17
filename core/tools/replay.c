/*
 * replay - feed a recorded landmark trace through the gesture core and check
 * what it did to the volume.
 *
 *   replay traces/slide_up.csv up        volume must end clearly higher
 *   replay traces/slide_down.csv down    ... clearly lower
 *   replay traces/nose_scratch.csv none  no volume change at all
 *
 * Exit status: 0 expectation met, 1 not met, 2 usage or unreadable trace.
 * The point is regression testing on real hands: record a session once with
 * `make run ARGS="--record traces/x.csv"`, list it in traces/manifest.txt, and
 * every future change to the core is checked against it.
 */
#include <stdio.h>
#include <string.h>

#include "gesture.h"
#include "trace.h"

#define START_VOLUME 0.5f
#define CLEAR_CHANGE 0.05f /* "clearly" higher or lower */

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[2], "up") != 0 && strcmp(argv[2], "down") != 0 &&
                      strcmp(argv[2], "none") != 0)) {
        fputs("usage: replay <trace.csv> <up|down|none>\n", stderr);
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
    gv_output out = { GV_IDLE, START_VOLUME, 0 };
    int frames = 0, armed_frames = 0, changes = 0, line_no = 0, status = 0;

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
    }

    /* One exit path releases everything, whatever happened above. */
    fclose(fp);
    gv_destroy(ctx);
    if (status != 0)
        return status;
    if (frames == 0) {
        fprintf(stderr, "%s: no frames\n", path);
        return 2;
    }

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
