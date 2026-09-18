/*
 * tune_flick - the flick's tuning harness.
 *
 * Two jobs, because tuning a gesture needs both halves of the evidence:
 *
 *   spans <trace.csv> [min_travel] [window_sec]
 *       THE PROBE. Prints every downward open-palm motion in one recording:
 *       a span reaching back `window_sec` (0.35 by default) whose oldest frame
 *       is an open palm and whose palm centre fell at least `min_travel`
 *       (0.75) hand-lengths. On the owner's own session these spans are the
 *       PROXY POSITIVES - attempted swipes. There are no intent labels in any
 *       recording, so "a positive" here means "a motion that looks like an
 *       attempted swipe by this measure", never "the owner meant to swipe".
 *       Each motion prints what every condition of the rule measured on it:
 *       the fall and the sideways travel in hand-lengths, the hand's size
 *       ratio across the span, whether the palm read as open at the oldest and
 *       at the newest sample, whether the newest sample is a fist, how far down
 *       the lowest landmark reached, and whether the state machine was idle.
 *       That is where flick_edge_margin and the relaxed end pose come from.
 *
 *   grid <edge_margin> <positives.csv> <corpus.csv>...
 *       THE TABLE. Sweeps the two motion constants of whatever rule currently
 *       ships (flick_drop_window_sec x flick_drop) through the REAL core -
 *       gv_update with an overridden gv_config, no rule reimplemented - and for
 *       each cell prints how many of the positives' motions a fire lands on and
 *       how many times it fires anywhere in the negative corpus. Zero corpus
 *       fires is the gate. The table this produced for the whole-window rule
 *       that shipped before hold-then-drop is in the design spec's Findings.
 *
 *   armdrop <edge_margin> <positives.csv> <corpus.csv>...
 *       HOLD, THEN DROP: the rule that now ships, re-implemented offline (see
 *       the comment above armdrop_run). It was written here first, as the study
 *       that chose the shape, so it is the reference gesture.c was built to
 *       match - and the mode still begins by checking the two against each
 *       other on every recording at the shipped constants. It then prints
 *       whether a held, still open palm happens before the positives at all,
 *       and sweeps the hold's own two constants against the drop's.
 *
 * Re-running it (from the repository root, with the binary built by
 * `make -C core build/tune_flick`):
 *
 *   core/build/tune_flick spans traces/real_rate_first_session.csv
 *   core/build/tune_flick grid 0.10 traces/real_rate_first_session.csv \
 *       traces/take1/real_*.csv traces/real_*_session.csv traces/probe_*.csv
 *
 * (the corpus is the 18 landmark recordings; keep the .events.csv files out of
 * the list, and keep the positives file out of the corpus.)
 *
 * The recordings are untracked, so the numbers this prints cannot be rebuilt
 * from the repository alone - the table it produced is written into the design
 * spec's Findings, which is the durable record.
 *
 * WHAT THIS TOOL SHARES WITH THE CORE, AND WHAT IT REPEATS.
 *
 * Shared, by including the core's own headers: all the geometry (pose.h -
 * gv_pose_is_open_palm, gv_pose_is_fist, gv_hand_scale, gv_palm_x / gv_palm_y,
 * gv_hand_lowest_y), the config and its defaults (gesture.h), and the two
 * frame-level rules that decide what is admissible at all - gv_frame_is_usable
 * and GV_MIN_HAND_SCALE (gesture_internal.h). Those two were static inside
 * gesture.c until this tool needed them: a tool that claims to reproduce the
 * core is worth nothing if the two disagree about which frames exist.
 *
 * Repeated here, on purpose: the flick's decision itself, and the state
 * machine's idle/refractory bookkeeping around it. The point of repeating it is
 * to sweep the constants without rebuilding the core - and the price is that
 * the copy can drift, so `armdrop` opens by comparing the two frame by frame
 * (see compare_rules) rather than taking the resemblance on trust.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gesture.h"
#include "gesture_internal.h" /* gv_frame_is_usable, GV_MIN_HAND_SCALE: see below */
#include "pose.h"
#include "trace.h"

/* --- loading a recording ------------------------------------------------- */

typedef struct {
    gv_frame *f;
    int       count, cap;
    float     aspect;
} trace;

static int trace_push(trace *tr, const gv_frame *f)
{
    if (tr->count == tr->cap) {
        int cap = tr->cap ? tr->cap * 2 : 1024;
        gv_frame *grown = realloc(tr->f, (size_t)cap * sizeof *grown);
        if (grown == NULL)
            return -1;
        tr->f = grown;
        tr->cap = cap;
    }
    tr->f[tr->count++] = *f;
    return 0;
}

