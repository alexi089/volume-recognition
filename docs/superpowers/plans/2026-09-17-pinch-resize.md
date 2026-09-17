# Pinch to Resize Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Thumb and index out, other fingers curled: spreading the fingertips grows the frontmost application window around its centre, pinching shrinks it, phone-style (scale = gap now / gap at arming).

**Architecture:** The C core becomes one recogniser with gestures as definitions (`can_arm`, `can_stay`, `on_armed`, `on_frame`); volume is moved into that shape first with no behaviour change, then resize is added. The action goes through an injectable window backend (macOS Accessibility API, plain C) beside the existing volume backend. Python stays a pipe.

**Tech Stack:** C11 (clang, make, ASan + UBSan), CoreGraphics + ApplicationServices (Accessibility), Python 3.12 + ctypes + MediaPipe 0.10.35 (CPU delegate), Swift/AppKit HUD.

**Spec:** `docs/superpowers/specs/2026-09-17-pinch-resize-design.md`

## Global Constraints

- No gesture or window logic in Python. Python moves data and draws.
- Test first, watch it fail, then implement. New tests are mutation-checked: `sed` a mutant of the code under test into a scratch file, compile the test against it, confirm at least one check fails.
- C builds with `-std=c11 -Wall -Wextra -Werror -Wpedantic`; tests with `-fsanitize=address,undefined`.
- Constants come from measurement, not guesses. A number that is a proxy is called one.
- Comments in the C explain each idiom the first time it appears; a false explanatory comment is a defect.
- The tests never touch a real window, the real volume, or the camera. Backends are faked.
- Never rebuild `core/build/libgesture.dylib` or `hud/build/volume-hud` while the app is running. `make -C core test` and `make -C hud test` are always safe.
- Run the suite as `make test > log 2>&1; echo $?`. Never pipe it into `head`.
- MediaPipe stays pinned at 0.10.35 on the CPU delegate; every MediaPipe call goes through `detect()` / `detect_photo()` in `app/main.py`.
- Changing a struct that crosses the FFI (`gv_frame`, `gv_config`, `gv_output`, `gv_step_result`) means mirroring it in `app/bridge.py` in the same commit. The ABI check compares sizes only; verify field order by eye.
- Commit messages: plain text, what changed and why, no emoji, no session links, no author name, no mention of local tooling directories. End with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- Docs live under a git-ignored directory but are tracked: commit them with `git add -f docs`.
- Do not commit new recordings. Frames needed as test fixtures are copied into `core/tests/fixtures.h` by `core/tools/gen_fixtures.py`, which is committed; the recording they came from is not.
- Do not start any recording without the owner confirming he is at the camera.
- Out of scope: the four-finger swipe, separate width/height resizing, moving windows, choosing among windows, outlining the target.

## File Structure

| File | Responsibility |
|---|---|
| `core/src/gesture_internal.h` (new) | `struct gv_ctx`, `gv_gesture` definition type, shared helpers' prototypes. Private to `core/src`. |
| `core/src/gesture.c` (modify) | State machine, shared gate / timing / release, config, arbitration. No gesture-specific maths. |
| `core/src/gestures/volume.c` (new) | The volume gesture: axis lock, accumulator, edge push, slew, dead-band. Moved, not rewritten. |
| `core/src/gestures/resize.c` (new) | The resize gesture: gap measurement, scale, log-space slew, dead-band. |
| `core/src/pose.{h,c}` (modify) | Thumb measurement and the resize arm / stay poses. |
| `core/include/gesture.h` (modify) | `gv_kind`, `scale` and `kind` in `gv_output`, new config fields. |
| `core/include/window.h`, `core/src/window_math.c`, `core/src/window_macos.c` (new) | Window backend: pure geometry, then Accessibility calls. |
| `core/tools/win.c` (new) | CLI: `win get`, `win scale <f>`; exercises the backend the way `vol` exercises CoreAudio. |
| `core/include/step.h`, `core/src/step.c`, `core/src/step_system.c` (modify) | Backend table `{volume, window}`; acquire / apply / release. |
| `core/tests/test_resize.c`, `core/tests/test_window_math.c` (new); `test_pose.c`, `test_step.c` (modify) | Tests. |
| `core/tools/measure_thumb.py`, `app/record_pinch_probe.sh` (new) | Task 1's measurement. |
| `app/bridge.py`, `app/main.py`, `app/test_bridge.py` (modify) | Struct mirrors, overlay text, HUD message. |
| `hud/Sources/HudMessage.swift`, `HudContentView.swift`, `AppController.swift`, `Tests/test_message.swift` (modify) | `gesture` + `scale` fields, resize glyph and percentage. |

---

### Task 1: Measure the thumb (needs the owner at the camera once, ~45 s)

**Files:**
- Create: `app/record_pinch_probe.sh`, `core/tools/measure_thumb.py`
- Output (not committed): `traces/probe_pinch.csv`; (committed) the report pasted into the spec under a new "Thumb measurements" heading.

**Interfaces:**
- Produces: four numbers used by Task 4: `THUMB_OUT_EXTEND`, `THUMB_OUT_CURL` (thumb tip to index knuckle, over hand size) and `GAP_MIN_ARM` (smallest fingertip gap, in hand-lengths, seen in the open pinch-ready pose), plus the list of 1-based line numbers of fixture frames: `pinch_open`, `pinch_closed`, `pointing_thumb_in`.

- [ ] **Step 1: Write the guided recorder**

`app/record_pinch_probe.sh` (executable). It reuses `main.py`'s guided mode; four segments in one file would hide which segment a frame belongs to, so it records four short clips:

