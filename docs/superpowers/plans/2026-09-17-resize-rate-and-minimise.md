# Rate-controlled resize and swipe-down-to-minimise: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** the pinch steers the window's size at a rate (no pinch-first, unlimited range, no jitter at rest), window writes are throttled so the camera loop stays near 30 fps, and an open palm swiped down minimises the front window.

**Architecture:** the recogniser (`gesture.c`) keeps one state machine and gains a one-shot "flick" check on its idle path; the resize definition (`gestures/resize.c`) changes only its per-frame arithmetic; the glue (`step.c`) throttles writes and learns the minimise call; the window backend (`window_macos.c`) gains `win_minimize`; Python and the HUD learn one new `gesture` value. Every constant is a `gv_config` field.

**Tech Stack:** C11 (`-Wall -Wextra -Werror -Wpedantic`, tests under ASan+UBSan), Python 3.12 + ctypes, Swift/AppKit for the HUD, macOS Accessibility API.

**Spec:** `docs/superpowers/specs/2026-09-17-resize-rate-and-minimise-design.md`

## Global Constraints

- No gesture logic in Python. Test-first in C; mutation-check every new rule (copy the source to a scratch file, break it, compile the test against it, show a check fails).
- Constants are measured or, where no recording exists yet, marked as proxies in the code comment and the spec.
- Every change to `gv_config`, `gv_output`, `gv_frame`, `gv_step_result` or `gv_replay_summary` is mirrored in `app/bridge.py` the same task, field order verified by eye, before any `make test`.
- NEVER rebuild `libgesture.dylib` or `hud/build/volume-hud` while `app/main.py` runs. `make -C core test` and `make -C hud test` are always safe (test binaries only).
- `core/build/win minimize` / `restore` / `scale` / `bench` act on the owner's front window: tell the owner first.
- Nothing pushed mentions `.claude`, a worktree path, a session link, an emoji or the owner's name. Commit text: plain, `type(scope): what and why`, with the attribution trailer the tooling injects.
- Do not commit recordings (`traces/real_*`, takes, events). `make test > log 2>&1; echo $?`, never piped into `head`.
- Never start a guided recording without confirming the owner is at the camera. Never push `main`.

---

### Task 1: Rate control for the pinch

**Files:**
- Modify: `core/include/gesture.h` (`gv_config`), `core/src/gesture.c` (defaults, validation), `core/src/gestures/resize.c` (`resize_on_frame`), `core/tests/test_resize.c`, `app/bridge.py`, `app/test_bridge.py`, `traces/manifest.txt` (the synthetic pinch traces regenerate with different end scales)

**Interfaces:**
- Consumes: `gv_pinch_gap`, `gv_filter_apply`, the ring buffer `remember_scale`, `GV_SLEW_MAX_CREDIT_SEC` (0.1).
- Produces, in `gv_config`, `scale_max_slew` REPLACED IN PLACE by `resize_rate_max` and two fields appended:

```c
    float resize_rate_max;     /* fastest the scale may move, octaves (doublings) per second */
    float release_rollback_sec;
    /* Rate control (see the spec): offset = gap / gap_at_arm - 1. Inside
     * +/- resize_dead_zone nothing moves; beyond it the scale moves at
     * resize_rate_gain octaves/s per unit of offset past the dead zone, capped
     * at resize_rate_max. 0.08 sits clear of the measured 2.3% jitter step;
     * 4.5 and 2.0 give about one doubling per second at a 30% offset. All three
     * are proxies until the guided clips exist. */
    float resize_dead_zone;
    float resize_rate_gain;
```
Defaults: `resize_rate_max` 2.0, `resize_dead_zone` 0.08, `resize_rate_gain` 4.5, `scale_min` 0.1, `scale_max` 10.0. Validation: `resize_rate_max > 0` finite, `resize_dead_zone >= 0 && < 1` finite, `resize_rate_gain >= 0` finite.

- [ ] **Step 1: Failing tests.** In `core/tests/test_resize.c`, `pinned()` sets `c.resize_rate_max = 2.0f; c.resize_dead_zone = 0.08f; c.resize_rate_gain = 4.5f;` (and no longer `scale_max_slew`). Delete `expected_scale` and the tests that assume the phone-style ratio (`pinching_shrinks_and_spreading_grows`, `the_scale_limits_are_clamps`, `the_scale_cannot_jump`, `the_scale_cannot_jump_upward_either`, `a_gap_in_tracking_does_not_earn_a_big_jump`, `a_wobbling_gap_does_not_chatter`); their subjects come back below under rate control. Keep `arming_reports_resize_at_scale_one`, `a_deep_pinch_freezes_instead_of_collapsing_the_window` (re-check its numbers), the rollback tests, `each_resize_starts_again_from_one`, `a_pinched_start_does_not_arm`, `volume_and_resize_never_cross`, `the_frame_that_lets_go_is_never_an_armed_frame`, `nonsense_scale_config_is_rejected` (rename `scale_max_slew` lines to `resize_rate_max`, add `resize_dead_zone = 1.0f` rejected, `resize_rate_gain = -1.0f` rejected). Add a helper that holds a frame with a given gap OFFSET from the arming gap, by blending: the blend value `k` for a wanted offset is found by bisection on `gv_pinch_gap(pinch(k)) / gv_pinch_gap(pinch(0)) - 1` (k in [-0.4, 0.6]); assert the found offset is within 0.005.