static int trace_load(const char *path, trace *tr)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        perror(path);
        return -1;
    }
    tr->f = NULL;
    tr->count = tr->cap = 0;
    tr->aspect = 16.0f / 9.0f;

    char line[8192];
    while (fgets(line, sizeof line, fp) != NULL) {
        float aspect;
        if (gv_trace_parse_aspect(line, &aspect)) {
            tr->aspect = aspect;
            continue;
        }
        gv_frame f;
        if (gv_trace_parse_line(line, &f) == GV_TRACE_FRAME && trace_push(tr, &f) != 0) {
            fputs("out of memory\n", stderr);
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);
    return 0;
}

/* --- replaying one recording through the real core ----------------------- */

/* Runs the whole trace through gv_update with `cfg`. `states`, if not NULL,
 * receives the state reported on each frame. Fire times land in `fires` (up to
 * `max_fires`); the return value is how many fires there were, which may exceed
 * max_fires - the count is the gate, the times are only for matching. */
static int replay(const trace *tr, const gv_config *cfg, gv_state *states,
                  double *fires, int max_fires)
{
    gv_config c = *cfg;
    c.aspect = tr->aspect;
    gv_ctx *ctx = gv_create(&c);
    if (ctx == NULL) {
        fputs("gv_create refused the config\n", stderr);
        exit(2);
    }
    int count = 0;
    for (int i = 0; i < tr->count; i++) {
        gv_output out = gv_update(ctx, &tr->f[i]);
        if (states != NULL)
            states[i] = out.state;
        if (out.kind == GV_KIND_MINIMIZE && out.changed) {
            if (count < max_fires)
                fires[count] = tr->f[i].t_sec;
            count++;
        }
    }
    gv_destroy(ctx);
    return count;
}

/* --- the probe ----------------------------------------------------------- */

static int g_detections; /* motions found, for the grid's own report */

/* One motion: a run of frames that each end a qualifying span, with no quiet
 * gap of g_group_sec inside it. The default is one flick window: inside that
 * long, consecutive qualifying frames are re-detections of the same fall, and a
 * quiet gap of a whole window means the fall ended and another one began.
 * (Grouping by the 1.5 s refractory instead chains: every fall within 1.5 s of
 * the last extends the same motion, and one burst of swiping collapses to a
 * single "motion". That flatters the catch rate, so it is not the default.) */
static double g_group_sec = 0.35;

#define MAX_MOTIONS 256

typedef struct {
    double t_first, t_last; /* first and last frame that qualified */
    /* Everything below is measured on the span that qualified FIRST, which is
     * the moment the motion became a candidate. */
    /* What the shipped rule measures: the two ENDS of the whole window. */
    float  win_dy, win_dx;
    int    win_open_old;    /* ... and whether its oldest sample is an open palm */
    /* The best sub-span inside the same window: the deepest fall from any
     * open-palm frame in it. This is what the swipe actually did, and the gap
     * between the two columns is the price of judging the whole window. */
    float  best_dy, best_dx;
    float  size;            /* hand size at the newest over the sub-span's start */
    int    open_new, fist_new;
    /* The lowest landmark reached, in frame heights (1.0 is the bottom edge;
     * MediaPipe reports more than that for a hand partly out of the picture).
     * `low_min` is taken over every qualifying frame of the motion, because the
     * edge veto is judged frame by frame: a motion survives it if ANY of its
     * frames was still fully inside the picture. */
    float  lowest_y, low_min;
    int    idle;            /* was the state machine idle on that frame? */
    float  hold[3];         /* longest still-open run before it, per look-back
                             * (filled in by the arm-then-drop study only) */
} motion;

/* The probe proper. Walks the recording frame by frame; for each frame, back
 * over the frames inside `window` seconds (stopping at a tracking gap longer
 * than the release grace, exactly as the rule does), and finds the deepest fall
 * from any OPEN PALM in that span. A frame qualifies when that fall reaches
 * `min_travel` hand-lengths - the loosest reading of "an open palm moved down",
 * so that the motions found here are not already filtered by the rule being
 * tuned. Everything else is measured and printed, not required. */
