/*
 * verdict.h - did a replayed recording do what its manifest line says?
 *
 * Kept apart from tools/replay.c so the rule can be tested without files.
 */
#ifndef GV_VERDICT_H
#define GV_VERDICT_H

/* What a replay observed, start to finish. */
typedef struct {
    float volume_delta;   /* final volume minus starting volume */
    int   volume_changes; /* frames on which the volume gesture reported a change */
    int   resize_changes; /* ... and the resize gesture */
    float final_scale;    /* the last scale a resize reported (its rolled-back last word
                           * included); 1.0 if none ever did */
} gv_replay_summary;

typedef enum {
    GV_EXPECT_UP,     /* volume clearly higher, and no window touched */
    GV_EXPECT_DOWN,   /* volume clearly lower, and no window touched */
    GV_EXPECT_GROW,   /* a window clearly bigger, and the volume untouched */
    GV_EXPECT_SHRINK, /* a window clearly smaller, and the volume untouched */
    GV_EXPECT_NONE    /* nothing at all: no gesture changed anything */
} gv_expectation;

#define GV_CLEAR_VOLUME_CHANGE 0.05f /* "clearly" higher or lower */
#define GV_CLEAR_SCALE_CHANGE  0.05f /* "clearly" bigger or smaller: beyond 1.05x / 0.95x */

/* Parses "up" | "down" | "grow" | "shrink" | "none". Returns 1 and sets *out, or 0. */
int gv_expectation_parse(const char *word, gv_expectation *out);

/* 1 if the summary meets the expectation. */
int gv_verdict(gv_expectation expect, const gv_replay_summary *seen);

#endif /* GV_VERDICT_H */