```bash
#!/usr/bin/env bash
# record_pinch_probe.sh - four 10 s clips that measure the thumb for the resize pose.
# Run from the repo root, with the owner at the camera. Not part of `make test`.
set -uo pipefail
cd "$(dirname "$0")/.."
while pgrep -x volume-hud >/dev/null || ps -Ao args= | grep -q "^.venv/bin/python app/main.py"; do
    echo "another copy of the app is running; quit it first..."; sleep 3
done
make -s setup core || exit 1
CLIPS=(
"probe_pinch_open|HOLD the resize pose: thumb and index OUT and apart, other three fingers curled. Keep it up, move it around a little."
"probe_pinch_move|Same pose: slowly PINCH the two fingertips together, then SPREAD them wide. Repeat."
"probe_volume_pose|HOLD the volume pose: index and middle up, thumb wherever it naturally sits."
"probe_pointing|POINT at the screen with your index finger, thumb tucked in, other fingers curled."
)
for clip in "${CLIPS[@]}"; do
    IFS='|' read -r name prompt <<< "$clip"
    .venv/bin/python app/main.py --no-hud --record "traces/$name.csv" --duration 10 --lead-in 6 --prompt "$prompt" \
        2> >(grep -v -E '^(W0000|I0000|INFO:|WARNING: All log|.*clearcut|=== Source|wireless/android)' >&2) || exit $?
done
.venv/bin/python core/tools/measure_thumb.py
```

- [ ] **Step 2: Write the measurement tool**

`core/tools/measure_thumb.py`: for each of the four clips, over frames with a hand, print the 5th / 50th / 95th percentile of: `thumb_out = dist(lm4, lm5) / hand_scale`; `thumb_reach = dist(lm4, lm0) / dist(lm2, lm0)`; `thumb_straight = dist(lm1, lm4) / (|lm1-lm2| + |lm2-lm3| + |lm3-lm4|)`; `gap = dist(lm4, lm8) / hand_scale`; and the existing reach / straightness of index, middle, ring, pinky. Distances use `hypot(dx * aspect, dy)` with the aspect from the file's `# aspect=` line, exactly as `core/src/pose.c` does. Then print, for `probe_pinch_open`, the 1-based line number of the frame whose `thumb_out` is closest to that clip's median (fixture `pinch_open`); for `probe_pinch_move`, the line of the smallest `gap` (`pinch_closed`); for `probe_pointing`, the median-`thumb_out` frame (`pointing_thumb_in`). Reuse `load_trace` from `core/tools/sweep.py` (`sys.path.insert(0, str(ROOT / "core" / "tools"))`).

- [ ] **Step 3: Ask the owner whether he is at the camera; only then run `app/record_pinch_probe.sh`**

Expected: four `traces/probe_*.csv` files with a hand in most frames, then the report. If a clip shows a hand in under half its frames, discard and re-record it.

- [ ] **Step 4: Choose the thresholds from the report, by rule**

- `THUMB_OUT_EXTEND` = midpoint between the 95th percentile of `thumb_out` in `probe_pointing` and `probe_volume_pose` (whichever is larger) and the 5th percentile in `probe_pinch_open`. If those two overlap, `thumb_out` does not separate the poses: stop, report it, and evaluate `thumb_reach` and `thumb_straight` the same way before going on.
- `THUMB_OUT_CURL` = the 95th percentile of `thumb_out` in `probe_pointing` (below this the thumb is confidently in).
- `GAP_MIN_ARM` = half the 5th percentile of `gap` in `probe_pinch_open`, and not below 0.15.

- [ ] **Step 5: Record the numbers**

Paste the report and the three chosen values, with the rule that produced each, into the spec under "Thumb measurements". Commit the two tools and the spec (`git add -f docs`), not the clips.

---

### Task 2: Move volume into a gesture definition (pure refactor)

**Files:**
- Create: `core/src/gesture_internal.h`, `core/src/gestures/volume.c`
- Modify: `core/src/gesture.c`, `core/Makefile` (`CORE_SRC += src/gestures/volume.c`; `HDRS` already globs `src/*.h`)
- Test: every existing suite, unedited.

**Interfaces:**
- Produces (in `gesture_internal.h`):

```c
struct gv_ctx { /* the existing fields, moved here verbatim, plus: */ const struct gv_gesture *active; };

typedef struct gv_gesture {
    int  (*can_arm)(const gv_frame *f, const gv_config *cfg);
    int  (*can_stay)(const gv_frame *f, const gv_config *cfg);
    void (*on_armed)(gv_ctx *ctx, const gv_frame *f, float x, float y, double t);
    /* One believed frame while armed. x, y: filtered palm centre. since_t: time of the
     * previous believed frame. Returns 1 if the reported value changed. */
    int  (*on_frame)(gv_ctx *ctx, const gv_frame *f, float x, float y, double t, double since_t);
    void (*on_idle)(gv_ctx *ctx);   /* reset this gesture's own state */
} gv_gesture;

extern const gv_gesture gv_gesture_volume;
float gv_clamp01(float v);          /* was static clamp01 in gesture.c */
#define GV_SLEW_MAX_CREDIT_SEC 0.1  /* was SLEW_MAX_CREDIT_SEC */
```

- [ ] **Step 1: Freeze the behaviour**

Run `make -C core test > /tmp/before.log 2>&1; echo $?` and `for t in traces/synthetic_*.csv traces/real_first_session.csv; do core/build/replay $t stats; done > /tmp/before_stats.txt`. Expected: exit 0. These two files are the definition of "unchanged".

- [ ] **Step 2: Create `gesture_internal.h`** with the block above; move `struct gv_ctx` out of `gesture.c` into it unchanged and add the `active` pointer.

- [ ] **Step 3: Create `gestures/volume.c`**

Move, without editing their bodies: `choose_axis`, `edge_push`, `gain_at`, and from `gv_update` the whole `else if (ctx->state == GV_ARMED)` body from the line computing `right` to the end of the dead-band block. They become:

```c
static int volume_can_arm(const gv_frame *f, const gv_config *c)  { return gv_pose_can_arm(f, c->aspect, &c->pose); }
static int volume_can_stay(const gv_frame *f, const gv_config *c) { return gv_pose_can_stay(f, c->aspect, &c->pose); }
static void volume_on_armed(gv_ctx *ctx, const gv_frame *f, float x, float y, double t)
{ (void)f; (void)x; (void)y; ctx->target = ctx->system_volume; ctx->slew_t = t; }
static int volume_on_frame(gv_ctx *ctx, const gv_frame *f, float x, float y, double t, double since_t)
{ /* the moved block; `changed` becomes the return value */ }
static void volume_on_idle(gv_ctx *ctx) { ctx->axis = GV_AXIS_NONE; }
const gv_gesture gv_gesture_volume = { volume_can_arm, volume_can_stay, volume_on_armed, volume_on_frame, volume_on_idle };
```