static int probe(const trace *tr, const gv_config *cfg, float min_travel, float window,
                 const gv_state *states, motion *out, int max_out)
{
    const float aspect = tr->aspect;
    int n = 0;
    for (int i = 1; i < tr->count; i++) {
        const gv_frame *newest = &tr->f[i];
        if (!gv_frame_is_usable(newest))
            continue;
        int oldest = -1;
        for (int j = i - 1; j >= 0; j--) {
            if (!gv_frame_is_usable(&tr->f[j]))
                continue;
            if (newest->t_sec - tr->f[j].t_sec > (double)window)
                break;
            /* a hole longer than the grace: the hand may have been put there
             * rather than have travelled there */
            double next_t = tr->f[oldest < 0 ? i : oldest].t_sec;
            if (next_t - tr->f[j].t_sec > (double)cfg->release_grace_sec)
                break;
            oldest = j;
        }
        if (oldest < 0)
            continue;

        float y_new = gv_palm_y(newest), x_new = gv_palm_x(newest, aspect);
        int best = -1;
        float best_dy = 0.0f, best_dx = 0.0f;
        for (int j = oldest; j < i; j++) {
            if (!gv_frame_is_usable(&tr->f[j]) ||
                !gv_pose_is_open_palm(&tr->f[j], aspect, &cfg->pose))
                continue;
            float hl = gv_hand_scale(&tr->f[j], aspect);
            if (hl < GV_MIN_HAND_SCALE)
                continue;
            float dy = (y_new - gv_palm_y(&tr->f[j])) / hl;
            if (best < 0 || dy > best_dy) {
                best = j;
                best_dy = dy;
                best_dx = fabsf(x_new - gv_palm_x(&tr->f[j], aspect)) / hl;
            }
        }
        if (best < 0 || best_dy < min_travel)
            continue;

        float low = gv_hand_lowest_y(newest);
        if (n > 0 && newest->t_sec - out[n - 1].t_last < g_group_sec) {
            out[n - 1].t_last = newest->t_sec; /* same motion, still falling */
            if (low < out[n - 1].low_min)
                out[n - 1].low_min = low;
            continue;
        }
        g_detections++;
        if (n == max_out)
            break;
        motion *m = &out[n++];
        m->t_first = m->t_last = newest->t_sec;
        m->best_dy = best_dy;
        m->best_dx = best_dx;
        m->win_open_old = gv_pose_is_open_palm(&tr->f[oldest], aspect, &cfg->pose);
        float hl_win = gv_hand_scale(&tr->f[oldest], aspect);
        m->win_dy = hl_win > GV_MIN_HAND_SCALE ? (y_new - gv_palm_y(&tr->f[oldest])) / hl_win : 0.0f;
        m->win_dx = hl_win > GV_MIN_HAND_SCALE ? fabsf(x_new - gv_palm_x(&tr->f[oldest], aspect)) / hl_win : 0.0f;
        m->size = gv_hand_scale(newest, aspect) / gv_hand_scale(&tr->f[best], aspect);
        m->open_new = gv_pose_is_open_palm(newest, aspect, &cfg->pose);
        m->fist_new = gv_pose_is_fist(newest, aspect, &cfg->pose);
        m->lowest_y = m->low_min = low;
        m->idle = states[i] == GV_IDLE;
    }
    return n;
}

static void print_spans(const char *path, float min_travel, float window)
{
    trace tr;
    if (trace_load(path, &tr) != 0)
        exit(2);
    gv_config cfg = gv_default_config();
    cfg.aspect = tr.aspect;

    gv_state *states = calloc((size_t)tr.count, sizeof *states);
    double fires[64];
    if (states == NULL) { fputs("out of memory\n", stderr); exit(2); }
    replay(&tr, &cfg, states, fires, 64);

    motion m[MAX_MOTIONS];
    int n = probe(&tr, &cfg, min_travel, window, states, m, MAX_MOTIONS);

    printf("# %s: %d frames, %d motions (grouped at %.2f s): an open palm inside a\n"
           "#   %.2f s window, with the palm centre >= %.2f hand-lengths below it since.\n"
           "#   win_*: what the shipped rule measures (the two ends of the whole window).\n"
           "#   best_*: the deepest fall from any open palm inside the same window.\n",
           path, tr.count, n, g_group_sec, (double)window, (double)min_travel);
    puts("#   t_first  t_last  win_dy win_dx w_open  best_dy best_dx   size  open_new fist_new  lowest_y low_min idle");
    for (int i = 0; i < n; i++)
        printf("  %9.3f %8.3f %7.2f %6.2f %5d   %7.2f %7.2f %6.2f %7d %8d    %6.3f %6.3f %4d\n",
               m[i].t_first, m[i].t_last,
               (double)m[i].win_dy, (double)m[i].win_dx, m[i].win_open_old,
               (double)m[i].best_dy, (double)m[i].best_dx, (double)m[i].size,
               m[i].open_new, m[i].fist_new,
               (double)m[i].lowest_y, (double)m[i].low_min, m[i].idle);

    free(states);
    free(tr.f);
}

/* --- the table ----------------------------------------------------------- */

static const float k_windows[] = { 0.4f, 0.5f, 0.6f, 0.7f };
static const float k_travels[] = { 0.6f, 0.8f, 1.0f, 1.2f, 1.5f };
#define WINDOWS ((int)(sizeof k_windows / sizeof k_windows[0]))
#define TRAVELS ((int)(sizeof k_travels / sizeof k_travels[0]))