```c
static float k_for_offset(float offset)
{
    float lo = -0.4f, hi = 0.6f, base = gv_pinch_gap(&(gv_frame){0}, REAL); /* placeholder, replaced below */
    gv_frame open = pinch(0.0f);
    base = gv_pinch_gap(&open, REAL);
    for (int i = 0; i < 40; i++) {                 /* the gap falls as k rises */
        float mid = (lo + hi) / 2.0f;
        gv_frame f = pinch(mid);
        if (gv_pinch_gap(&f, REAL) / base - 1.0f > offset) lo = mid; else hi = mid;
    }
    return (lo + hi) / 2.0f;
}
static void hold_offset(session *s, float offset, double seconds) { hold_pinch(s, k_for_offset(offset), seconds); }
```
(Write it without the placeholder line: compute `base` from `pinch(0.0f)` first.) Tests:

```c
static void inside_the_dead_zone_nothing_moves(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.5);            /* armed at 1.0 */
    hold_offset(&s, +0.05f, 1.0);         /* 5% wider: inside the 8% dead zone */
    CHECK(s.out.state == GV_ARMED);
    CHECK(s.changes == 0);
    CHECK_NEAR(s.out.scale, 1.0f, 1e-6);
    hold_offset(&s, -0.05f, 1.0);
    CHECK(s.changes == 0);
    finish(&s);
}

/* offset 0.30: rate = 4.5 * (0.30 - 0.08) = 0.99 octaves/s: one second later the
 * scale is 2^0.99 = 1.99 (wider) or 0.50 (closer). */
static void a_steady_offset_moves_the_scale_at_the_formulas_rate(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.5);
    hold_offset(&s, -0.30f, 1.0);
    CHECK_NEAR(s.out.scale, exp2f(-0.99f), 0.04);   /* the filter settles over the first frames */
    CHECK(s.changes > 20);                          /* a change most frames, not one jump */
    finish(&s);
}

static void wider_grows_without_pinching_first(void)
{
    session s = start();
    hold_pinch(&s, 0.25f, 0.5);           /* armed with the fingers part-way together */
    float k = k_for_offset(+0.30f);       /* relative to THIS arming gap: bisection must use gap_at_arm; see note */
    (void)k;
    /* Note for the implementer: k_for_offset measures against pinch(0); for a
     * different arming gap compute base from pinch(0.25f) instead. Write a
     * second helper k_for_offset_from(float k0, float offset). */
    finish(&s);
}
```
Replace the third test's body with the real one once `k_for_offset_from` exists: arm at `k0 = 0.25`, hold `+0.30` offset for 1 s, `CHECK(s.out.scale > 1.8f)`. Then:

```c
static void returning_to_neutral_stops_it(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.5);
    hold_offset(&s, -0.30f, 0.5);
    float held = s.out.scale;
    CHECK(held < 0.8f);
    int before = s.changes;
    hold_pinch(&s, 0.0f, 1.0);            /* back to the arming gap */
    CHECK(s.changes - before <= 3);       /* the filter's settling, then nothing */
    CHECK_NEAR(s.out.scale, held, 0.05);
    finish(&s);
}

static void the_rate_is_capped(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.5);
    hold_offset(&s, -0.60f, 1.0);         /* formula says 2.34 octaves/s; the cap is 2.0 */
    CHECK_NEAR(s.out.scale, exp2f(-2.0f), 0.06);
    CHECK(s.max_octaves_per_frame <= 2.0f * (float)DT * 1.05f);
    finish(&s);
}

static void lost_frames_earn_no_more_than_a_tenth_of_a_second(void)
{
    session s = start();
    hold_pinch(&s, 0.0f, 0.5);
    hold_offset(&s, -0.30f, 0.2);
    float before = s.out.scale;
    s.t += 1.0;                            /* the app stalls; the hand is still there, still offset */
    gv_frame f = pinch(k_for_offset(-0.30f));
    frame(&s, &f);                         /* release (stall > grace) then re-judged: state must not be ARMED */
    CHECK(s.out.state != GV_ARMED);
    (void)before;
    finish(&s);
}
```
That last test shows the stall path releases (already covered elsewhere); the credit cap is better tested directly: hold offset, then feed the SAME frame with `s.t` advanced by 0.09 s per frame (fewer frames, same elapsed time) versus DT and compare: within the 0.1 s cap the scale after 0.9 s must match to 3%; and with `s.t` advanced by 0.2 s per frame (past the cap, but under the 0.25 s grace) the scale must move at most `2^(rate*0.1)` per frame. Write it as `credit_across_a_slow_frame_is_capped`.