- [ ] **Step 4: Make `gesture.c` generic**

In `gv_update`: a file-scope `static const gv_gesture *const k_gestures[] = { &gv_gesture_volume };`. "Can this frame arm" becomes: the first `g` in `k_gestures` with `g->can_arm(frame, cfg)`; `start_arming` stores it in `ctx->active`. While ARMING, a good frame is `ctx->active->can_arm`; while ARMED, `ctx->active->can_stay` plus `frame_is_plausible`. At the arming moment keep the shared assignments (`anchor_x/y`, `anchor_scale`, `believed_*`) and call `ctx->active->on_armed`. While ARMED update `believed_*` as now, then `changed = ctx->active->on_frame(...)`. `go_idle` calls `ctx->active->on_idle` when `active` is set, then clears it.

- [ ] **Step 5: Prove nothing changed**

Run the two commands of Step 1 into `/tmp/after.log` and `/tmp/after_stats.txt`. Expected: `diff /tmp/before_stats.txt /tmp/after_stats.txt` prints nothing; every suite reports the same check counts, 0 failed; `git diff --stat core/tests` is empty.

- [ ] **Step 6: Commit** `refactor(core): volume becomes a gesture definition; the state machine goes generic`.

---

### Task 3: `kind` and `scale` in the output

**Files:**
- Modify: `core/include/gesture.h`, `core/src/gesture.c`, `core/src/step.c`, `core/tools/replay.c` (struct initialisers), `app/bridge.py`, `app/test_bridge.py`
- Test: `core/tests/test_gesture.c`

**Interfaces:**
- Produces:

```c
typedef enum { GV_KIND_NONE = 0, GV_KIND_VOLUME = 1, GV_KIND_RESIZE = 2 } gv_kind;
typedef struct { gv_state state; float volume; int changed; gv_axis axis;
                 gv_kind kind;   /* which gesture is arming or armed; NONE when idle */
                 float scale;    /* resize only: size relative to the moment of arming; 1.0 otherwise */
} gv_output;
```
New fields go at the END so existing offsets do not move. `bridge.Output._fields_` gains `("kind", C.c_int), ("scale", C.c_float)`; `bridge.KIND_NAMES = {0: None, 1: "volume", 2: "resize"}`.

- [ ] **Step 1: Failing test** in `test_gesture.c`:

```c
static void the_output_says_which_gesture_is_active(void)
{
    session s = start(0.50f);
    hold(&s, NULL, 0.0f, 0.2);
    CHECK(s.out.kind == GV_KIND_NONE);
    CHECK_NEAR(s.out.scale, 1.0f, 1e-6);
    hold(&s, &fixture_victory, 0.0f, 0.1);
    CHECK(s.out.state == GV_ARMING && s.out.kind == GV_KIND_VOLUME);
    arm(&s, &fixture_victory, 0.0f);
    CHECK(s.out.kind == GV_KIND_VOLUME);
    hold(&s, &fixture_thumbs_up, 0.0f, 0.4);
    CHECK(s.out.state == GV_IDLE && s.out.kind == GV_KIND_NONE);
    finish(&s);
}
```
- [ ] **Step 2:** `make -C core build/test_gesture && core/build/test_gesture`. Expected: compile error (no member `kind`); add the fields with `kind` always NONE and `scale` 0 to get a run that FAILS on the kind and scale checks.
- [ ] **Step 3:** `gv_gesture` gains `gv_kind kind;` as its first member; `gv_gesture_volume` sets `GV_KIND_VOLUME`; `gv_update` fills `out.kind = ctx->active ? ctx->active->kind : GV_KIND_NONE` and `out.scale = 1.0f`. Update every `gv_output` initialiser (`grep -n "gv_output .* = {" core`).
- [ ] **Step 4:** Mirror in `bridge.py`; in `app/test_bridge.py` add `check(out.kind == 1, "a volume slide reports kind volume")` after the horizontal slide. With the app not running: `make test > log 2>&1; echo $?` -> 0.
- [ ] **Step 5: Commit** `feat(core): report which gesture is active, and a scale, in the output`.

---

### Task 4: The thumb and the resize poses

**Files:**
- Modify: `core/src/pose.h`, `core/src/pose.c`, `core/include/gesture.h` (`gv_pose_limits`), `core/src/gesture.c` (defaults, validation), `app/bridge.py` (`PoseLimits`), `core/tools/gen_fixtures.py`, `core/tests/fixtures.h` (regenerated)
- Test: `core/tests/test_pose.c`

**Interfaces:**
- Consumes: Task 1's `THUMB_OUT_EXTEND`, `THUMB_OUT_CURL`, `GAP_MIN_ARM`, fixture line numbers.
- Produces:

```c
/* gv_pose_limits gains, at the end: */
float thumb_out_extend;  /* thumb is OUT above this (tip to index knuckle, over hand size) */
float thumb_out_curl;    /* thumb is IN below this */
float pinch_gap_min;     /* smallest thumb-index gap, in hand-lengths, that may arm a resize */

float gv_thumb_out(const gv_frame *f, float aspect);
float gv_pinch_gap(const gv_frame *f, float aspect);   /* dist(thumb tip, index tip) / hand scale */
int   gv_pose_can_arm_resize(const gv_frame *f, float aspect, const gv_pose_limits *lim);
int   gv_pose_can_stay_resize(const gv_frame *f, float aspect, const gv_pose_limits *lim);
```
`can_arm_resize`: present AND thumb out AND index extended AND middle, ring, pinky NOT extended AND `gv_pinch_gap >= pinch_gap_min`. `can_stay_resize`: present AND thumb not in AND middle NOT extended.