/* A fire counts for a motion if it lands inside it, give or take half a second
 * at each end: a longer window than the probe's finds the same swipe a frame or
 * two later, and the motion's own run is what the probe grouped. */
#define MATCH_SLACK 0.5

#define MAX_FIRES 4096

static void print_grid(float edge_margin, const char *positives, int corpus_count, char **corpus)
{
    trace pos;
    if (trace_load(positives, &pos) != 0)
        exit(2);
    gv_config base = gv_default_config();
    base.aspect = pos.aspect;
    base.flick_edge_margin = edge_margin;

    gv_state *states = calloc((size_t)pos.count, sizeof *states);
    double *fires = malloc(MAX_FIRES * sizeof *fires);
    if (states == NULL || fires == NULL) { fputs("out of memory\n", stderr); exit(2); }
    replay(&pos, &base, states, fires, MAX_FIRES);

    /* The proxy positives, found once at the probe's own 0.75 / 0.35 s, so
     * every cell of the table is scored against the same set of motions. */
    motion m[MAX_MOTIONS];
    int n = probe(&pos, &base, 0.75f, 0.35f, states, m, MAX_MOTIONS);
    int eligible = 0;
    for (int i = 0; i < n; i++)
        eligible += m[i].idle;
    printf("# positives: %s, %d motions, %d of them with the state machine idle\n",
           positives, n, eligible);
    printf("# corpus: %d recordings, flick_edge_margin %.2f\n", corpus_count, (double)edge_margin);
    puts("# drop_window drop  caught/eligible  corpus_fires");

    /* Every corpus recording is loaded once and replayed per cell. */
    trace *neg = calloc((size_t)corpus_count, sizeof *neg);
    if (neg == NULL) { fputs("out of memory\n", stderr); exit(2); }
    for (int i = 0; i < corpus_count; i++)
        if (trace_load(corpus[i], &neg[i]) != 0)
            exit(2);

    for (int wi = 0; wi < WINDOWS; wi++) {
        for (int ti = 0; ti < TRAVELS; ti++) {
            gv_config cfg = gv_default_config();
            cfg.flick_edge_margin = edge_margin;
            cfg.flick_drop_window_sec = k_windows[wi];
            cfg.flick_drop            = k_travels[ti];

            int fire_count = replay(&pos, &cfg, NULL, fires, MAX_FIRES);
            if (fire_count > MAX_FIRES)
                fire_count = MAX_FIRES;
            int caught = 0;
            for (int i = 0; i < n; i++) {
                if (!m[i].idle)
                    continue;
                for (int k = 0; k < fire_count; k++)
                    if (fires[k] >= m[i].t_first - MATCH_SLACK &&
                        fires[k] <= m[i].t_last + MATCH_SLACK) {
                        caught++;
                        break;
                    }
            }

            int false_fires = 0;
            char worst[512];
            worst[0] = '\0';
            for (int i = 0; i < corpus_count; i++) {
                int c = replay(&neg[i], &cfg, NULL, fires, MAX_FIRES);
                false_fires += c;
                if (c > 0)
                    snprintf(worst, sizeof worst, "%s x%d @ %.2f", corpus[i], c, fires[0]);
            }
            printf("  %.2f   %.1f       %2d/%2d            %d  %s\n",
                   (double)k_windows[wi], (double)k_travels[ti],
                   caught, eligible, false_fires, worst);
        }
    }

    for (int i = 0; i < corpus_count; i++)
        free(neg[i].f);
    free(neg);
    free(fires);
    free(states);
    free(pos.f);
}

/* --- the arm-then-drop variant (measured here only, NOT in gesture.c) ------ */

/*
 * The gesture's shape, and the one the volume and resize gestures already use:
 * hold a pose still to say "I mean it", then move. Written here as a study
 * before gesture.c had it; kept as the reference implementation.
 *
 *   1. an open palm HELD nearly still for `hold` seconds - present, open and
 *      moving slower than `still` hand-lengths per second on every frame of it;
 *   2. within `drop_window` seconds after any frame of that hold, the palm
 *      centre `drop` hand-lengths lower than it was held, sideways travel at
 *      most half the drop, the hand not a fist and still inside the picture
 *      (the same edge margin) at the end.
 *
 * Same refractory as the shipped rule (1.5 s after any release and after the
 * last flick) and only while the machine is IDLE. The tracking-gap rule is kept
 * too: a hole longer than release_grace_sec between the hold and the drop ends
 * the candidate, because a hand that vanishes and reappears lower down did not
 * necessarily travel there.
 *
 * The rule is evaluated HERE rather than through gv_update, so that its four
 * constants can be swept without rebuilding the core: the state machine is
 * replayed first (for the state and the release times) and the rule applied to
 * the result. print_armdrop starts by checking this offline rule against
 * gv_update at the shipped constants, on every recording, so a drift between
 * the two shows up before any number below is believed.
 */