```c
static void a_fist_still_rolls_back(void)
{
    /* the same closing motion as closing_the_hand_ends_the_resize_where_it_was,
     * after shrinking by rate to about 0.7 and holding still (neutral) for 0.6 s */
}
```
Fill it in by copying that test's closing loop; the held size must survive.

- [ ] **Step 2: Run** `make -C core build/test_resize && core/build/test_resize`. Expected: compile errors on the new config fields (the header has not changed). Add the fields to `gv_config` (Interfaces above), rename in `gesture.c` defaults and validation, in `app/bridge.py` (`("resize_rate_max", C.c_float)` in place of `scale_max_slew`; `resize_dead_zone`, `resize_rate_gain` appended after `release_rollback_sec`), in `app/test_bridge.py` (`abs(c.resize_rate_gain - 4.5) < 1e-6` as the last-field pin). Re-run: the new tests FAIL (the old arithmetic ignores the dead zone).
- [ ] **Step 3: Implement** `resize_on_frame`:

```c
static int resize_on_frame(gv_ctx *ctx, const gv_frame *f, float x, float y, double t, double since_t)
{
    (void)x; (void)y; (void)since_t;
    const gv_config *cfg = &ctx->cfg;

    /* Rate control, like a joystick: how far the fingers sit from where they
     * were at arming sets how FAST the size changes, not the size itself. So
     * a few centimetres of finger travel reach any size, growing needs no
     * pinch first, and a hand at rest (inside the dead zone) moves nothing,
     * whatever the fingertip tracking is doing. */
    float gap = gv_filter_apply(&ctx->gap_filter, gv_pinch_gap(f, cfg->aspect), t);
    float offset = gap / ctx->gap_at_arm - 1.0f;
    float beyond = fabsf(offset) - cfg->resize_dead_zone;
    float rate = 0.0f;                                     /* octaves per second */
    if (beyond > 0.0f) {
        rate = cfg->resize_rate_gain * beyond;
        if (rate > cfg->resize_rate_max)
            rate = cfg->resize_rate_max;
        if (offset < 0.0f)
            rate = -rate;
    }

    /* Time since the last frame, credited only up to a cap: a gap in tracking
     * cannot bank up permission for one big jump. */
    double since = t - ctx->slew_t;
    if (since > GV_SLEW_MAX_CREDIT_SEC)
        since = GV_SLEW_MAX_CREDIT_SEC;
    ctx->slew_t = t;

    /* Integrate in log space and clamp. `target` tracks the unrounded value;
     * `reported_scale` is what the caller last heard. */
    float target = ctx->scale_target * exp2f(rate * (float)since);
    if (target < cfg->scale_min) target = cfg->scale_min;
    if (target > cfg->scale_max) target = cfg->scale_max;
    ctx->scale_target = target;

    int at_end = (target == cfg->scale_min || target == cfg->scale_max) && target != ctx->reported_scale;
    int changed = 0;
    if (at_end || fabsf(target / ctx->reported_scale - 1.0f) >= cfg->scale_dead_band) {
        ctx->reported_scale = target;
        changed = 1;
    }
    remember_scale(ctx, t);
    return changed;
}
```
`gv_ctx` gains `float scale_target;` next to `reported_scale` (set to 1.0 in `resize_on_armed` and `resize_on_idle`; the rollback sets both `reported_scale` and `scale_target` to the rolled value). Update the file comment at the top of `resize.c` (it describes the phone-style ratio) and the teaching comment for the credit cap.
- [ ] **Step 4:** `make -C core test`: all green. Regenerate the synthetic pinch traces (`make -C core pinch-traces`), look at what `replay` now says for each, and set the manifest expectations to what the motion now means: `synthetic_pinch_shrink` (fingers closer, held) still `shrink`; `synthetic_pinch_grow` (arms part-way together, spreads) still `grow`; `synthetic_pinch_then_fist` still `shrink`; `glimpse` `none`. If a trace no longer meets its word, change the MOTION in `gen_pinch_traces.c` (e.g. hold the offset longer), not the expectation. Mutation-check: dead zone removed (`beyond = fabsf(offset)`), sign dropped, cap removed, credit cap removed, integration in linear space (`target += rate * since`), `scale_target` not clamped. Each must fail a test.
- [ ] **Step 5: Evidence on the live recording.** `core/build/replay traces/real_resize_second_session.csv stats` before and after (the "before" number is in the spec: 15% reversals). Add to `replay`'s stats a line `resize: N direction reversals between consecutive writes (P%)` computed exactly as the spec's Python did (sign change of consecutive relative steps). Record both numbers in the spec's "Findings" section for this design. Note honestly that the recording was made under the OLD mapping (the hands moved to steer a ratio, not a rate), so the after-number is a plausibility check, not proof.
- [ ] **Step 6:** `make test > log 2>&1; echo $?` -> 0 (app not running). **Commit** `feat(core): the pinch steers the window size at a rate`.