**Amended after Task 1 (see the spec's "Thumb measurements"):** the thumb is judged like a finger, on reach AND straightness, not on tip-to-index-knuckle distance, which turned out larger with the thumb tucked in than out. The limits are therefore `thumb_extend_reach` 1.05, `thumb_extend_straight` 0.67, `thumb_curl_reach` 0.55, `thumb_curl_straight` 0.45 (replacing `thumb_out_extend` / `thumb_out_curl`), plus `pinch_gap_min` 0.73; the functions are `gv_thumb_reach`, `gv_thumb_straightness`, `gv_thumb_is_out`, `gv_thumb_is_in`. The index finger is NOT part of the stay pose: pinching curls it (reach 0.57). Fixture frames: `pinch_open` = probe_pinch_open line 240, `pinch_closed` = probe_pinch_move line 23, `pointing_thumb_in` = probe_pointing line 211.

- [ ] **Step 1:** Add to `gen_fixtures.py` a second source dict `PROBE_FRAMES = {"pinch_open": ("probe_pinch_open", N1), "pinch_closed": ("probe_pinch_move", N2), "pointing_thumb_in": ("probe_pointing", N3)}` with Task 1's line numbers, emitted as `fixture_real_pinch_open` etc. with the probe's aspect. Regenerate `fixtures.h`. (The clips themselves stay uncommitted; `fixtures.h` is the committed copy.)
- [ ] **Step 2: Failing tests** in `test_pose.c` (limits literal uses Task 1's numbers):

```c
static void the_pinch_ready_hand_can_arm_a_resize(void)
{
    CHECK(gv_pose_can_arm_resize(&fixture_real_pinch_open, REAL, &LIM));
    CHECK(gv_thumb_out(&fixture_real_pinch_open, REAL) > LIM.thumb_out_extend);
}
static void other_hand_shapes_cannot_arm_a_resize(void)
{
    CHECK(!gv_pose_can_arm_resize(&fixture_real_pointing_thumb_in, REAL, &LIM)); /* thumb in */
    CHECK(!gv_pose_can_arm_resize(&fixture_real_tight_two, REAL, &LIM));         /* middle up: that is volume */
    CHECK(!gv_pose_can_arm_resize(&fixture_real_open_palm, REAL, &LIM));
    CHECK(!gv_pose_can_arm_resize(&fixture_real_fist, REAL, &LIM));
    CHECK(!gv_pose_can_arm_resize(&fixture_real_pinch_closed, REAL, &LIM));      /* gap too small to start from */
}
static void no_hand_shape_arms_two_gestures(void)
{
    const gv_frame *all[] = { &fixture_victory, &fixture_pointing_up, &fixture_thumbs_up, &fixture_real_relaxed_two,
        &fixture_real_tight_two, &fixture_real_fist, &fixture_real_open_palm, &fixture_real_pinch_open,
        &fixture_real_pinch_closed, &fixture_real_pointing_thumb_in };
    const float asp[] = { 1, 1, 1, REAL, REAL, REAL, REAL, REAL, REAL, REAL };
    for (int i = 0; i < 10; i++)
        CHECK(!(gv_pose_can_arm(all[i], asp[i], &LIM) && gv_pose_can_arm_resize(all[i], asp[i], &LIM)));
}
static void a_pinched_hand_can_stay_but_a_fist_cannot(void)
{
    CHECK(gv_pose_can_stay_resize(&fixture_real_pinch_closed, REAL, &LIM));
    CHECK(!gv_pose_can_stay_resize(&fixture_real_fist, REAL, &LIM));
}
static void pinch_gap_is_size_and_rotation_free(void)
{
    float g = gv_pinch_gap(&fixture_real_pinch_open, REAL);
    gv_frame small = scaled(&fixture_real_pinch_open, 0.5f), turned = rotated(&fixture_real_pinch_open, REAL, 60.0f);
    CHECK_NEAR(gv_pinch_gap(&small, REAL), g, 0.01);
    CHECK_NEAR(gv_pinch_gap(&turned, REAL), g, 0.01);
}
```
- [ ] **Step 3:** Stub the four functions (return 0); run `core/build/test_pose`; expected: the positive checks FAIL, the negative ones pass vacuously.
- [ ] **Step 4:** Implement with the existing `dist()` and `TINY` guard; `gv_thumb_out = dist(lm[GV_THUMB_TIP], lm[GV_INDEX_MCP]) / gv_hand_scale`, 0 when the scale is below `TINY`. Add the three limits to `gv_default_config` and to `config_is_valid` (`thumb_out_curl > 0`, `thumb_out_curl <= thumb_out_extend`, `pinch_gap_min >= 0`, all finite). Mirror in `bridge.PoseLimits`.
- [ ] **Step 5:** Tests pass. Mutation-check: drop the middle-finger term from `can_arm_resize`; drop the gap term; make `can_stay_resize` ignore the thumb. Each must fail at least one check; add a fixture-based test for any that survives.
- [ ] **Step 6: Commit** `feat(core): measure the thumb; arm and stay poses for resize`.

---

### Task 5: The resize gesture

**Files:**
- Create: `core/src/gestures/resize.c`, `core/tests/test_resize.c`
- Modify: `core/src/gesture_internal.h` (ctx fields), `core/src/gesture.c` (`k_gestures`, idle reset, output scale), `core/include/gesture.h` (config), `app/bridge.py`, `core/Makefile` (`CORE_SRC`, `TESTS += test_resize`, its rule)

**Interfaces:**
- Produces:

```c
/* gv_config gains, at the end: */
float scale_min, scale_max;   /* 0.25, 4.0 */
float scale_dead_band;        /* 0.01: smallest relative change worth reporting */
float scale_max_slew;         /* 1.0: octaves per second (2x per second) */
/* gv_ctx gains: */ gv_filter gap_filter; float gap_at_arm; float scale_target; float reported_scale;
extern const gv_gesture gv_gesture_resize;   /* kind GV_KIND_RESIZE */
```
`k_gestures` becomes `{ &gv_gesture_volume, &gv_gesture_resize }` (order is irrelevant: Task 4 proves the arm poses are disjoint). `gv_update` reports `out.scale = ctx->reported_scale` when the active kind is resize, else 1.0.

- [x] **Step 1: Failing tests** `test_resize.c`. It copies `session`, `frame_xy`, `hold`, `arm`, `aligned_to` from `test_gesture.c` (a test file stands alone) and adds a helper that sets the fingertip gap of the pinch-ready fixture by moving the thumb tip along the thumb-tip to index-tip line:

```c
static gv_frame with_gap(const gv_frame *src, float aspect, float factor)   /* 1.0 = as recorded */
{
    gv_frame out = *src;
    out.lm[GV_THUMB_TIP].x = src->lm[GV_INDEX_TIP].x + (src->lm[GV_THUMB_TIP].x - src->lm[GV_INDEX_TIP].x) * factor;
    out.lm[GV_THUMB_TIP].y = src->lm[GV_INDEX_TIP].y + (src->lm[GV_THUMB_TIP].y - src->lm[GV_INDEX_TIP].y) * factor;
    (void)aspect;
    return out;
}
```
Tests (each `start_real`, fixture `fixture_real_pinch_open`, 30 fps):
  1. `arming_reports_resize_and_scale_one`: 0.5 s hold -> ARMED, `kind == GV_KIND_RESIZE`, `scale` within 1e-3 of 1.0, `changes == 0`.
  2. `spreading_grows_pinching_shrinks`: ramp the gap factor 1.0 -> 1.5 over 1 s, hold 0.8 s -> `scale` within 0.04 of 1.5; ramp to 0.6, hold -> within 0.03 of 0.6.
  3. `scale_is_clamped`: gap factor 8 for 4 s -> exactly `scale_max`; factor 0.05 is NOT used (it would leave the stay pose).
  4. `scale_cannot_jump`: gap factor 1.0 -> 3.0 in ONE frame; the next output's `scale <= 2^(scale_max_slew * 0.1) + 1e-3`; after 3 s it reaches 3.0 within 0.06.
  5. `a_gap_in_tracking_does_not_bank_a_jump`: 0.2 s of NULL frames, then factor 2.0: first step bounded as in 4.
  6. `closing_the_hand_releases_and_keeps_the_scale`: grow to 1.4, then `aligned_to(&fixture_real_fist, ...)` for 0.4 s -> IDLE, `changes` unchanged during the release, last armed `scale` within 0.04 of 1.4.
  7. `a_pinched_start_does_not_arm`: `fixture_real_pinch_closed` for 1 s -> never ARMED.
  8. `volume_and_resize_do_not_cross`: arm volume with `fixture_real_tight_two`, then show the pinch-ready hand aligned to it for 1 s -> stays `GV_KIND_VOLUME` until release; and the reverse.
  9. `a_wobbling_gap_does_not_chatter`: +/-0.4% gap wobble for 2 s -> at most 1 change.
- [x] **Step 2:** Stub `gv_gesture_resize` (`can_arm` returns 0); run; expected: tests 1-6, 8 FAIL.
- [x] **Step 3: Implement.** `on_armed`: reset `gap_filter`, `gap_at_arm = gv_filter_apply(&gap_filter, gv_pinch_gap(f, aspect), t)`, `scale_target = reported_scale = 1.0f`. `on_frame`: `gap = filter(gv_pinch_gap)`; `target = clamp(gap / gap_at_arm, scale_min, scale_max)`; slew in log space: `allow = scale_max_slew * min(t - slew_t, GV_SLEW_MAX_CREDIT_SEC)` octaves, `want = log2f(target / reported_scale)`, clamp `want` to `[-allow, allow]`, `target = reported_scale * exp2f(want)`; `slew_t = t`; report when `fabsf(target / reported_scale - 1.0f) >= scale_dead_band` or when `target` is exactly `scale_min` / `scale_max` and differs from `reported_scale` (the same end-of-range bypass volume has, for the same reason). `gap_at_arm` is guaranteed `>= pinch_gap_min > 0` by the arm pose; assert it with a guard that refuses to arm (`on_armed` sets a flag that `gv_update` turns into `go_idle`) if the filtered value is below `TINY`.
- [x] **Step 4:** All tests pass; `make -C core test` all green. Mutation-check: remove the log-space slew; remove the credit cap; invert the ratio; remove the end-of-range bypass.
- [x] **Step 5:** Mirror the four config fields in `bridge.Config`. **Commit** `feat(core): the resize gesture: fingertip gap to a rate-limited scale`.


**As built (2026-09-17). The steps above are the plan as approved; these are the departures, each forced by a measurement. The spec's "What building it changed" has the numbers.**

- The `with_gap` helper was dropped: moving only the thumb tip bends the thumb into a different pose. The tests blend between two real frames instead (`core/tests/pinch_motion.h`: `aligned_to`, `pinch(k)`), shared with `test_step.c`.
- Test 6 as written could not pass: the recorded fist keeps the thumb "out" and is nearly the same shape as a full pinch, so it did not release and shrank the window to 0.51x. Two changes follow. `gv_pose_can_stay_resize` requires the index not curled. `gv_gesture` gains an optional `on_release(ctx, pose_ended)`; resize uses it to end on the scale from `release_rollback_sec` (0.3, a new `gv_config` field, last) before its last good frame, reported as one final `changed` frame with `kind` RESIZE while `state` is already IDLE. `gv_ctx` gains a 32-entry ring buffer of reported scales, `last_hand_t` and `last_hand_before_t`. Consumers (Task 8's window backend) must apply a `changed` RESIZE frame even when the state is IDLE, and release the window after it.
- `fixture_pointing_up` armed the resize (caught by `other_hand_shapes_moving_across_the_frame_do_nothing`). `gv_thumb_clearance` and `gv_pose_limits.thumb_clear_min` (0.62, last pose field) were added to the ARM pose only.
- `pinch_gap_min` must be positive (validated), which is what makes the division by `gap_at_arm` safe; the flag-and-go-idle guard in Step 3 was not needed.
- `gv_step` writes the volume only for `GV_KIND_VOLUME` changes (test: `a_resize_never_writes_the_volume`). This was pulled forward from Task 8 because without it a resize would have written the volume as soon as it existed.
- `bridge.Config` mirrors five fields, not four, and `bridge.PoseLimits` one more; `test_bridge.py` pins the last field of each by value.
- Mutation check: 25 mutants over the slew, clamps, dead-band, rollback, release plumbing, stay pose and clearance; 7 survived the first run (upward slew, log versus linear, rollback depth, history reset, ring and pinky in the clearance, two validations), tests were added, all 25 are killed.
- Known and not fixed here: on the blind-recorded pinch clip every pinch is a 0.2 s snap to touching, which this design treats as closing the hand (net change nil). Whether a watched pinch is slow enough is for Task 10's live check.
---

### Task 6: Window geometry (pure)

**Files:**
- Create: `core/include/window.h`, `core/src/window_math.c`, `core/tests/test_window_math.c`; Makefile rule.

**Interfaces:**
- Produces:

```c
typedef struct { double x, y, w, h; } win_rect;   /* top-left origin, points: the Accessibility API's space */
/* The frame a window should have at `scale`, given the frame it had when the gesture armed and the
 * visible frame of its screen. Centre kept where possible; size clamped to [min_w x min_h, visible];
 * then moved, not shrunk, to lie inside `visible`. */
win_rect win_scaled_frame(win_rect original, double scale, win_rect visible, double min_w, double min_h);
```

- [x] **Step 1: Failing tests:** scale 1 -> identical; scale 1.5 on 800x600 centred at (1000, 700) in a 2560x1440 visible frame -> 1200x900 with the same centre; scale 0.1 -> 320x240, same centre; scale 4 -> clamped to the visible size, origin at the visible origin; a window near the right edge growing -> width as asked, shifted left to fit, top unchanged unless it also overflows; NaN or non-positive scale -> the original frame unchanged; a visible frame with a negative origin (a display above or left of the primary: the owner's second display sits at y = -611) -> still correct.
- [x] **Step 2:** stub returns `original`; run; expected FAIL on all but the first and the NaN case.
- [x] **Step 3:** implement (about 25 lines; `fmin` / `fmax`; centre = origin + size / 2).
- [x] **Step 4:** pass; mutation-check the clamp order (clamp size before computing the origin).
- [x] **Step 5: Commit** `feat(core): window geometry for centred, on-screen resizing`.


**As built (2026-09-17):** as planned, plus three cases the list above lacked: each side has its own floor, a screen smaller than the minimum wins over the minimum, and a sweep asserting every result lies on the screen. 158 checks; 14 mutants (clamp order, clamp precedence, each slide, each floor and ceiling, the scale guard), all killed.
---

### Task 7: The Accessibility backend and its CLI

**Files:**
- Create: `core/src/window_macos.c`, `core/tools/win.c`; modify `core/include/window.h`, `core/Makefile` (`AX := -framework ApplicationServices`; `build/win`; add `window_math.c window_macos.c` to `LIB_SRC`).

**Interfaces:**
- Produces:

```c
typedef enum { WIN_OK = 0, WIN_ERR_NOT_TRUSTED = -1, WIN_ERR_NO_WINDOW = -2, WIN_ERR_NOT_RESIZABLE = -3,
               WIN_ERR_IGNORED = -4, WIN_ERR_AX = -5, WIN_ERR_BAD_VALUE = -6 } win_status;
win_status  win_acquire(void);                          /* frontmost layer-0 window not owned by this process */
win_status  win_apply(float scale);                     /* relative to the frame at acquire */
void        win_release(void);
win_status  win_target_name(char *buf, size_t len);     /* owning app's name, for the overlay and HUD */
const char *win_strerror(win_status s);
int         win_last_ax_error(void);
```
Verified route (spec, "Feasibility"): `CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID)`; first entry with `kCGWindowLayer == 0` and `kCGWindowOwnerPID != getpid()`; `AXUIElementCreateApplication(pid)`; `kAXFocusedWindowAttribute`, falling back to `kAXMainWindowAttribute`. Do NOT use the system-wide element's `kAXFocusedApplicationAttribute`: it returns -25204 here.

- [x] **Step 1:** `win_acquire`: `AXIsProcessTrusted()` false -> `WIN_ERR_NOT_TRUSTED`; read size and position (`AXValueGetValue`, `kAXValueCGSizeType` / `kAXValueCGPointType`); `AXUIElementIsAttributeSettable(kAXSizeAttribute)` false -> `WIN_ERR_NOT_RESIZABLE`; find the visible frame of the display containing the window's centre with `CGGetDisplaysWithPoint` + `CGDisplayBounds` (menu-bar height is not available from CoreGraphics in C: subtract 25 pt from the top of the MAIN display only, and say so in a comment); retain the window element.
- [x] **Step 2:** `win_apply`: `win_scaled_frame`; write position then size (shrinking: size then position, so the window never pokes off-screen mid-step); read the size back; differs by more than 2 pt from what was asked AND equals the previous size -> `WIN_ERR_IGNORED`; differs otherwise (the app enforces its own minimum or aspect) -> `WIN_OK`, and the comment says why that is not an error.
- [x] **Step 3:** `win.c`: `win get` prints the target app and frame; `win scale 0.9` acquires, applies, sleeps 1 s, applies 1.0, releases, printing each read-back. Build with the project flags (no sanitizer needed; it links frameworks).
- [x] **Step 4: Verify by hand, with the owner told first:** `core/build/win get`, then `core/build/win scale 0.9` on whatever window is in front; expected: it shrinks around its centre and returns exactly. Try a fixed-size window (System Settings) -> `WIN_ERR_NOT_RESIZABLE`. Measure 60 consecutive `win_apply` calls with `clock_gettime`; record the per-call time in the spec (the "rate of writes is unmeasured" risk).
- [x] **Step 5: Commit** `feat(core): resize the front window through the Accessibility API`.


**As built (2026-09-17):**
- The visible frame does not subtract a fixed 25 pt. The menu bar's height is read from the window list (the window at `kCGMainMenuWindowLevelKey` lying along the top of the display): 37 pt on the built-in display, 24 pt on the ultrawide, one per display. The arithmetic is `win_below_menu_bar` in `window_math.c`, tested (202 checks in `test_window_math`, 7 more mutants killed). The Dock is not subtracted; its window spans the display.
- Added `win_current_frame` (the CLI and, later, the overlay need the read-back) and `WIN_MIN_W` / `WIN_MIN_H`. AX messaging timeout 0.25 s on the app and window elements.
- Write order is chosen by area (growing: position then size; shrinking: size then position).
- `win bench` was added to the CLI for the timing: 13.8 ms mean, 23.8 ms worst per `win_apply`.
- Step 4, done: `win get`, `win scale 0.9` (Terminal, snapped to 1187x623, restored exactly), `win bench` (Chrome, restored exactly), `leaks`: 0. Step 4, NOT done: the fixed-size window check, because it means pulling System Settings in front of whatever the owner is doing. To run it: open System Settings, then `sleep 3; core/build/win get` from a terminal and click System Settings within the three seconds; expected `the front window has a fixed size`.
---

### Task 8: Glue: a table of backends

**Files:**
- Modify: `core/include/step.h`, `core/src/step.c`, `core/src/step_system.c`, `core/tests/test_step.c`

**Interfaces:**
- Produces:

```c
typedef struct { win_status (*acquire)(void); win_status (*apply)(float scale); void (*release)(void); } gv_window_ops;
typedef struct { gv_volume_ops volume; gv_window_ops window; } gv_backends;
extern const gv_backends gv_system_backends;
typedef struct { gv_output out; vol_status vol; win_status win; } gv_step_result;   /* win appended at the end */
gv_step_result gv_step(gv_ctx *ctx, const gv_frame *frame, const gv_backends *b);
```
`gv_step` remembers, in `gv_ctx` (a new `int window_held`), whether it holds a window target.

- [x] **Step 1: Failing tests** with a recording fake window backend (counts of acquire / apply / release, last scale, a settable status): arming a resize acquires exactly once and applies nothing; a changed scale applies that scale; release (fist) releases exactly once; a failing acquire (`WIN_ERR_NOT_RESIZABLE`) is reported in `result.win` on every armed frame and nothing is applied; a failing apply is reported on that frame; a volume slide never touches the window backend and a resize never calls `volume.set`; the existing volume tests keep passing with the fake window ops present.
- [x] **Step 2:** run; FAIL. **Step 3:** implement: on the transition into `ARMED` with `kind == RESIZE`, `acquire`; on `changed` with that kind, `apply(out.scale)`; on leaving `ARMED`, `release` if held. Volume path unchanged. **Step 4:** pass; mutation-check "release never called" and "apply called for volume".
- [x] **Step 5:** `bridge.StepResult` gains `("win", C.c_int)`; `bridge.win_strerror`, `bridge.win_target_name`. **Commit** `feat(core): per-frame glue drives a window backend as well as volume`.


**As built (2026-09-17):**
- `gv_ctx` gains three `step_` fields, not one: whether a resize is active, whether a window is held, and how the acquire went (repeated in `result.win` while armed). `gesture.c` never reads them.
- The write is NOT limited to armed frames: a resize that rolls back sends its last scale on the frame it goes idle (Task 5), and the glue writes it and only then releases. Test: `the_rolled_back_scale_is_written_before_the_window_is_let_go`; the fake counts writes to a window already let go.
- Two lines written first were removed after surviving mutation as redundant: a kind check on the window write (while a resize is active the only `changed` the core can report is that resize's) and a reset of the stored acquire status (only read while active). 12 mutants remain, all killed, including the plan's two.
- Not handled, on purpose: `gv_destroy` while a window is held leaves one retained element until the next acquire or exit.
---

### Task 9: Python, overlay, HUD

**Files:**
- Modify: `app/main.py`, `app/bridge.py`, `app/test_bridge.py`, `hud/Sources/HudMessage.swift`, `hud/Sources/HudContentView.swift`, `hud/Sources/AppController.swift`, `hud/Tests/test_message.swift`, `hud/README.md`

- [x] **Step 1:** `test_bridge.py` tests the plumbing only: `Output` has `kind` and `scale`, `StepResult` has `win`, a default-config `Gesture` reports `kind == 0` and `scale == 1.0` with no hand, and the ABI sizes match. Resize BEHAVIOUR is tested in C (Task 5); the probe clips are not committed, so Python has no pinch landmarks to feed, and exporting C fixtures through ctypes just to repeat a C test is not worth a new API.
- [x] **Step 2:** `main.py`: overlay label `RESIZE x{scale:.2f}  {target app}` when `kind == resize`; the error latch also takes `win` status text; HUD message gains `"gesture": bridge.KIND_NAMES[kind]` and `"scale": round(scale, 3)`; events file gains `kind,scale,win_status` columns.
- [x] **Step 3 (HUD, test-first in `test_message.swift`):** `gesture` optional, `"volume" | "resize"`, anything else -> volume (never rejects the line); `scale` optional finite number, clamped 0.25 - 4, default 1. For resize the pill shows `arrow.up.left.and.arrow.down.right` and `"{scale * 100, rounded}%"`, and the bar shows `log2(scale)` mapped from [-2, 2] to [0, 1] (so 1.0x is the middle). Same fade / pulse / error behaviour. Update `hud/README.md`'s contract.
- [x] **Step 4:** `make test > log 2>&1; echo $?` -> 0 (app not running). **Commit** `feat(app): show the resize gesture in the overlay and the HUD`.


**As built (2026-09-17):**
- The bridge plumbing tests of Step 1 landed with Tasks 5 and 8 (`win`, the last config fields pinned by value, an idle step).
- The banner now says WHICH thing is failing (`WINDOW NOT RESIZING` / `VOLUME NOT CHANGING`); `describe_problem` only words a status the C side produced. `Hud.send` and `draw_overlay` take the C output struct instead of a growing argument list.
- HUD: the readout arithmetic is a Foundation-only `HudReadout` (percent text, logarithmic bar level), tested with the parser: 105 checks. The glyph is rebuilt when the error flag changes as well as the symbol name, because the resize symbol has no error variant; VoiceOver hears "window size", not "volume".
- Verified without a camera: `py_compile`, the banner wording, the overlay drawing, and the JSON line `{"gesture": "resize", "scale": 1.322, ...}`. NOT verified: the running camera loop with these changes, and how the resize pill looks. Both belong to Task 10's live check.
- Coming (owner's decision, 2026-09-17): the laptop moves to macOS 26, after which the pill's material can be Apple's Liquid Glass (`NSGlassEffectView`) behind `if #available(macOS 26, *)`, keeping `NSVisualEffectView` as the fallback. That needs the macOS 26 SDK (Command Line Tools 26), not only the OS, and nothing here has been compiled against it yet.
---

### Task 10: Live check, traces, docs, review

- [x] **Step 1 (done 2026-09-17; see "As built" below):** `replay` learns `grow` / `shrink` (final `scale` above 1.05 / below 0.95, where `scale` is the last reported while armed) and prints the final scale; test the verdict function in `test_trace.c`'s style with synthetic traces from `gen_traces.py` (pinch-ready fixture with the gap ramped; added to `traces/manifest.txt`).
- [ ] **Step 2:** With the owner at the camera: `make run`; resize a real window; then a guided recording adding `real_pinch_grow`, `real_pinch_shrink` to `app/record_scripted.sh` plus the existing five negatives (which must now also produce zero RESIZE arms). Analyse with `core/tools/sweep.py` (extend its report with resize arms and final scale).
- [ ] **Step 3:** README (behaviour, pose table, tuning table, Accessibility permission instructions, reading order gains `gestures/`, `window_*`), spec "Findings" section, project memory.
- [ ] **Step 4:** Dispatch a read-only review agent over the iteration's diff with the same brief as the earlier reviews (forbid rebuilding the dylib / HUD if the app may be running); fix findings test-first; push the branch. Do not push `main`.

**As built, Step 1 (2026-09-17):**
- The rule is its own unit, `core/src/verdict.{h,c}` (`gv_expectation_parse`, `gv_verdict` over a `gv_replay_summary`), with `tests/test_verdict.c`: 29 checks, 13 mutants killed. It is stricter than the plan's one-liner: `up` / `down` also require that no window was touched, `grow` / `shrink` that the volume did not move, and `none` means no change of either kind even if the totals came back to where they started. The final scale is the last one a resize REPORTED, its rolled-back last word included, not "the last while armed".
- The synthetic pinch traces come from a C tool, `core/tools/gen_pinch_traces.c` (`make -C core pinch-traces`), not `gen_traces.py`, so the pinch blend exists once (`tests/pinch_motion.h`). Four traces in the manifest: shrink (0.72), grow (1.55), then_fist (0.82, ends by closing the hand) and glimpse (none).
- `sweep.py` no longer has its own copy of the rule: `verdict.c` is in the library and `bridge.verdict` asks it. Its report gains resize arms and the final scale.
- Every Makefile target now depends on the Makefile: adding `verdict.c` to `LIB_SRC` did not relink the library, and the bridge test failed on a missing symbol.
- Step 3 (README, memory) and Step 4 (review) are done; see below. Step 2 is half done.

**As built, Steps 2 - 4 (2026-09-17 evening):**
- Step 2, live run: done twice. The first failed on every resize: with the camera in use the Window Server keeps two 10 x 18 pt layer-0 windows in front of everything, and the backend picked one (AXError -25204). Fixed with `win_is_candidate` (layer 0, not ours, at least 100 pt a side), reproduced and confirmed with the camera open. The second run worked ("works", the owner); numbers in the spec's "First live session". From it: `win_write_was_ignored` (a window that has responded and then stops is at its own limit, not ignoring writes), and the events file gained `win_ax_error`, `win_target`, `win_w`, `win_h`.
- Step 2, NOT done: the guided recordings (`app/record_scripted.sh pinch`, then the five negatives again) and the `sweep.py` analysis. `record_scripted.sh` already has the two pinch clips.
- Step 4, review: no correctness bug in normal use. Fixed: the frame that lets go of a gesture never arms the next (only reachable with `arm_hold_sec` 0). Open: a size guard for `ReplaySummary` in `bridge._check_abi` (needs `gv_abi_replay_summary_size` in C), and the target name reading empty on the rollback frame (`win_release` clears it before `main.py` asks).
- State of the tests at the end of the day: `make test` exit 0; 758 C checks in 8 suites, 8 manifest traces, HUD 105, bridge and leak tests.
- Not yet done from Step 4: the finishing-a-development-branch skill. The branch is pushed; `main` is untouched.

**Open questions for the next session, in the order they matter:**
1. How did it FEEL? Slow pinch following the fingers, snap pinch doing nothing, closing the hand keeping the size: the owner said "works" and nothing more.
2. The probable false arm on Terminal while typing (spec, "First live session"). One event; needs the negatives re-recorded with the resize in place.
3. 16 fps while a window moves. Decide: every other frame, or a writer thread.
4. 29 of 70 sessions changed nothing. Rollback too eager, arm hold too short, or fine? Needs the guided clips (intent labels) to answer.

## Self-review notes

- Spec coverage: architecture -> Tasks 2, 3, 5; thumb rule -> 1, 4; resize gesture -> 5; window backend -> 6, 7; glue -> 8; Python / HUD -> 9; testing, replay, docs -> 10; risks (write rate, fixed-size windows, Accessibility grant, target surprise) -> Task 7 steps 1 and 4, Task 9 step 2.
- The only values not fixed in this plan are the three thumb thresholds and three fixture line numbers. That is deliberate: they are measured in Task 1 by a stated rule, and Task 4 consumes them by name.
- Task 9 step 1 deliberately tests plumbing only, and says why, so the next reader does not re-propose a ctypes fixture accessor.
