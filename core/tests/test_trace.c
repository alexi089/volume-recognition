#include "test.h"

#include <string.h>

#include "trace.h"

/* Builds "t,present,v,v+1,v+2,..." with `fields` landmark values. */
static const char *row(const char *t, const char *present, int fields)
{
    static char buf[2048];
    int n = snprintf(buf, sizeof buf, "%s,%s", t, present);
    for (int i = 0; i < fields; i++)
        n += snprintf(buf + n, sizeof buf - (size_t)n, ",%d.5", i);
    return buf;
}

static void data_row_fills_the_frame(void)
{
    gv_frame f;
    memset(&f, 0xAA, sizeof f); /* garbage, so untouched fields would show */
    CHECK(gv_trace_parse_line(row("1.250000", "1", 63), &f) == GV_TRACE_FRAME);
    CHECK_NEAR(f.t_sec, 1.25, 1e-9);
    CHECK(f.present == 1);
    CHECK_NEAR(f.lm[0].x, 0.5f, 1e-6);
    CHECK_NEAR(f.lm[0].y, 1.5f, 1e-6);
    CHECK_NEAR(f.lm[0].z, 2.5f, 1e-6);
    CHECK_NEAR(f.lm[20].z, 62.5f, 1e-6);
}

static void absent_hand_row_is_a_frame_with_present_zero(void)
{
    gv_frame f;
    CHECK(gv_trace_parse_line(row("2.0", "0", 63), &f) == GV_TRACE_FRAME);
    CHECK(f.present == 0);
}

static void trailing_newline_is_accepted(void)
{
    char line[2048];
    snprintf(line, sizeof line, "%s\r\n", row("0.5", "1", 63));
    gv_frame f;
    CHECK(gv_trace_parse_line(line, &f) == GV_TRACE_FRAME);
    CHECK_NEAR(f.lm[20].z, 62.5f, 1e-6);
}

static void comments_header_and_blank_lines_are_skipped(void)
{
    gv_frame f;
    CHECK(gv_trace_parse_line("# aspect=1.777778\n", &f) == GV_TRACE_SKIP);
    CHECK(gv_trace_parse_line("t_sec,present,x0,y0,z0\n", &f) == GV_TRACE_SKIP);
    CHECK(gv_trace_parse_line("\n", &f) == GV_TRACE_SKIP);
    CHECK(gv_trace_parse_line("", &f) == GV_TRACE_SKIP);
}

static void wrong_field_count_is_malformed(void)
{
    gv_frame f;
    CHECK(gv_trace_parse_line(row("1.0", "1", 62), &f) == GV_TRACE_MALFORMED);
    CHECK(gv_trace_parse_line(row("1.0", "1", 64), &f) == GV_TRACE_MALFORMED); /* 66th field is 63.5: not a handedness */
    CHECK(gv_trace_parse_line(row("1.0", "1", 65), &f) == GV_TRACE_MALFORMED);
    CHECK(gv_trace_parse_line("1.0\n", &f) == GV_TRACE_MALFORMED);
}

static void non_numeric_field_is_malformed(void)
{
    char line[2048];
    snprintf(line, sizeof line, "%s", row("1.0", "1", 63));
    char *p = strstr(line, ",30.5,");
    CHECK(p != NULL);
    memcpy(p, ",oops,", 6);
    gv_frame f;
    CHECK(gv_trace_parse_line(line, &f) == GV_TRACE_MALFORMED);
    CHECK(gv_trace_parse_line(row("1.0", "yes", 63), &f) == GV_TRACE_MALFORMED);
}

/* strtod happily parses "nan", "inf" and "1e999"; a trace must not be able to
 * smuggle those into the core. */
static void non_finite_values_are_malformed(void)
{
    gv_frame f;
    CHECK(gv_trace_parse_line(row("nan", "1", 63), &f) == GV_TRACE_MALFORMED);
    CHECK(gv_trace_parse_line(row("inf", "1", 63), &f) == GV_TRACE_MALFORMED);
    CHECK(gv_trace_parse_line(row("1e999", "1", 63), &f) == GV_TRACE_MALFORMED);

    char line[2048];
    snprintf(line, sizeof line, "%s", row("1.0", "1", 63));
    char *p = strstr(line, ",30.5,");
    CHECK(p != NULL);
    memcpy(p, ",nan ,", 6);
    CHECK(gv_trace_parse_line(line, &f) == GV_TRACE_MALFORMED);
}

/* Newer recordings carry a 66th column: which hand MediaPipe thinks it is
 * (+1 right, -1 left, 0 unsure). Older ones have 65 and mean "unknown". */
static void handedness_column_is_optional(void)
{
    gv_frame f;
    memset(&f, 0xAA, sizeof f);
    CHECK(gv_trace_parse_line(row("1.0", "1", 63), &f) == GV_TRACE_FRAME);
    CHECK(f.handedness == 0);

    char line[2048];
    snprintf(line, sizeof line, "%s,-1", row("1.0", "1", 63));
    CHECK(gv_trace_parse_line(line, &f) == GV_TRACE_FRAME);
    CHECK(f.handedness == -1);
    CHECK_NEAR(f.lm[20].z, 62.5f, 1e-6); /* the landmarks are where they were */

    snprintf(line, sizeof line, "%s,1\n", row("1.0", "1", 63));
    CHECK(gv_trace_parse_line(line, &f) == GV_TRACE_FRAME);
    CHECK(f.handedness == 1);

    snprintf(line, sizeof line, "%s,2", row("1.0", "1", 63));
    CHECK(gv_trace_parse_line(line, &f) == GV_TRACE_MALFORMED); /* not a hand we know */
    snprintf(line, sizeof line, "%s,1,0.5", row("1.0", "1", 63));
    CHECK(gv_trace_parse_line(line, &f) == GV_TRACE_MALFORMED); /* 67 fields */
}

static void aspect_comment_is_read(void)
{
    float a = 0.0f;
    CHECK(gv_trace_parse_aspect("# aspect=1.777778\n", &a) == 1);
    CHECK_NEAR(a, 1.777778f, 1e-6);
    a = 0.0f;
    CHECK(gv_trace_parse_aspect("# something else\n", &a) == 0);
    CHECK(gv_trace_parse_aspect("# aspect=banana\n", &a) == 0);
    CHECK(gv_trace_parse_aspect("# aspect=-2\n", &a) == 0);
    CHECK_NEAR(a, 0.0f, 1e-9);
}

int main(void)
{
    fprintf(stderr, "test_trace\n");
    RUN(data_row_fills_the_frame);
    RUN(absent_hand_row_is_a_frame_with_present_zero);
    RUN(trailing_newline_is_accepted);
    RUN(comments_header_and_blank_lines_are_skipped);
    RUN(wrong_field_count_is_malformed);
    RUN(non_numeric_field_is_malformed);
    RUN(non_finite_values_are_malformed);
    RUN(handedness_column_is_optional);
    RUN(aspect_comment_is_read);
    return TEST_SUMMARY();
}