---

### Task 2: Throttle window writes

**Files:**
- Modify: `core/include/gesture.h` (`gv_config`), `core/src/gesture.c` (default 0.06, validation `>= 0` finite), `core/src/gesture_internal.h` (ctx fields), `core/src/step.c`, `core/tests/test_step.c`, `app/bridge.py`, `app/test_bridge.py`

**Interfaces:**
- Produces: `gv_config.win_write_interval_sec` (appended); `gv_ctx` gains `double step_last_write_t; int step_write_pending;`.

- [ ] **Step 1: Failing tests** in `test_step.c` (the fake window backend records `fake_apply_calls`, `fake_last_scale`):

```c
static void window_writes_are_throttled_but_nothing_is_lost(void)
{
    fake_reset(0.40f);
    gv_config cfg = gv_default_config();
    cfg.aspect = FIXTURE_REAL_ASPECT;
    cfg.win_write_interval_sec = 0.06f;    /* about every other frame at 30 fps */
    gv_ctx *ctx = gv_create(&cfg);
    now = 0.0;
    step_pinch(ctx, 0.0f, 0.0f, 0.5);
    int before = fake_apply_calls;
    step_pinch(ctx, 0.0f, 0.3f, 1.0);      /* 30 frames of steady change */
    int writes = fake_apply_calls - before;
    CHECK(writes >= 14 && writes <= 16);   /* every other frame, not every frame */
    CHECK_NEAR(fake_last_scale, last.out.scale, 1e-6); /* the latest value, never a stale one */
    gv_destroy(ctx);
}

static void a_change_that_arrives_inside_the_interval_is_written_on_the_next_allowed_frame(void)
{
    fake_reset(0.40f);
    gv_config cfg = gv_default_config();
    cfg.aspect = FIXTURE_REAL_ASPECT;
    cfg.win_write_interval_sec = 0.06f;
    gv_ctx *ctx = gv_create(&cfg);
    now = 0.0;
    step_pinch(ctx, 0.0f, 0.0f, 0.5);
    step_pinch(ctx, 0.0f, 0.3f, 0.3);      /* moving */
    /* one more frame that changes, then two frames that do not: the change must land */
    step_pinch(ctx, 0.3f, 0.32f, DT);
    float wanted = last.out.scale;
    step_pinch(ctx, 0.32f, 0.32f, 2 * DT);
    CHECK_NEAR(fake_last_scale, wanted, 1e-6);
    gv_destroy(ctx);
}

static void the_last_word_is_written_whatever_the_interval(void)
{
    /* as the_rolled_back_scale_is_written_before_the_window_is_let_go, with
     * cfg.win_write_interval_sec = 1.0f: the rolled-back scale must still be written */
}
```
Fill in the third by copying that test with the interval set to 1.0. Also assert in the first test that `fake_applies_after_release == 0` still holds at the end of the existing release tests (they run with the default interval).
- [ ] **Step 2:** run; FAIL (the field does not exist; add it, the default and validation; bridge mirror; re-run: `writes` is 30).
- [ ] **Step 3: Implement** in `step.c`, replacing the `if (ctx->step_window_held && r.out.changed)` write:

```c
        /* Writes are throttled: one costs about 14 ms and dropped the camera
         * loop to 16 fps when done every frame. A change inside the interval
         * is not lost, it is written on the first frame the interval allows;
         * a gesture's last word (state no longer ARMED) is written at once. */
        if (ctx->step_window_held && r.out.changed)
            ctx->step_write_pending = 1;
        const double t = frame->t_sec;
        const int due = t - ctx->step_last_write_t >= (double)ctx->cfg.win_write_interval_sec || !armed_resize;
        if (ctx->step_window_held && ctx->step_write_pending && due) {
            r.win = window->apply(r.out.scale);
            ctx->step_last_write_t = t;
            ctx->step_write_pending = 0;
        }
```
(`step.c` includes `gesture_internal.h`, so `ctx->cfg` is visible.) On acquire set `step_last_write_t = -INFINITY` (include `<math.h>`) and `step_write_pending = 0`.
- [ ] **Step 4:** pass. Mutation-check: `due` always true (the first test fails); pending never set (second fails); `!armed_resize` dropped (third fails). `make test` -> 0. **Commit** `feat(core): write the window at most every other frame`.

