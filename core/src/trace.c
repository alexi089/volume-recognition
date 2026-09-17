#include "trace.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FIELDS_PER_ROW (2 + 3 * GV_NUM_LANDMARKS) /* t_sec, present, 21 * xyz */

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

    double values[FIELDS_PER_ROW];
    for (int i = 0; i < FIELDS_PER_ROW; i++) {
        char *end;
        values[i] = strtod(p, &end);
        if (end == p)
            return GV_TRACE_MALFORMED; /* no number here */
        /* strtod also accepts "nan", "inf", and overflows "1e999" to infinity.
         * None of those is a coordinate or a time. */
        if (!isfinite(values[i]))
            return GV_TRACE_MALFORMED;
        p = skip_space(end);

        int last = (i == FIELDS_PER_ROW - 1);
        if (last) {
            if (*p != '\0')
                return GV_TRACE_MALFORMED; /* extra fields */
        } else {
            if (*p != ',')
                return GV_TRACE_MALFORMED; /* too few fields, or junk */
            p = skip_space(p + 1);
        }
    }

    if (values[1] != 0.0 && values[1] != 1.0)
        return GV_TRACE_MALFORMED;

    out->t_sec = values[0];
    out->present = (int)values[1];
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