typedef struct {
    float hold, still, drop_window, drop;
} armdrop;

/* Per-frame facts the variant needs, computed once per recording. */
typedef struct {
    float  x, y, scale, low, speed;
    int    present, open, fist;
    int    hold_start;   /* first frame of the still-open run ending here, or -1 */
} fdata;

static fdata *frames_measure(const trace *tr, const gv_config *cfg, float still)
{
    fdata *d = calloc((size_t)tr->count, sizeof *d);
    if (d == NULL) { fputs("out of memory\n", stderr); exit(2); }
    int prev = -1;
    for (int i = 0; i < tr->count; i++) {
        const gv_frame *f = &tr->f[i];
        d[i].hold_start = -1;
        /* Exactly what flick_remember admits, and for the same two reasons:
         * gv_frame_is_usable keeps NaN out, and a hand below GV_MIN_HAND_SCALE
         * is a detector glitch that is dropped WHOLE - it neither continues a
         * hold nor breaks one, and it is not a link in the tracking-gap chain
         * either. `present` here therefore means "in the core's flick
         * history", which is the only sense that matters below. */
        if (!gv_frame_is_usable(f))
            continue;
        float scale = gv_hand_scale(f, tr->aspect);
        if (scale < GV_MIN_HAND_SCALE)
            continue;
        d[i].present = 1;
        d[i].x = gv_palm_x(f, tr->aspect);
        d[i].y = gv_palm_y(f);
        d[i].scale = scale;
        d[i].low = gv_hand_lowest_y(f);
        d[i].open = gv_pose_is_open_palm(f, tr->aspect, &cfg->pose);
        d[i].fist = gv_pose_is_fist(f, tr->aspect, &cfg->pose);

        /* Speed against the previous frame that had a hand in it, in
         * hand-lengths per second. A hole longer than the grace breaks the run
         * whatever the speed says. */
        int broken = 1;
        if (prev >= 0) {
            double dt = tr->f[i].t_sec - tr->f[prev].t_sec;
            if (dt > 0.0 && dt <= (double)cfg->release_grace_sec) {
                d[i].speed = hypotf(d[i].x - d[prev].x, d[i].y - d[prev].y) /
                             d[i].scale / (float)dt;
                broken = 0;
            }
        }
        if (d[i].open && (broken || d[i].speed < still)) {
            /* A run starts here unless the frame before it was already in one. */
            d[i].hold_start = (!broken && prev >= 0 && d[prev].hold_start >= 0)
                            ? d[prev].hold_start : i;
        }
        prev = i;
    }
    return d;
}

/* How long the hand has been held still and open, ending at frame i. */
static double held_for(const trace *tr, const fdata *d, int i)
{
    return d[i].hold_start < 0 ? -1.0
         : tr->f[i].t_sec - tr->f[d[i].hold_start].t_sec;
}

/* Runs the rule over one recording, offline. Returns the number of fires. */
static int armdrop_run(const trace *tr, const gv_config *cfg, const armdrop *p,
                       float edge_margin, double *fires, int max_fires)
{
    gv_config c = *cfg;
    c.aspect = tr->aspect;
    gv_state *states = calloc((size_t)tr->count, sizeof *states);
    if (states == NULL) { fputs("out of memory\n", stderr); exit(2); }
    replay(tr, &c, states, fires, max_fires); /* the real machine, for its state */

    fdata *d = frames_measure(tr, &c, p->still);
    int count = 0;
    double last_release_t = -INFINITY, last_flick_t = -INFINITY;
    /* The core keeps its history in a ring of GV_FLICK_HISTORY entries and
     * empties it when a flick fires, so the search back can be cut short by the
     * buffer as well as by the drop window. Counting the entries the core would
     * be holding is what makes this a reproduction rather than a resemblance:
     * without it, a drop window longer than the buffer (1.33 s at 30 fps) is
     * searched further here than the core can search. */
    int hist = 0;
    gv_state prev_state = GV_IDLE;

    for (int i = 0; i < tr->count; i++) {
        double t = tr->f[i].t_sec;
        /* go_idle records the release on the frame the machine ARRIVES at idle,
         * so that is the frame to take it from - not the last busy one before
         * it, which is a frame earlier. */
        if (states[i] == GV_IDLE && prev_state != GV_IDLE)
            last_release_t = t;
        prev_state = states[i];

        if (d[i].present)
            hist++;   /* flick_remember takes this frame, before the check below */

        if (states[i] != GV_IDLE || !d[i].present)
            continue;
        if (hist < 2)
            continue;
        if (t - last_release_t < (double)cfg->flick_refractory_sec ||
            t - last_flick_t < (double)cfg->flick_refractory_sec)
            continue;
        /* gv_update also refuses a frame on which a gesture that has just ended
         * reports its final value. That cannot happen here: such a frame IS the
         * release frame, so the refractory above has already rejected it for any
         * flick_refractory_sec above zero. */
        if (d[i].fist || d[i].low > 1.0f - edge_margin)
            continue;

        int fired = 0;
        int budget = hist < GV_FLICK_HISTORY ? hist : GV_FLICK_HISTORY;
        int steps = 0;
        int oldest = i;
        for (int j = i - 1; j >= 0 && !fired; j--) {
            if (!d[j].present)
                continue;
            if (++steps > budget - 1)
                break;                      /* out of buffer, as the core would be */
            if (t - tr->f[j].t_sec > (double)p->drop_window)
                break;
            if (tr->f[oldest].t_sec - tr->f[j].t_sec > (double)cfg->release_grace_sec)
                break;
            oldest = j;
            if (held_for(tr, d, j) < (double)p->hold)
                continue;
            float dy = (d[i].y - d[j].y) / d[j].scale;
            float dx = fabsf(d[i].x - d[j].x) / d[j].scale;
            if (dy >= p->drop && dx <= cfg->flick_max_sideways * dy)
                fired = 1;
        }
        if (fired) {
            if (count < max_fires)
                fires[count] = t;
            count++;
            last_flick_t = t;
            hist = 0;   /* the core clears its history on firing */
        }
    }
    free(d);
    free(states);
    return count;
}

