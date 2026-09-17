#include "trace.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FIELDS_PER_ROW (2 + 3 * GV_NUM_LANDMARKS) /* t_sec, present, 21 * xyz */
#define MAX_FIELDS     (FIELDS_PER_ROW + 1)       /* plus an optional handedness column */

static const char *skip_space(const char *p)
{
    /* isspace takes an int that must be an unsigned char value or EOF; a
     * plain (possibly negative) char is undefined behaviour, hence the cast. */
    while (*p != '\0' && isspace((unsigned char)*p))
        p++;
    return p;
}

/*
 * The line is walked with strtod rather than split with strtok. strtok writes
 * NUL bytes into the string it scans (so it cannot take a const line) and
 * keeps hidden global state between calls. strtod just reports where the
 * number ended, through `end`, and that is all a CSV reader needs.
 */
gv_trace_result gv_trace_parse_line(const char *line, gv_frame *out)
{
    const char *p = skip_space(line);
    if (*p == '\0' || *p == '#' || strncmp(p, "t_sec", 5) == 0)
        return GV_TRACE_SKIP;

    /* The handedness column was added after the first recordings were made, so
     * a row has either 65 fields or 66. `count` ends up as which. */
    double values[MAX_FIELDS];
    int count = 0;
    for (int i = 0; i < MAX_FIELDS; i++) {
        char *end;
        values[i] = strtod(p, &end);
        if (end == p)
            return GV_TRACE_MALFORMED; /* no number here */
        /* strtod also accepts "nan", "inf", and overflows "1e999" to infinity.
         * None of those is a coordinate or a time. */
        if (!isfinite(values[i]))
            return GV_TRACE_MALFORMED;
        p = skip_space(end);
        count = i + 1;

        if (*p == '\0')
            break; /* end of the row */
        if (*p != ',' || count == MAX_FIELDS)
            return GV_TRACE_MALFORMED; /* junk after a number, or too many fields */
        p = skip_space(p + 1);
    }
    if (count < FIELDS_PER_ROW)
        return GV_TRACE_MALFORMED; /* too few fields */

    if (values[1] != 0.0 && values[1] != 1.0)
        return GV_TRACE_MALFORMED;

    double hand = (count == MAX_FIELDS) ? values[MAX_FIELDS - 1] : 0.0;
    if (hand != -1.0 && hand != 0.0 && hand != 1.0)
        return GV_TRACE_MALFORMED;

    out->t_sec = values[0];
    out->present = (int)values[1];
    out->handedness = (int)hand;
    for (int i = 0; i < GV_NUM_LANDMARKS; i++) {
        out->lm[i].x = (float)values[2 + 3 * i];
        out->lm[i].y = (float)values[3 + 3 * i];
        out->lm[i].z = (float)values[4 + 3 * i];
    }
    return GV_TRACE_FRAME;
}

int gv_trace_parse_aspect(const char *line, float *aspect)
{
    static const char prefix[] = "# aspect=";
    const size_t n = sizeof prefix - 1; /* sizeof counts the trailing NUL */

    if (strncmp(line, prefix, n) != 0)
        return 0;

    char *end;
    double v = strtod(line + n, &end);
    if (end == line + n || !isfinite(v) || v <= 0.0)
        return 0;

    *aspect = (float)v;
    return 1;
}
