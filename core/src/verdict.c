#include "verdict.h"

#include <string.h>

int gv_expectation_parse(const char *word, gv_expectation *out)
{
    /* A table rather than an if-chain: the words and their values sit side by
     * side, and adding one is one line. */
    static const struct { const char *word; gv_expectation value; } words[] = {
        { "up", GV_EXPECT_UP },     { "down", GV_EXPECT_DOWN },
        { "grow", GV_EXPECT_GROW }, { "shrink", GV_EXPECT_SHRINK },
        { "none", GV_EXPECT_NONE },
    };
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
        if (strcmp(word, words[i].word) == 0) {
            *out = words[i].value;
            return 1;
        }
    }
    return 0;
}

int gv_verdict(gv_expectation expect, const gv_replay_summary *seen)
{
    switch (expect) {
    case GV_EXPECT_UP:
        return seen->volume_delta >= GV_CLEAR_VOLUME_CHANGE && seen->resize_changes == 0;
    case GV_EXPECT_DOWN:
        return seen->volume_delta <= -GV_CLEAR_VOLUME_CHANGE && seen->resize_changes == 0;
    case GV_EXPECT_GROW:
        return seen->final_scale >= 1.0f + GV_CLEAR_SCALE_CHANGE && seen->volume_changes == 0;
    case GV_EXPECT_SHRINK:
        return seen->final_scale <= 1.0f - GV_CLEAR_SCALE_CHANGE && seen->volume_changes == 0;
    case GV_EXPECT_NONE:
        return seen->volume_changes == 0 && seen->resize_changes == 0;
    }
    return 0; /* not reached for a valid enum; a corrupt one fails closed */
}