/* Do the rule here and the rule in the core decide the same way, frame by
 * frame? Both are run over the same recording with the same config, and the
 * frames each fires on are collected; a DISAGREEMENT is a frame that one fired
 * on and the other did not. `either` counts the frames at least one fired on -
 * how many decisions the comparison actually got to see.
 *
 * That last number is the point. Comparing fire COUNTS at the shipped constants
 * would be worthless: both sides are zero on every recording, and 0 == 0
 * demonstrates nothing about the rules, only that neither fires. So the caller
 * runs this at a deliberately loose setting as well, where both fire hundreds
 * of times and the two can actually differ. */
static int compare_rules(const trace *tr, const gv_config *cfg, const armdrop *p,
                         float edge_margin, int *either_out, int *mine_out, int *core_out)
{
    static double mine[MAX_FIRES], core[MAX_FIRES];
    gv_config c = *cfg;
    c.flick_edge_margin      = edge_margin;
    c.flick_hold_sec         = p->hold;
    c.flick_still_speed      = p->still;
    c.flick_drop_window_sec  = p->drop_window;
    c.flick_drop             = p->drop;

    int na = armdrop_run(tr, &c, p, edge_margin, mine, MAX_FIRES);
    int nb = replay(tr, &c, NULL, core, MAX_FIRES);
    if (na > MAX_FIRES) na = MAX_FIRES;
    if (nb > MAX_FIRES) nb = MAX_FIRES;
    *mine_out = na;
    *core_out = nb;

    /* Both lists are frame times in order, taken from the same trace, so equal
     * frames compare exactly; walking them together is a set difference. */
    int i = 0, j = 0, disagree = 0, either = 0;
    while (i < na || j < nb) {
        if (j >= nb || (i < na && mine[i] < core[j])) { i++; disagree++; }
        else if (i >= na || core[j] < mine[i])        { j++; disagree++; }
        else                                          { i++; j++; }
        either++;
    }
    *either_out = either;
    return disagree;
}

static const float k_holds[]    = { 0.2f, 0.3f };
static const float k_stills[]   = { 0.5f, 1.0f };
static const float k_dwindows[] = { 0.5f, 0.7f };
static const float k_drops[]    = { 0.6f, 0.8f, 1.0f };
#define HOLDS    ((int)(sizeof k_holds    / sizeof k_holds[0]))
#define STILLS   ((int)(sizeof k_stills   / sizeof k_stills[0]))
#define DWINDOWS ((int)(sizeof k_dwindows / sizeof k_dwindows[0]))
#define DROPS    ((int)(sizeof k_drops    / sizeof k_drops[0]))

static int caught_by(const motion *m, int n, const double *fires, int fire_count)
{
    int caught = 0;
    for (int i = 0; i < n; i++) {
        if (!m[i].idle)
            continue;
        for (int k = 0; k < fire_count; k++)
            if (fires[k] >= m[i].t_first - MATCH_SLACK &&
                fires[k] <= m[i].t_last + MATCH_SLACK) {
                caught++;
                break;
            }
    }
    return caught;
}

