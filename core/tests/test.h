/*
 * test.h - the whole test framework. C has no built-in one, and for a project
 * this size forty lines of macros beat a dependency.
 *
 * These are macros rather than functions for one reason: __FILE__, __LINE__
 * and #cond (the condition's source text) are filled in by the preprocessor at
 * the place the macro is used, so a failure points at the failing line.
 *
 * The `do { ... } while (0)` wrapper makes a multi-statement macro behave like
 * a single statement, so `if (x) CHECK(y); else ...` still parses correctly.
 */
#ifndef TEST_H
#define TEST_H

#include <math.h>
#include <stdio.h>

static int t_checks;
static int t_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        t_checks++;                                                              \
        if (!(cond)) {                                                           \
            t_failures++;                                                        \
            fprintf(stderr, "    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(actual, expected, tol)                                        \
    do {                                                                         \
        double t_a = (double)(actual), t_e = (double)(expected);                 \
        t_checks++;                                                              \
        if (!(fabs(t_a - t_e) <= (double)(tol))) {                               \
            t_failures++;                                                        \
            fprintf(stderr, "    FAIL %s:%d: %s = %g, expected %g +/- %g\n",     \
                    __FILE__, __LINE__, #actual, t_a, t_e, (double)(tol));       \
        }                                                                        \
    } while (0)

#define RUN(test_fn)                                                             \
    do {                                                                         \
        fprintf(stderr, "  %s\n", #test_fn);                                     \
        test_fn();                                                               \
    } while (0)

/* Call as `return TEST_SUMMARY();` at the end of main. A non-zero exit code is
 * how `make` learns that tests failed. */
#define TEST_SUMMARY()                                                           \
    (fprintf(stderr, "%d checks, %d failed\n", t_checks, t_failures), t_failures ? 1 : 0)

#endif /* TEST_H */
