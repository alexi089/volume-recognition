/*
 * trace.h - parse the landmark CSV written by `app/main.py --record`.
 *
 *   # aspect=1.777778
 *   t_sec,present,x0,y0,z0,...,x20,y20,z20[,hand]
 *   0.033412,1,0.284924,0.761340,0.000001,...
 */
#ifndef GV_TRACE_H
#define GV_TRACE_H

#include "gesture.h"

typedef enum {
    GV_TRACE_FRAME     =  1, /* a data row; *out was filled in */
    GV_TRACE_SKIP      =  0, /* blank line, comment, or the column header */
    GV_TRACE_MALFORMED = -1  /* wrong field count or a field that is not a number */
} gv_trace_result;

/* Parses one line. `line` is not modified. */
gv_trace_result gv_trace_parse_line(const char *line, gv_frame *out);

/* If `line` is the "# aspect=..." comment, stores the value and returns 1. */
int gv_trace_parse_aspect(const char *line, float *aspect);

#endif /* GV_TRACE_H */
