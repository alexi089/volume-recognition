/*
 * filter.h - One Euro filter for a single noisy value (internal header).
 *
 * The problem: hand landmarks jitter by a few pixels even when the hand is
 * still. Smooth heavily and the jitter goes, but the slider lags behind a
 * moving hand. Smooth lightly and it keeps up, but the volume wobbles at rest.
 *
 * The One Euro filter (Casiez, Roussel, Vogel, 2012) picks the amount of
 * smoothing from the speed of the signal: slow movement gets a low cutoff
 * (heavy smoothing), fast movement gets a higher one (little lag).
 *
 *   min_cutoff  Hz. Smoothing at rest. Lower = steadier but laggier.
 *   beta        How quickly the cutoff opens up with speed. 0 = plain low-pass.
 *   d_cutoff    Hz. Smoothing applied to the speed estimate itself.
 */
#ifndef GV_FILTER_H
#define GV_FILTER_H

/* The struct is defined in the header, so callers can hold one by value (on
 * the stack or inside another struct) with no malloc. The price is that the
 * fields are visible; by convention only filter.c touches them. */
typedef struct {
    float  min_cutoff;
    float  beta;
    float  d_cutoff;
    int    has_prev;  /* 0 until the first sample arrives */
    float  x_prev;    /* previous filtered value */
    float  dx_prev;   /* previous filtered speed */
    double t_prev;    /* previous timestamp, seconds */
} gv_filter;

void  gv_filter_init(gv_filter *f, float min_cutoff, float beta, float d_cutoff);

/* Forget history; the next sample passes through unfiltered. */
void  gv_filter_reset(gv_filter *f);

/* Feed one sample taken at time t_sec; returns the filtered value. A timestamp
 * that does not advance returns the previous output rather than dividing by
 * a zero or negative time step. */
float gv_filter_apply(gv_filter *f, float x, double t_sec);

#endif /* GV_FILTER_H */