static void print_armdrop(float edge_margin, const char *positives,
                          int corpus_count, char **corpus)
{
    trace pos;
    if (trace_load(positives, &pos) != 0)
        exit(2);
    gv_config base = gv_default_config();
    base.aspect = pos.aspect;
    base.flick_edge_margin = edge_margin;

    gv_state *states = calloc((size_t)pos.count, sizeof *states);
    double *fires = malloc(MAX_FIRES * sizeof *fires);
    if (states == NULL || fires == NULL) { fputs("out of memory\n", stderr); exit(2); }
    replay(&pos, &base, states, fires, MAX_FIRES);

    motion m[MAX_MOTIONS];
    int n = probe(&pos, &base, 0.75f, 0.35f, states, m, MAX_MOTIONS);
    int eligible = 0;
    for (int i = 0; i < n; i++)
        eligible += m[i].idle;

    trace *neg = calloc((size_t)corpus_count, sizeof *neg);
    if (neg == NULL) { fputs("out of memory\n", stderr); exit(2); }
    for (int i = 0; i < corpus_count; i++)
        if (trace_load(corpus[i], &neg[i]) != 0)
            exit(2);

    /* --- the self-check: this rule against the one that ships -------------
     * This mode was written before gesture.c had the rule, so it is the
     * reference the C was built to match. Keeping the comparison means the two
     * cannot drift apart silently. It is run twice: at the constants that ship,
     * and at a loose setting chosen so that both rules fire a great many times,
     * because at the shipped constants neither fires on any recording and a
     * comparison of two zeros would prove nothing. */
    const struct { const char *what; armdrop p; } checks[] = {
        { "shipped", { base.flick_hold_sec, base.flick_still_speed,
                       base.flick_drop_window_sec, base.flick_drop } },
        { "loose  ", { 0.03f, 50.0f, 1.5f, 0.05f } },
    };
    for (int ci = 0; ci < 2; ci++) {
        int files = 0, disagree_total = 0, either_total = 0, mine_total = 0, core_total = 0;
        printf("# self-check (%s: hold %.2f, still %.1f, window %.2f, drop %.2f)\n",
               checks[ci].what, (double)checks[ci].p.hold, (double)checks[ci].p.still,
               (double)checks[ci].p.drop_window, (double)checks[ci].p.drop);
        for (int f = -1; f < corpus_count; f++) {
            const trace *tr = f < 0 ? &pos : &neg[f];
            const char *nm = f < 0 ? positives : corpus[f];
            int either = 0, na = 0, nb = 0;
            int d = compare_rules(tr, &base, &checks[ci].p, edge_margin, &either, &na, &nb);
            files++;
            disagree_total += d;
            either_total += either;
            mine_total += na;
            core_total += nb;
            if (d != 0) {
                const char *bn = strrchr(nm, '/');
                printf("#   %-34s offline %d, core %d, %d frames differ   <-- MISMATCH\n",
                       bn ? bn + 1 : nm, na, nb, d);
            }
        }
        printf("#   %d recordings, %d frames where either rule fired (offline %d, core %d),\n"
               "#   %d of them decided differently%s\n",
               files, either_total, mine_total, core_total, disagree_total,
               disagree_total == 0
                   ? (either_total == 0
                          ? " - but NOTHING fired, so this shows nothing"
                          : ": the two agree")
                   : " - THEY DISAGREE, the numbers below are suspect");
    }
    putchar('\n');

    /* --- did the owner ever pause before swiping? ------------------------- */
    printf("# Was there a held, still open palm before each proxy positive?\n");
    printf("# Longest run of open-palm frames under the speed limit ending within\n");
    printf("# 0.7 s (the widest drop_window) and, for context, within 1.5 s before\n");
    printf("# the motion starts, in seconds (-1: no such frame at all).\n");
    printf("#   t_first    idle   0.7s@still=0.5  0.7s@still=1.0   1.5s@still=1.0\n");
    int ever_02 = 0, ever_03 = 0, ever_wide = 0;
    static const double k_backs[] = { 0.7, 0.7, 1.5 };
    static const int    k_back_still[] = { 0, 1, 1 };
    for (int c = 0; c < 3; c++) {
        fdata *d = frames_measure(&pos, &base, k_stills[k_back_still[c]]);
        for (int i = 0; i < n; i++) {
            m[i].hold[c] = -1.0f;
            for (int k = 0; k < pos.count; k++) {
                double dt = m[i].t_first - pos.f[k].t_sec;
                if (dt < 0.0 || dt > k_backs[c])
                    continue;
                double h = held_for(&pos, d, k);
                if (h > (double)m[i].hold[c])
                    m[i].hold[c] = (float)h;
            }
        }
        free(d);
    }
    for (int i = 0; i < n; i++) {
        printf("  %9.3f %6d %14.2f %15.2f %16.2f\n", m[i].t_first, m[i].idle,
               (double)m[i].hold[0], (double)m[i].hold[1], (double)m[i].hold[2]);
        if (m[i].idle) {
            ever_02 += m[i].hold[1] >= 0.2f;
            ever_03 += m[i].hold[1] >= 0.3f;
            ever_wide += m[i].hold[2] >= 0.2f;
        }
    }
    printf("# of the %d eligible positives: %d had a 0.2 s hold and %d a 0.3 s hold\n"
           "# available within 0.7 s before them (at the loosest speed limit, 1.0);\n"
           "# %d had a 0.2 s hold anywhere in the 1.5 s before them.\n\n",
           eligible, ever_02, ever_03, ever_wide);

    /* --- does a held, still open palm happen AT ALL? ---------------------- */
    printf("# Sanity check on the hold detector: frames anywhere in each recording that\n"
           "# end a still-open run of at least 0.2 s / 0.3 s, at each speed limit, and\n"
           "# the longest such run in the whole recording.\n");
    printf("#   %-34s  still=0.5: >=0.2  >=0.3  longest    still=1.0: >=0.2  >=0.3  longest\n", "recording");
    for (int f = -1; f < corpus_count; f++) {
        const trace *tr = f < 0 ? &pos : &neg[f];
        const char *nm = f < 0 ? positives : corpus[f];
        const char *bn = strrchr(nm, '/');
        int c2[2] = { 0, 0 }, c3[2] = { 0, 0 };
        double longest[2] = { -1.0, -1.0 };
        for (int si = 0; si < STILLS; si++) {
            fdata *d = frames_measure(tr, &base, k_stills[si]);
            for (int i = 0; i < tr->count; i++) {
                double h = held_for(tr, d, i);
                c2[si] += h >= 0.2;
                c3[si] += h >= 0.3;
                if (h > longest[si])
                    longest[si] = h;
            }
            free(d);
        }
        printf("#   %-34s             %6d %6d   %6.2f               %6d %6d   %6.2f\n",
               bn ? bn + 1 : nm, c2[0], c3[0], longest[0], c2[1], c3[1], longest[1]);
    }
    putchar('\n');

    /* --- the grid --------------------------------------------------------- */
    printf("# arm-then-drop grid: caught/%d eligible positives, corpus fires over %d recordings\n",
           eligible, corpus_count);
    puts("# hold still drop_window drop  caught  corpus_fires  where");
    for (int hi = 0; hi < HOLDS; hi++)
    for (int si = 0; si < STILLS; si++)
    for (int wi = 0; wi < DWINDOWS; wi++)
    for (int di = 0; di < DROPS; di++) {
        armdrop p = { k_holds[hi], k_stills[si], k_dwindows[wi], k_drops[di] };
        int fc = armdrop_run(&pos, &base, &p, edge_margin, fires, MAX_FIRES);
        if (fc > MAX_FIRES)
            fc = MAX_FIRES;
        int caught = caught_by(m, n, fires, fc);

        int false_fires = 0;
        char where[512];
        where[0] = '\0';
        for (int i = 0; i < corpus_count; i++) {
            int c = armdrop_run(&neg[i], &base, &p, edge_margin, fires, MAX_FIRES);
            false_fires += c;
            if (c > 0) {
                char one[96];
                const char *base_name = strrchr(corpus[i], '/');
                snprintf(one, sizeof one, "%s x%d ", base_name ? base_name + 1 : corpus[i], c);
                strncat(where, one, sizeof where - strlen(where) - 1);
            }
        }
        printf("  %.1f  %.1f      %.1f      %.1f   %3d/%d       %3d        %s\n",
               (double)p.hold, (double)p.still, (double)p.drop_window, (double)p.drop,
               caught, eligible, false_fires, where);
    }

    for (int i = 0; i < corpus_count; i++)
        free(neg[i].f);
    free(neg);
    free(fires);
    free(states);
    free(pos.f);
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "spans") == 0) {
        float min_travel = argc > 3 ? (float)atof(argv[3]) : 0.75f;
        float window     = argc > 4 ? (float)atof(argv[4]) : 0.35f;
        if (argc > 5)
            g_group_sec = atof(argv[5]);
        print_spans(argv[2], min_travel, window);
        return 0;
    }
    if (argc >= 5 && strcmp(argv[1], "armdrop") == 0) {
        print_armdrop((float)atof(argv[2]), argv[3], argc - 4, argv + 4);
        return 0;
    }
    if (argc >= 5 && strcmp(argv[1], "grid") == 0) {
        print_grid((float)atof(argv[2]), argv[3], argc - 4, argv + 4);
        return 0;
    }
    fputs("usage: tune_flick spans <trace.csv> [min_travel] [window_sec] [group_sec]\n"
          "       tune_flick grid <edge_margin> <positives.csv> <corpus.csv>...\n"
          "       tune_flick armdrop <edge_margin> <positives.csv> <corpus.csv>...\n", stderr);
    return 2;
}