---

### Task 3: The open-palm pose, measured

**Files:**
- Modify: `core/src/pose.h`, `core/src/pose.c`, `core/tests/test_pose.c`; create `core/tools/measure_palm.py`

**Interfaces:**
- Produces: `int gv_pose_is_open_palm(const gv_frame *f, float aspect, const gv_pose_limits *lim);` = index, middle, ring and pinky `gv_finger_is_extended` AND `gv_thumb_is_out`.

- [ ] **Step 1: Measure first.** `core/tools/measure_palm.py` (copy `measure_thumb.py`'s shape; read traces with the inline loader `clear.py` used, not `sweep.load_trace`, so it runs with a stale library) prints, for `traces/take1/real_waving.csv`, `real_talking_hands.csv`, `real_cup_reach.csv` and `real_resize_second_session.csv`, the fraction of hand frames on which all four fingers read extended and the thumb reads out, using the rules in `pose.c` re-implemented in Python (as `measure_thumb.py` does). Also for `fixture_real_open_palm` (from `fixtures.h`). Record the numbers in the spec's Findings. Expected: the open-palm fixture passes; the waving clip has many such frames (that is the point: the pose alone must never fire anything); the resize session has few.
- [ ] **Step 2: Failing tests** in `test_pose.c`: `fixture_real_open_palm` is an open palm; `fixture_victory`, `fixture_real_pinch_open`, `fixture_real_fist`, `fixture_real_relaxed_two` are not; `with_finger_folded(open palm, PINKY)` is not (one finger down is enough to say no); `mirrored(open palm)` still is.
- [ ] **Step 3:** implement (four `gv_finger_is_extended` calls and `gv_thumb_is_out`); pass; mutant: drop the pinky test (the folded-pinky test must fail); drop the thumb test (needs a fixture: `with_thumb_bent(open palm)` must NOT be an open palm; add that test).
- [ ] **Step 4: Commit** `feat(core): judge an open palm`.

---

### Task 4: The flick

**Files:**
- Modify: `core/include/gesture.h` (`GV_KIND_MINIMIZE = 3`, config fields), `core/src/gesture.c`, `core/src/gesture_internal.h`, `core/tests/test_gesture.c`; create `core/tests/test_flick.c`; `core/Makefile` (`TESTS += test_flick`, rule as `test_resize`); `app/bridge.py` (`KIND_MINIMIZE`, `KIND_NAMES[3] = "minimize"`, config mirror)

**Interfaces:**
- Produces, in `gv_config` (appended): `flick_travel` 1.5 (hand-lengths), `flick_window_sec` 0.35, `flick_max_sideways` 0.5, `flick_refractory_sec` 1.0. Validation: travel > 0, window > 0, sideways >= 0, refractory >= 0, all finite.
- In `gv_ctx`:

```c
#define GV_FLICK_HISTORY 16   /* about half a second at 30 fps: longer than flick_window_sec */
    struct { double t; float x, y, scale; int open; } flick_hist[GV_FLICK_HISTORY];
    int    flick_next, flick_count;
    double last_release_t;    /* when any gesture last let go (ARMING or ARMED -> IDLE) */
    double last_flick_t;
```
- Output on the firing frame: `state` IDLE, `kind` GV_KIND_MINIMIZE, `changed` 1, `scale` 1.0, `volume` unchanged. Every other frame: as before (kind NONE while idle).

- [ ] **Step 1: Failing tests** `core/tests/test_flick.c`, in `test_resize.c`'s style (`session`, `frame`, `hold`; count `s.flicks` = frames with `kind == GV_KIND_MINIMIZE && changed`). A helper moves the open-palm fixture:

```c
/* The open palm moved so that its palm centre travels `dx_hands` sideways and
 * `dy_hands` down (in hand-lengths of THIS fixture) over `seconds`. */
static void sweep(session *s, float dx_hands, float dy_hands, double seconds)
{
    const gv_frame *hand = &fixture_real_open_palm;
    float hl = gv_hand_scale(hand, REAL);
    int n = (int)(seconds / DT + 0.5);
    for (int i = 1; i <= n; i++) {
        gv_frame f = *hand;
        float k = (float)i / (float)n;
        for (int j = 0; j < GV_NUM_LANDMARKS; j++) {
            f.lm[j].x += dx_hands * hl * k / REAL;   /* x is stored in frame widths; hand lengths are in frame heights */
            f.lm[j].y += dy_hands * hl * k;
        }
        frame(s, &f);
    }
}
```
Tests: `a_fast_downward_open_palm_fires_once` (hold 0.3 s, sweep 0, +2.0 over 0.25 s, hold 0.5 s -> exactly 1 flick, state IDLE throughout, volume unchanged); `too_slow_does_not_fire` (+2.0 over 1.0 s -> 0); `not_far_enough_does_not_fire` (+1.0 over 0.25 s -> 0); `sideways_does_not_fire` (+2.0 dx, +2.0 dy -> 0; and +2.0 dx alone -> 0); `upward_does_not_fire` (-2.0 dy -> 0); `a_gap_in_tracking_breaks_the_sweep` (sweep half, 0.3 s of NULL frames, sweep the rest -> 0); `a_closed_hand_sweeping_down_does_not_fire` (same motion with `fixture_real_fist` -> 0); `within_a_second_of_letting_go_nothing_fires` (arm volume with `fixture_real_tight_two` for 0.5 s, then NULL for 0.3 s (release), then the open-palm sweep at once -> 0; the same sweep 1.2 s after the release -> 1); `a_second_flick_needs_a_second` (two sweeps back to back -> 1; with 1.2 s between -> 2); `a_flick_never_fires_while_armed` (arm volume, then sweep with the open palm: the stay pose ends the slide, and the sweep is inside the refractory -> 0 flicks).
- [ ] **Step 2:** run; FAIL on the kind (does not exist). Add `GV_KIND_MINIMIZE`, the config fields, defaults, validation (`test_gesture.c` nonsense-config test gains the four), bridge mirror (`KIND_MINIMIZE = 3`, `KIND_NAMES`, fields). Re-run: the positive tests FAIL (0 flicks).
- [ ] **Step 3: Implement** in `gesture.c`. In `go_idle`, before clearing: `if (ctx->state != GV_IDLE) ctx->last_release_t = <t>;` (pass `t` into `go_idle`; every caller has it). After the existing arm/armed handling and before building `out`:

```c
    /* The flick: a one-shot gesture that never arms. Recorded on every usable
     * frame (so the history is there the moment the state is idle), judged
     * only while idle, outside the refractory after any release: lowering the
     * hand after a slide is itself a fast downward motion. */
    if (usable)
        flick_remember(ctx, frame, t);
    int flicked = 0;
    if (ctx->state == GV_IDLE && usable &&
        t - ctx->last_release_t >= (double)cfg->flick_refractory_sec &&
        t - ctx->last_flick_t   >= (double)cfg->flick_refractory_sec &&
        flick_seen(ctx, t)) {
        ctx->last_flick_t = t;
        flicked = 1;
    }
```
with

```c
static void flick_remember(gv_ctx *ctx, const gv_frame *f, double t)
{
    const gv_config *cfg = &ctx->cfg;
    int i = ctx->flick_next;
    ctx->flick_hist[i].t = t;
    ctx->flick_hist[i].x = gv_palm_x(f, cfg->aspect);
    ctx->flick_hist[i].y = gv_palm_y(f);
    ctx->flick_hist[i].scale = gv_hand_scale(f, cfg->aspect);
    ctx->flick_hist[i].open = gv_pose_is_open_palm(f, cfg->aspect, &cfg->pose);
    ctx->flick_next = (i + 1) % GV_FLICK_HISTORY;
    if (ctx->flick_count < GV_FLICK_HISTORY) ctx->flick_count++;
}

/* Over the last flick_window_sec: open palm first and last, no gap longer than
 * the grace, and the palm centre went down by flick_travel hand-lengths with
 * little sideways travel. Units: x and y are in frame heights (see pose.h),
 * so dividing by the hand scale gives hand-lengths on both axes. */
static int flick_seen(const gv_ctx *ctx, double now)
{
    const gv_config *cfg = &ctx->cfg;
    if (ctx->flick_count < 2) return 0;
    int newest = (ctx->flick_next - 1 + GV_FLICK_HISTORY) % GV_FLICK_HISTORY;
    if (!ctx->flick_hist[newest].open) return 0;
    int oldest = newest;
    for (int back = 2; back <= ctx->flick_count; back++) {
        int i = (ctx->flick_next - back + GV_FLICK_HISTORY) % GV_FLICK_HISTORY;
        int later = (i + 1) % GV_FLICK_HISTORY;
        if (now - ctx->flick_hist[i].t > (double)cfg->flick_window_sec) break;
        if (ctx->flick_hist[later].t - ctx->flick_hist[i].t > (double)cfg->release_grace_sec) return 0;
        oldest = i;
    }
    if (oldest == newest || !ctx->flick_hist[oldest].open) return 0;
    float hl = ctx->flick_hist[oldest].scale;
    float dy = (ctx->flick_hist[newest].y - ctx->flick_hist[oldest].y) / hl; /* down is +y */
    float dx = fabsf(ctx->flick_hist[newest].x - ctx->flick_hist[oldest].x) / hl;
    return dy >= cfg->flick_travel && dx <= cfg->flick_max_sideways * dy;
}
```
Check `gv_palm_x`'s units in `pose.h` before trusting the comment (the README says "palm_x_is_in_frame_heights"). Build `out` as before, then `if (flicked) { out.kind = GV_KIND_MINIMIZE; out.changed = 1; }`. After a flick, clear the history (`flick_count = 0`) so one motion cannot fire twice as the window slides.
- [ ] **Step 4:** all green. Mutation-check: refractory after release dropped; refractory after flick dropped; sideways test dropped; travel compared with `>` 0; the "open at the oldest" check dropped; history not cleared after firing. `make -C core test` -> 0.
- [ ] **Step 5: The negative corpus.** `replay` counts flicks (`stats` prints `flicks: N`); run it over EVERY recording: `traces/take1/real_*.csv`, `traces/real_*_session.csv`, `traces/probe_*.csv`. The bar is zero flicks on all of them. If any fires, print the frame times, look at what the hand did (`gen_fixtures.py` can lift the frame), and tune by the spec's order: refractory, travel, then a palm-facing rule. Record the per-recording counts in the spec's Findings, including the tuning that was needed.
- [ ] **Step 6:** `make test` -> 0. **Commit** `feat(core): an open palm swiped down is a flick to minimise`.

---

### Task 5: Minimise through the Accessibility API

**Files:**
- Modify: `core/include/window.h`, `core/src/window_macos.c`, `core/include/step.h` (`gv_window_ops.minimize`), `core/src/step.c`, `core/src/step_system.c`, `core/tests/test_step.c`, `core/tools/win.c`, `core/Makefile` (nothing new to link)

**Interfaces:**

```c
/* window.h */
win_status win_minimize(void);   /* the front window, by win_acquire's rule; verified by read-back; nothing stays held */
win_status win_restore(void);    /* the CLI's way back: un-minimise the window win_minimize last minimised */
/* step.h */
typedef struct { win_status (*acquire)(void); win_status (*apply)(float scale); void (*release)(void);
                 win_status (*minimize)(void); } gv_window_ops;
```

- [ ] **Step 1: Failing tests** in `test_step.c`: the fake gains `fake_minimize_calls` and `fake_minimize_status`; `fake_ops` gains the fourth pointer. Tests: `a_flick_minimises_the_front_window_once` (drive the open-palm sweep from Task 4's helper through `gv_step`: `fake_minimize_calls == 1`, `fake_acquire_calls == 0` (minimise does its own taking hold, inside the backend), `last.win == WIN_OK` on the firing frame); `a_failed_minimise_is_reported_on_that_frame` (`fake_minimize_status = WIN_ERR_NOT_TRUSTED` -> `last.win` on the firing frame, and `WIN_OK` on the next).
- [ ] **Step 2:** run; FAIL. Implement: in `step.c` after the resize block: `if (r.out.kind == GV_KIND_MINIMIZE && r.out.changed) r.win = window->minimize();`. In `window_macos.c`, split `win_acquire` so the "find the front window's element" part is a static `take_front_window(int need_resizable, AXUIElementRef *out)` shared by both; `win_minimize` takes it (no resizable check), `AXUIElementSetAttributeValue(w, kAXMinimizedAttribute, kCFBooleanTrue)`, reads the attribute back (`AXUIElementCopyAttributeValue` -> `CFBooleanGetValue`), `WIN_ERR_IGNORED` if still false, releases the element but keeps a retained copy in `g_minimised` for `win_restore`. `win_restore` writes false to that element, reads back, releases it. Every Copy/Create released on every path (the file's own rule).
- [ ] **Step 3:** `win.c` gains `win minimize` (minimise the front window, sleep 1 s, restore it, print each read-back) and `win restore`. Build (`make -C core build/win`). **Verify by hand, with the owner told first and NOT while the app runs:** `core/build/win minimize` from a terminal: the terminal window (or whatever is in front) minimises to the Dock and comes back after a second. Then, with the camera OPEN (`app/main.py` running with `--no-hud` is not enough: the tool runs in another process, so use the live-conditions probe from the pinch-resize spec instead, calling `win_minimize` through ctypes with the camera open), confirm it still finds a real window and not the camera indicator. Record the outcome in the spec.
- [ ] **Step 4:** `make test` -> 0. **Commit** `feat(core): minimise the front window; the flick reaches the backend`.

---

### Task 6: Replay, verdict, bridge, Python, HUD

**Files:**
- Modify: `core/src/verdict.h`, `verdict.c`, `core/tests/test_verdict.c`, `core/tools/replay.c`, `core/src/step.c` (`gv_abi_replay_summary_size`), `core/include/step.h`, `app/bridge.py` (`ReplaySummary` gains `flicks`; `_check_abi` gains the summary), `app/test_bridge.py`, `app/main.py` (overlay `MINIMISE` label for one second via `ErrorLatch`-style latch; HUD line), `hud/Sources/HudMessage.swift` (`HudGesture.minimize`), `hud/Sources/VisibilityModel.swift` (a `minimize` message shows the pill for 1 s even though `state` is idle), `hud/Sources/AppController.swift` (glyph `arrow.down.right.and.arrow.up.left`, percent text hidden), `hud/Tests/test_message.swift`, `hud/README.md`, `core/tools/sweep.py` (report flicks)

**Interfaces:**
- `gv_replay_summary` gains `int flicks;` (appended). `gv_expectation` gains `GV_EXPECT_MINIMIZE`: exactly one flick and no other change. `none`, `up`, `down`, `grow`, `shrink` all require `flicks == 0`.
- `unsigned gv_abi_replay_summary_size(void);` in `step.c`, checked in `bridge._check_abi` (closes the open review item).

- [ ] **Step 1: Failing tests:** `test_verdict.c` (`minimize` parses; `minimize` needs exactly one flick and nothing else; every other word fails on a flick); `test_message.swift` (`"gesture":"minimize"` parses; a visibility test: an idle message with gesture minimize shows the pill and it hides about a second later, in the reducer's existing style); `test_bridge.py` (`ReplaySummary` size matches; `bridge.verdict("minimize", 0, 0, 0, 1.0, flicks=1)`).
- [ ] **Step 2:** implement each; `replay` prints flicks in stats and in the one-line verdict; `main.py`'s events file gains a `flick` column (1 on the firing frame) at the end; HUD README's contract table gains the value.
- [ ] **Step 3:** `make test` -> 0. **Commit** `feat(app): minimise reaches replay, the overlay and the HUD`.

---

### Task 7: Live check, guided clips, docs, review

- [ ] **Step 1: With the owner at the camera (confirm first):** `make run --record traces/real_rate_first_session.csv`. Ask three things and write the answers down verbatim: does the pinch feel like steering (does it stop when the fingers return to neutral), is the jitter gone, does the swipe down minimise and did anything minimise that should not have. Then `app/record_scripted.sh` after adding `real_pinch_rate|shrink|...`, `real_pinch_rate_grow|grow|...` and `real_swipe_minimise|minimize|...` to `CLIPS`, and re-run the five negatives.
- [ ] **Step 2: Measure:** `replay ... stats` on the new recording: reversals (the jitter number), fps while writing (from the events file, as in the first live session's analysis), flicks on the negatives (zero required), session counts. Tune the four proxies with `sweep.py` by the usual rule. Write every number into the spec's Findings, including the ones that came out badly.
- [ ] **Step 3: Docs:** README (behaviour table gains the swipe; "How it works" gains rate control and the flick; tuning table gains the new fields; Known limits updated), spec Findings, the project memory's agenda.
- [ ] **Step 4: Review:** dispatch a read-only review agent over `de41095..HEAD` with the same brief as before (forbid rebuilding the library or HUD while the app may run); fix findings test-first; `make test` -> 0; push. Finish with the finishing-a-development-branch skill. Do not push `main`.

## Self-review notes

- Spec coverage: rate control -> Task 1; jitter and frame rate -> Tasks 1 and 2; the flick (pose, motion, refractory, output) -> Tasks 3 and 4; backend -> Task 5; HUD, Python, replay, verdict, ABI -> Task 6; testing and the negative corpus -> Tasks 4 and 7; the open review item (summary size guard) -> Task 6.
- Placeholders: Task 1's `k_for_offset` sketch contains a line marked as a placeholder and a note to write `k_for_offset_from`; the implementer writes the real helper before the tests, as the step says. Task 1's `a_fist_still_rolls_back` and Task 2's third test say which existing test to copy. Everything else is written out.
- Type consistency: `gv_window_ops` gains `minimize` in Task 5 and the fake in `test_step.c` grows with it; `GV_KIND_MINIMIZE` is defined in Task 4 and used in Tasks 5 and 6; `flicks` is added to `gv_replay_summary` in Task 6 and `replay.c` counts it there. `scale_max_slew` disappears in Task 1 everywhere it is named (defaults, validation, `pinned()`, bridge).
