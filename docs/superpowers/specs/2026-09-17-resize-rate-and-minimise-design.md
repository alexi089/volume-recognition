# Resize by rate, and swipe down to minimise: design

Date: 2026-09-17 (late). Status: approved by the owner in conversation; not
built. Follows `2026-09-17-pinch-resize-design.md`, whose "First live session"
section is the evidence this design answers.

## Why

The first live session of pinch-to-resize (70 resizes, verdict "works") showed
three things the owner then asked to change:

1. **It is jittery.** Of 1,657 window writes, 15% reversed the direction of the
   previous write, with a median reversal step of 2.3%. Fingertip noise passes
   the 1% dead-band and is written to the window, and the camera loop drops
   from 29.9 to 16.3 fps while writing.
2. **Growing needs a pinch first,** because the size is the ratio of the gap now
   to the gap at arming: to grow, the fingers must have room to open.
3. **Fingers only open so far,** so one gesture reaches a limited range.

Owner's proposals and the decisions taken (2026-09-17, in conversation):

| Proposal | Decision |
|---|---|
| Faster, smoother resizing on the pinch | **Rate control** ("joystick"): finger offset from the arming gap sets the SPEED of the size change, not the size |
| Open the hand to grow | **Dropped**: rate control grows too; opening the hand collides with "closing the hand releases" and with the open-palm negatives |
| Swipe the window away to minimise | **Yes, an open palm moved DOWN.** Sideways swipes stay reserved for the media gestures planned next. (Built as a bare swipe; became "hold the open palm still, then drop it" after two studies - see the flick section and the Findings) |

## Rate control for the pinch

The gesture, its arm pose, stay pose, release and rollback are unchanged. Only
what a frame does while armed changes.

- `offset = gap / gap_at_arm - 1`, gap One Euro filtered as now.
- Inside the **dead zone** `|offset| < resize_dead_zone` (0.08) nothing happens.
- Beyond it, the scale changes at `rate = sign(offset) * min(resize_rate_max,
  resize_rate_gain * (|offset| - resize_dead_zone))` octaves per second
  (`resize_rate_gain` 4.5, `resize_rate_max` 2.0: about 1 octave/s at 30% offset,
  2 at 52% and beyond). Log space, as before: shrinking and growing feel the same.
- Per frame: `log2(scale) += rate * min(dt, 0.1 s)`. The credit cap across
  lost frames stays; the slew limit becomes `resize_rate_max` itself.
- The reported scale changes only when it moves by `scale_dead_band` (1%) from
  the last report, as now. `scale_min` / `scale_max` widen to 0.1x / 10x: they
  are a safety net, not the range; the window itself is clamped to its screen
  and to a minimum size by `win_scaled_frame`.

Why these numbers, and their status: the 8% dead zone sits well clear of the
measured 2.3% median jitter step and is a **proxy** until the guided clips
exist; 4.5 and 2.0 were chosen so that a comfortable finger offset gives a
window that doubles in about a second. All three are `gv_config` fields and
tunable live; the tuning rule is the usual one (no change on the negatives,
smooth positives).

What this costs: it is less direct. Under phone-style mapping the window
follows the fingers and stops when they stop; under rate control the fingers
steer, and stopping means returning to neutral (or letting go, which the
rollback protects).

## Jitter and frame rate

- The dead zone above removes twitching at rest by construction. The evidence
  will be the same measurement as in "Why", on the next live recording.
- **Window writes are throttled** in the glue (`step.c`): while a resize is
  armed, at most one write per `win_write_interval_sec` (0.06 s, about every
  other frame). A change that arrives inside the interval is not lost: it is
  written on the first frame the interval allows, whether or not that frame
  changed anything. The final word of a rollback is always written. Measured
  after, on the live loop: the target is the camera loop back near 30 fps while
  a window moves. If throttling is not enough, the fallback is a writer thread.

## Hold, then drop, to minimise: a "flick"

A flick is a new class of gesture: it **fires once** instead of arming and
running. It does hold a pose first, like the other two, but it never enters
ARMED and has no state to leave - it is judged from a short history of the
palm's motion while the machine is IDLE.

The shape changed after two studies (Findings below). It was first designed as
a swipe: one downward motion of an open palm. That could not be made to work,
because a swipe and a hand being put down are the same motion - the owner's own
swipes measured 0.78 - 1.07 hand-lengths, and so does ordinary hand-lowering.
The gesture now asks for a deliberate pause first, the way arming a volume slide
or a resize already does: **hold the open palm still, then drop it.**

- **The hold:** an open palm (`gv_pose_is_open_palm`) on every frame of a run
  lasting at least `flick_hold_sec` (0.25 s), with the palm centre moving
  slower than `flick_still_speed` (1.0 hand-lengths per second) between
  consecutive frames. A tracking hole longer than `release_grace_sec` starts a
  new run rather than continuing one: nothing is known about what the hand did
  inside it. 0.25 s is what `arm_hold_sec` already asks of the other gestures;
  on the recordings, runs under the 1.0 speed limit reach 0.9 - 1.7 s, so a
  hand that is meant to be still clears it easily.
- **The drop:** within `flick_drop_window_sec` (0.6 s) of any frame of that
  hold, the palm centre at least `flick_drop` (0.8 hand-lengths, measured in
  hand-lengths of the hand **as it was held**) below where it was held, with
  sideways travel at most `flick_max_sideways` (0.5) times the drop. No extra
  aspect factor - `gv_palm_x` already carries it and `gv_palm_y` does not need
  it.
- **At the end of the drop:** the hand must not be closed (`!gv_pose_is_fist`,
  i.e. not all four fingers curled) and no landmark may have reached past
  `1 - flick_edge_margin` (0.10) of the frame height (`gv_hand_lowest_y`;
  MediaPipe keeps reporting landmarks outside 0..1 for a hand half out of
  frame, so this is a margin inside a real picture, not a clamp).
  The two ends are asked different questions on purpose: the hold wants a
  confident open palm, the end of the drop only "not closed", because seven of
  the sixteen downward open-palm motions in the first live session had lost the
  open palm by their last sample (`open(old,new) = 1,0`) - fast motion blurs the
  fingers out of a "long AND straight" reading - and only two of those seven had
  actually closed the hand. The edge veto replaced an earlier rule requiring the
  hand to be the same size at both ends: a drop swings the hand away from the
  camera so a real one shrinks as it falls (measured 0.66, 0.68, 0.51, 0.24),
  and that rule threw real ones away. Leaving the frame is the thing itself,
  where a change of apparent size was only a symptom. Measured across the frames
  of the motion each of the two corpus false flicks fired in, the lowest
  landmark never came above 0.97 and 0.91 of the frame height; the deepest any
  open-palm motion in the live session reached - leaving out two already outside
  the frame - was 0.89.
- **When:** only from IDLE, and at least `flick_refractory_sec` (1.5 - measured;
  with the motion rule alone the last non-gesture sweep in the recordings lands
  1.10 s after a release, and the original 1.0 let it through) after the
  previous flick AND after the previous release of any gesture. The second
  condition is deliberate: lowering the hand after a volume slide or a resize
  is a fast downward motion, often with the hand opening, and it must not
  minimise the window just adjusted. One-shot: the history is cleared when it
  fires, so one drop cannot fire twice.

**Status of the four constants, stated plainly.** `flick_hold_sec`,
`flick_still_speed`, `flick_drop` and `flick_drop_window_sec` are **proxies for
the positive side**. Nobody has yet recorded a deliberate hold-then-drop, so
nothing measures whether 0.25 s feels like a pause or 0.8 hand-lengths feels
like a drop. In particular **the owner's own session contains no hold before any
of its swipes and fires nothing at all under this rule** - it is a recording of
the old gesture being attempted, not of this one. What IS measured is the
negative side: zero fires on all eighteen recordings, about forty minutes of
ordinary hand motion, at every setting of the four that was tried. A guided clip
(`real_swipe_minimise`, Task 7) is the missing input.

- **Output:** one frame with `kind = GV_KIND_MINIMIZE` (new), `changed = 1`,
  `state = IDLE`. Nothing arms.
- **Backend:** `gv_window_ops` gains `minimize(void)`: take hold of the front
  window by the resize's rule, set the Accessibility "minimized" attribute,
  read it back, let go. Status in `result.win` on that frame.
- **HUD:** the message's `gesture` gains `"minimize"`; the HUD shows a
  minimise glyph for about a second, then fades as after any gesture.

Risk, stated plainly: every recorded session holds dozens of hand-lowering
motions after a release, and the waving clip holds open palms sweeping past the
camera. Those recordings are the negative corpus for the flick, and the rule is
tuned against them BEFORE the first live try: zero flicks on all of them is the
bar. The refractory after release is the first line of defence, and on the
evidence it does most of the work: with it switched off the motion rule fires
on twelve frames across the corpus, ten of them within 0.30 s of a release.

What was then actually found, when the rule was built and run (Findings below),
is that the three candidates this section first named could NOT reach zero. A
longer refractory could not: one of the two survivors was 3.15 s after any
release. A larger travel could not at any defensible value: that survivor scored
2.67 hand-lengths, so the threshold would have had to sit about 1% above a known
negative. Requiring the palm to face the camera could not even be evaluated:
twelve of the eighteen recordings, including both failing ones, predate the
handedness column and read handedness 0, so such a rule would have turned the
corpus green for want of metadata rather than by discriminating. What did work
is the size-stability condition in the Motion bullet above - one hand at both
ends of the span - which takes the corpus to zero with the worst remaining false
motion at 1.28 hand-lengths against the 1.5 threshold.

## What must not change

- The volume gesture and its recordings stay green.
- All logic in C; Python and the HUD only learn the new `gesture` value.
- One state machine; the flick is checked in `gesture.c`'s idle path, not as a
  parallel machine.

## Config and ABI

`gv_config` (mirrored field by field in `app/bridge.py`, order verified by eye):
`scale_max_slew` is replaced in place by `resize_rate_max`; appended at the
end: `resize_dead_zone`, `resize_rate_gain`, `win_write_interval_sec`,
`flick_drop`, `flick_drop_window_sec`, `flick_max_sideways`,
`flick_refractory_sec`, `flick_edge_margin`, `flick_hold_sec`,
`flick_still_speed`. (The first two were built as `flick_travel` and
`flick_window_sec`, and renamed in place when the gesture became hold-then-drop;
the Findings below still use the old names where they describe what was measured
at the time.) `gv_kind` gains `GV_KIND_MINIMIZE = 3`. `gv_output` and
`gv_step_result` keep their layout.

## Testing

- Rate control: unit tests on the pinch blend: holding inside the dead zone
  changes nothing; a steady offset beyond it changes the scale at the expected
  rate (measured against the formula, both directions); returning to neutral
  stops it; the rate is capped; lost frames earn no more than 0.1 s of credit;
  the rollback still returns the size from 0.3 s before a fist. Mutation-checked.
- Throttling: tests with the fake window backend: writes at most every
  interval, a skipped change written on the next allowed frame, the final word
  always written, no write to a window already let go.
- Flick: unit tests from the open-palm fixture moved down at speed (fires
  once), too slowly (no), sideways (no), with a gap in tracking (no), inside
  the refractory after a release (no), from ARMED (no). The waving,
  talking-hands and cup-reach clips and every recorded session replayed:
  zero flicks required. `replay` and `verdict` learn `minimize` (a clip that
  must minimise exactly once) and count flicks in `none`.
- Live, with the owner at the camera: `make run`, then guided clips
  `real_pinch_rate` (shrink, grow, hold still), `real_swipe_minimise`, and the
  five negatives again. Frame rate while resizing re-measured.

## Out of scope

Media playlist gestures (next round); the open-hand grow gesture (dropped);
the nose-scratch false arm (parked); Liquid Glass in the HUD (waits for
macOS 26 and its SDK).

## Findings

Task 1 (rate control) is built. `replay`'s stats now print a reversal line:
`resize: N direction reversals between consecutive writes (P%)`, computed the
same way as the "Why" section's number above (sign change between consecutive
relative steps of the reported scale, on the frames a resize reported a
change), so the same file can be compared before and after.

- **Before** (phone-style ratio, from "Why" above, on this same recording,
  `traces/real_resize_second_session.csv`): 15% of 1,657 window writes
  reversed direction.
- **After** (rate control, `core/build/replay traces/real_resize_second_session.csv
  stats`, same file): `resize: 131 direction reversals between consecutive
  writes (9%)`, out of 1,405 writes.

Honestly: this is a plausibility check, not proof. The recording was made
under the OLD mapping, where the hands were steering a ratio (moving to reach
a size and then stopping), not a rate (moving to set a speed and then holding
still). Replaying it through the new arithmetic answers "does the same finger
motion look less jittery under the new rule", which is encouraging (9% versus
15%) but is not the same claim as "a person operating the rate-controlled
gesture live produces fewer reversals" - that needs a new guided recording
under rate control, which Task 1 does not include. The write count also
differs from the spec's 1,657 (1,405 here); the 70-session count matches
exactly, so this is the same recording, but the two counts most likely come
from different measurement points (the original figure over actual backend
writes through the pre-throttling glue, this one over the pure state
machine's `changed` events) - the discrepancy was not run down further here.

Task 3 (the open-palm pose) is built: `gv_pose_is_open_palm` (all four
fingers `gv_finger_is_extended`, thumb `gv_thumb_is_out`). Measured first with
`core/tools/measure_palm.py`, using the exact same limits as `pose.c`
(extend_reach 1.20, extend_straight 0.93, thumb_extend_reach 1.05,
thumb_extend_straight 0.67), on the fraction of hand-present frames each
recording reads as an open palm:

| recording | hand frames | open palm | fraction |
| --- | --- | --- | --- |
| `traces/take1/real_waving.csv` | 1,019 | 195 | 0.191 |
| `traces/take1/real_talking_hands.csv` | 866 | 399 | 0.461 |
| `traces/take1/real_cup_reach.csv` | 1,111 | 227 | 0.204 |
| `traces/real_resize_second_session.csv` | 8,431 | 465 | 0.055 |
| `fixture_real_open_palm` | 1 | 1 | 1.000 |

The open-palm fixture reads as an open palm, as expected. The resize session
reads as one on only 5.5% of its frames, also as expected: that clip is
dominated by the pinch pose (middle/ring/pinky curled), which this rule
requires NOT curled. The point of the other three numbers is the opposite of
"rare": the pose alone must never fire anything, so it does not matter that
waving (19%), cup-reach (20%) and especially talking-hands (46%) show an open
palm on a large share of their frames - the flick gesture's motion check, not
this pose, is what has to keep those clips at zero flicks. Talking-hands
reading higher than waving was not anticipated going in (an open, gesturing
hand while talking apparently holds this shape more than a waving hand does,
which is often mid-motion, not fully splayed) - worth remembering when tuning
the flick's negative corpus in the next task.

### Task 4 (the flick): the negative corpus says the rule as specified is not ready

*Superseded by the section after it; kept for the record.*

The flick is built exactly as this spec describes it - open palm at both ends
of a `flick_window_sec` span, `flick_travel` hand-lengths down, sideways at
most `flick_max_sideways` of the fall, no tracking gap longer than the release
grace inside the span, only from IDLE and only `flick_refractory_sec` after the
previous flick and after any release. Unit tests: 27 checks, green, nine
mutants killed. **It does not pass its own acceptance bar.** Run over all
eighteen recordings on disk - none of which contains a deliberate swipe down -
it fires twice:

| recording | frames | hand frames | flicks (defaults 1.5 / 0.35 / 0.5 / 1.0) |
| --- | --- | --- | --- |
| `traces/take1/real_cup_reach.csv` | 1,179 | 1,111 | 0 |
| `traces/take1/real_nose_scratch.csv` | 1,180 | 1,151 | 0 |
| `traces/take1/real_slide_down.csv` | 1,170 | 1,114 | 0 |
| **`traces/take1/real_slide_up.csv`** | 1,192 | 839 | **1** (at 23.20 s) |
| `traces/take1/real_talking_hands.csv` | 1,181 | 866 | 0 |
| `traces/take1/real_typing.csv` | 1,178 | 101 | 0 |
| `traces/take1/real_waving.csv` | 1,190 | 1,019 | 0 |
| **`traces/real_first_session.csv`** | 9,556 | 5,234 | **1** (at 223.79 s) |
| `traces/real_second_session.csv` | 11,330 | 6,445 | 0 |
| `traces/real_third_session.csv` | 6,819 | 2,666 | 0 |
| `traces/real_fourth_session.csv` | 6,298 | 3,507 | 0 |
| `traces/real_fifth_session.csv` | 6,021 | 2,100 | 0 |
| `traces/real_resize_first_session.csv` | 652 | 385 | 0 |
| `traces/real_resize_second_session.csv` | 12,575 | 8,431 | 0 |
| `traces/probe_pinch_move.csv` | 299 | 299 | 0 |
| `traces/probe_pinch_open.csv` | 300 | 300 | 0 |
| `traces/probe_pointing.csv` | 300 | 300 | 0 |
| `traces/probe_volume_pose.csv` | 299 | 299 | 0 |

**The refractory is already doing most of the work.** With the refractory
switched off, the motion rule alone fires on twelve frames across the corpus;
ten of those are within 0.30 s of a release, which the 1.0 s refractory
catches. The two that get through are 3.15 s (slide_up) and 1.10 s
(first_session) after the last release. Both are the same event in kind: the
hand dropping out of the bottom of the frame at the end of a clip.

What the hand actually did, frame by frame (`core/build/replay` prints the
times; the geometry below is `gv_palm_*` on the raw rows):

- `real_slide_up.csv`, 22.87 s - 23.23 s: the palm centre falls from y 0.659 to
  0.917 while the hand's apparent size grows from 0.100 to 0.172 frame heights
  (**1.71x in a third of a second**) and then vanishes from the frame. Measured
  in hand-lengths of the span's FIRST frame - the smallest of them - that is a
  fall of 2.67, comfortably past any travel threshold. The open-palm reading
  also flickers: open at both ends of the span, not open for the ten frames
  between.
- `real_first_session.csv`, 223.46 s - 223.79 s: palm y 0.633 to 0.794, hand
  size 0.095 to 0.111 (1.18x), then gone. A fall of 1.70 hand-lengths, with
  sideways travel of 0.80 - just inside the 0.5 x 1.70 = 0.85 allowance.

**The three tuning candidates this spec names cannot close it.**

1. *A longer refractory after release.* 1.5 s clears first_session (1.10 s) and
   costs little. slide_up is 3.15 s after the last release; no defensible
   refractory reaches it, and a 3 s one would not generalise anyway - a hand
   entering and leaving the frame needs no gesture before it.
2. *A larger travel.* slide_up scores 2.67 hand-lengths, so travel would have
   to be 2.7 - about 1% of margin over a known negative, and above the 2.0 fall
   the unit tests use for the real gesture. Fitting a constant to one event.
3. *The palm facing the camera.* **Not evaluable on this corpus.** The
   handedness column was added to the trace format after most recordings were
   made, and neither failing file has it: twelve of the eighteen recordings,
   including `real_slide_up.csv` and `real_first_session.csv`, carry no hand
   label at all, so every replayed frame reads handedness 0. A rule of the form
   `gv_palm_turn x handedness` would reject every frame of those two files and
   the corpus would go green **for want of metadata, not because the rule
   discriminates**. That is a false pass and is worse than the current failure.

**What does work, measured.** Two extra vetoes were each tried against the full
corpus (worst remaining false motion, in hand-lengths, against the 1.5
threshold):

| rule | refractory 1.0 s | refractory 1.5 s |
| --- | --- | --- |
| as specified | 2.67 (fires 2x) | 2.67 (fires 1x) |
| A: open palm on EVERY frame of the span | 1.70 (fires 1x) | **1.28 (fires 0x)** |
| B: hand size across the span within the existing scale gate | 1.70 (fires 1x) | **1.28 (fires 0x)** |

Either A or B, with the refractory at 1.5 s, gives zero flicks with the worst
false motion in the whole corpus at 1.28 hand-lengths - a 17% margin under the
1.5 default, rather than a fitted constant.

What each would cost the real gesture, over the 2,968 spans in the corpus whose
two ends are an open palm: rule A vetoes 19% of them, rule B 8%. **B is the
better of the two.** It is also the more principled: `flick_travel` is measured
in "hand-lengths", and across these spans the hand's apparent size ranges from
0.07x to 13.56x, so on a glitchy span that unit means nothing. The state
machine already has a name and a measured constant for "could this be the same
hand" - `gate_scale_min` / `gate_scale_max` (0.7 - 1.4), used by
`frame_is_plausible` on every armed frame - and B is that same test applied
across the flick's span. It makes the travel unit well defined rather than
adding a new idea.

**Not adopted, deliberately.** B is a change to the rule this spec defines, not
a retune of one of its constants, and the 8% it vetoes cannot be checked
against a genuine flick until one is recorded - there is still no positive
flick clip. That is a design call for the spec's author, not a tuning step. The
gesture is therefore built and tested but **must not be enabled live as it
stands**: on this evidence it would minimise a window roughly once per
twenty minutes of ordinary hand movement, for nothing.

### Task 4, after the fix: the corpus is at zero

The controller's ruling was to adopt the measured fix. Two changes from the
table above: the flick's span must show one hand - the `gv_hand_scale` ratio
between the span's oldest and newest samples within `gate_scale_min ..
gate_scale_max` (0.7 - 1.4), reusing the existing plausibility gate rather than
adding a constant - and `flick_refractory_sec` moves from 1.0 to 1.5. Nothing
else moved: travel stays 1.5, window 0.35, sideways 0.5. No palm-facing rule.

The ratio is compared **oldest sample to newest sample**, not every consecutive
pair; that is what was measured, and it is the same shape as the gate it reuses
(`frame_is_plausible` compares one frame with the last believed one).

| recording | frames | hand frames | flicks BEFORE | flicks AFTER |
| --- | --- | --- | --- | --- |
| `traces/take1/real_cup_reach.csv` | 1,179 | 1,111 | 0 | 0 |
| `traces/take1/real_nose_scratch.csv` | 1,180 | 1,151 | 0 | 0 |
| `traces/take1/real_slide_down.csv` | 1,170 | 1,114 | 0 | 0 |
| `traces/take1/real_slide_up.csv` | 1,192 | 839 | 1 (23.20 s) | **0** |
| `traces/take1/real_talking_hands.csv` | 1,181 | 866 | 0 | 0 |
| `traces/take1/real_typing.csv` | 1,178 | 101 | 0 | 0 |
| `traces/take1/real_waving.csv` | 1,190 | 1,019 | 0 | 0 |
| `traces/real_first_session.csv` | 9,556 | 5,234 | 1 (223.79 s) | **0** |
| `traces/real_second_session.csv` | 11,330 | 6,445 | 0 | 0 |
| `traces/real_third_session.csv` | 6,819 | 2,666 | 0 | 0 |
| `traces/real_fourth_session.csv` | 6,298 | 3,507 | 0 | 0 |
| `traces/real_fifth_session.csv` | 6,021 | 2,100 | 0 | 0 |
| `traces/real_resize_first_session.csv` | 652 | 385 | 0 | 0 |
| `traces/real_resize_second_session.csv` | 12,575 | 8,431 | 0 | 0 |
| `traces/probe_pinch_move.csv` | 299 | 299 | 0 | 0 |
| `traces/probe_pinch_open.csv` | 300 | 300 | 0 | 0 |
| `traces/probe_pointing.csv` | 300 | 300 | 0 | 0 |
| `traces/probe_volume_pose.csv` | 299 | 299 | 0 | 0 |
| **total** | **62,719** | **36,167** | **2** | **0** |

**The margin, not just the count.** Scoring every idle frame that passes the
pose, gap, sideways and size tests, the strongest downward motion left anywhere
in the corpus is **1.28 hand-lengths**, against a `flick_travel` of **1.5** - a
17% margin, and one that comes from the rule rather than from a constant fitted
to the failures. The next four are 1.27, 1.23, 1.05 and 1.00. Both changes are
load-bearing: put the refractory back to 1.0 and `real_first_session.csv` fires
again at 223.79 s (scoring 1.70); drop the size check and `real_slide_up.csv`
fires again at 23.20 s (2.67).

**What it costs the real gesture.** Over the 2,968 spans in the corpus whose two
ends are an open palm, the size condition vetoes **8%** - every one of them a
span where the hand's apparent size changed by more than 1.4x or less than 0.7x
in a third of a second. (The alternative measured, requiring the open palm on
every frame of the span rather than at its ends, reaches the same zero but
vetoes 19%, so it was not taken.)

**Said plainly: the positive side is still unmeasured.** There is no recording
of a deliberate swipe down, so every number above is about what the flick does
NOT do. Whether 1.5 hand-lengths in 0.35 s is a comfortable motion to perform,
and whether that 8% ever eats a real flick, are open questions until a guided
clip exists - Task 7. `flick_travel`, `flick_window_sec` and
`flick_max_sideways` remain proxies from this spec; only the refractory and the
size condition are measured, and they are measured against negatives only.

### Task 5 (the backend): the flick reaches the window, and the checking moved off the hot path

`gv_window_ops` gains `minimize()` and `minimize_settled()`, wired to a new
`win_minimize` / `win_minimize_settled` pair that takes the front window by the
resize's rule (minus the resizable check - fixed-size windows minimise perfectly
well) and keeps the element for `win_restore`, the way back for the command-line
tool. The glue calls the first on the one frame a flick fires. `win minimize`
and `win restore` are new subcommands of `core/build/win`.

**A minimise is not finished when the write returns.** The first by-hand run of
`core/build/win minimize` reported

```
win: minimize: the window accepted the change and did not change ...
target     Slack
```

on a window that had in fact minimised: Slack was gone from the on-screen window
list straight afterwards and its `kAXMinimized` read `true`. (It was put back by
hand; nothing was left in the Dock.) Measured with a scratch probe, on Terminal
and on Slack:

| after a `kAXMinimized = true` write that returned kAXErrorSuccess | the attribute reads |
| --- | --- |
| 0 ms (immediately) | **false** |
| ~309 ms | the read itself FAILS: kAXErrorCannotComplete (-25204), after blocking the full 0.25 s messaging timeout - the app is busy animating the window into the Dock |
| ~568 ms | true |

So the spec's "set the attribute, read it back" cannot be done in one call. The
first implementation waited for it inside `win_minimize`, which made the status
true and cost ~0.6 s of frozen camera loop per flick. **The design taken instead
defers the verification rather than dropping it:**

- `win_minimize` writes and returns at once. `WIN_OK` from it means only that
  the write was accepted; it keeps the window element (a minimised window is off
  the window list, so that handle is the only route back to it) and notes the
  time.
- `win_minimize_settled` reads the attribute back ONCE, never blocking beyond a
  single Accessibility call, and answers `WIN_OK`, `WIN_ERR_IGNORED` (the
  `WIN_SETTLE_DEADLINE_SEC` = 1.0 s deadline passed and the window is still
  there) or the new `WIN_PENDING`. A read that times out counts as pending: an
  app too busy to answer is what the animation looks like from outside.
- `win_status` therefore gains a positive value: **0 ok, positive not-yet,
  negative error**. `app/main.py` now treats only `win < 0` as a problem, so a
  pending minimise raises no banner; `bridge.WIN_PENDING` mirrors it.
- `gv_step` asks once per frame after a successful write, until the answer is
  anything but pending. Across a flick, `result.win` reads `WIN_OK` (the write
  took), `WIN_PENDING` on each frame the window is still travelling, then
  `WIN_OK` or `WIN_ERR_IGNORED` on the frame it settles - and then nothing.

**By-hand check (a), `core/build/win minimize`**, which polls exactly as the
camera loop does:

```
target     Terminal
written    ok
      0 ms since the write, the ask cost   0 ms  not yet: the window is still on its way
     89 ms since the write, the ask cost  34 ms  not yet: the window is still on its way
    177 ms since the write, the ask cost  35 ms  not yet: the window is still on its way
    264 ms since the write, the ask cost  34 ms  not yet: the window is still on its way
    354 ms since the write, the ask cost  35 ms  not yet: the window is still on its way
    444 ms since the write, the ask cost  35 ms  not yet: the window is still on its way
    532 ms since the write, the ask cost  35 ms  not yet: the window is still on its way
    582 ms since the write, the ask cost   0 ms  ok
settled    ok after 582 ms
restored   ok
```

**By-hand check (b), the live-conditions probe** (camera open, 45 preview frames
shown, then the same pair through ctypes, timing each call):

```
camera open, showing 45 frames
win_minimize -> 0 (ok) in 18 ms
target       -> 'Terminal' (win_target_name status -2)
  poll 1: 1 (not yet ...) - the call itself took  0 ms,  18 ms since the write
  poll 2: 1 (not yet ...) - the call itself took 31 ms, 100 ms since the write
  ... polls 3 to 7, each 30-31 ms ...
  poll 8: 0 (ok)          - the call itself took 17 ms, 577 ms since the write
settled      -> 0 after 8 polls, 577 ms
win_restore  -> 0 (ok) in 520 ms
```

With the camera in use it still found a real window and not one of the Window
Server's 10x18 pt camera-indicator windows: `win_is_candidate` holds for
minimising as it does for resizing, which is the point of the two sharing one
search (`take_front_window`). The `win_target_name` status of -2 is correct and
documented - the name is filled in, but nothing is held for resizing.

**What this costs the live loop, in two steps.** Deferring the check removed the
single 0.6 s freeze, but not the total: the first version still paid the
standard `AX_TIMEOUT_SEC` of 0.25 s on every asking frame, because a busy
application does not answer and the read waits out its whole timeout (measured:
polls of 0, 251 and 210 ms). So the settled read was given a timeout of its own,
`WIN_SETTLE_READ_TIMEOUT_SEC` = 0.03 s: an application that is ready answers in
under a millisecond, one that is busy answers nothing, and silence already means
"not yet" - there is nothing to be gained by waiting for it. The 1.0 s deadline,
not the read, is still what decides that a window is never going to move.

Measured after that change, under the camera: the write costs ~18 ms, each of
the eight asks 30-31 ms, and the window is confirmed 577 ms after the write. A
frame's budget at 30 fps is 33 ms, so while a minimise is settling the loop runs
at roughly half rate for about 0.6 s, instead of stopping dead for a quarter of
a second at a time. Whether even that is visible is a question for the first
live flick (Task 7), and the numbers to beat are in this paragraph.

`win_restore` still waits for the window to come back, deliberately: no camera
loop ever calls it (it exists for the tool and for probes), and a way back that
cannot say whether it worked is not worth much. Measured at 527-560 ms.

**One `win` field, two gestures.** A minimise settles in about 0.6 s and a
resize arms in `arm_hold_sec` (0.25 s), so flicking and then pinching has both
live at once - and `gv_step_result.win` can only say one thing. The resize's
status wins on any frame the resize path speaks for: a window that cannot be
resized is something the person can act on, while a minimise still on its way
is not news. The pending minimise is not dropped for it; the asking resumes on
the frames after the resize lets go. (Found in review: without that rule the
poll overwrote a resize's `WIN_ERR_NOT_TRUSTED` / `NOT_RESIZABLE` / `IGNORED`
for up to a second, and the preview raised no banner at all.) The banner also
names the right gesture now - a failed flick reads `WINDOW NOT MINIMISING`, not
`WINDOW NOT RESIZING`, and carries the app's name.

`leaks --atExit` reports 0 leaks on `core/build/win get` and, re-run on the
current binary after the deferral rewrite, 0 leaks on `core/build/win minimize`
as well (2214 nodes, 315 KB, footprint 5216K).

### First live session of the redesign (2026-09-18, about 7 minutes, `traces/real_rate_first_session.*`, untracked)

Run after Task 5 with Task 6 held (no minimise cue on the preview or HUD). The
owner's verdict, verbatim: "minimize by flick doesnt work too well, and
resizing works but well need to tune it to be more comfortable and better
later." What the recording shows:

**Resize by rate: the three measured problems are gone; comfort is not tuned.**
- Frame rate while a window is written: 29.4 fps median (was 16.3). Idle 30.0.
- Direction reversals between consecutive writes: 2% (was 15%): the jitter at
  rest is gone.
- 34 resize sessions, 1,406 writes, end scales from 0.10x to 6.42x; 8 of 34
  ended within 5% of where they began. No pinch-first needed; range unlimited.
- The dead zone, gain and cap are still the proxies from the design; "more
  comfortable" is the owner's word for what Task 7's guided clips will tune.
- **A false "did not change size" banner still appears on the FIRST write of
  each Chrome resize** (83 frames, all Chrome): Chrome applies a size one frame
  late, so the read-back after the first write sees the old size while the
  window has not yet "responded". `win_write_was_ignored` needs the same
  patience the minimise got: judge "ignored" only after a deadline (or a few
  writes), not on the first read-back.

**The flick: fired once in the whole session, correctly.**
- At 125.6 s a flick fired, the write took (target Notes), `WIN_PENDING` for
  0.6 s, then settled `WIN_OK`: the whole minimise path worked end to end, once.
- Replaying the landmarks through the rule with each condition printed: 31
  downward open-palm motions of at least 0.75 hand-lengths inside 0.35 s. Only
  one passed every condition (126.98 s, and it sat inside the 1.5 s refractory
  after the flick above). The rest were vetoed by, in order of frequency:
  1. **Travel**: the owner's swipes measure 0.8 - 1.2 hand-lengths inside 0.35 s
     against the 1.5 required. Over a 0.5 s window the same swipes measure
     1.4 - 2.2 (three would pass); over 0.7 s, 1.8 - 3.0. But at 0.5 s one
     corpus recording (`real_first_session`) fires falsely again, and at 0.7 s
     one other (`real_fourth_session`) does. The positive side and the negative
     side now overlap on travel-in-a-window alone.
  2. **The size rule**: a real swipe down moves the hand away from the camera,
     so its apparent size falls (ratios 0.66, 0.68, 0.51, 0.24 on real swipes).
     The rule that removed the two corpus false positives (a hand dropping OUT
     of the frame) also vetoes real swipes. The better discriminator is the
     thing itself: whether the hand is leaving the picture (landmarks at or
     below the bottom edge) rather than whether it changed size.
  3. **The open-palm pose lost mid-swipe**: fast motion blurs the fingers and
     the pose reads as not open on the newest sample (`open(old,new)=1,0`).
     Requiring the open palm at the START of the span and only "not a fist" at
     the end is the candidate.
- Consequence for tuning: the flick cannot be tuned on travel and window alone
  without false positives; it needs the leaving-the-frame veto in place of the
  size rule, and then a labelled positive clip (`real_swipe_minimise`) against
  the corpus. Task 7's guided clips are the way; nothing was changed tonight.

### Task 4b (tuning the flick to fire): the study, and why it stopped short

The rule was changed as Task 4b's brief specified - the size-stability condition
replaced by a leaving-the-frame veto, the end-of-span pose relaxed from "open
palm" to "not a fist" - and then the window and travel were swept. **The rule
change stands and the corpus gate still passes at zero. The window/travel
tuning did not reach its bar and nothing was adopted from it.**

The tool is `core/tools/tune_flick.c` (`make -C core build/tune_flick`), kept in
the tree with its own how-to-run header. Two modes: `spans`, the probe that
measures every downward open-palm motion in a recording, and `grid`, which
sweeps the two constants through the real `gv_update` (no rule is reimplemented
in it) and counts.

#### The positives, and what they are worth

`traces/real_rate_first_session.csv`, the owner's 7-minute session, replayed:
**16 motions** where an open palm inside a 0.35 s window is followed by the palm
centre at least 0.75 hand-lengths lower; runs of frames are one motion when no
quiet gap of a whole window separates them. **15 of the 16 happen while the
state machine is idle**, and only those can ever fire.

These are **proxies, not labels**. No recording carries any record of what the
owner meant, so "caught" below means "a fire landed on a motion this probe
called a candidate", never "a swipe the owner intended was recognised". Looking
at the 16 by eye, nine of them fall in one 11-second burst (117 - 128 s) around
the two minimises that actually fired live, and the rest are scattered singles
between resizes that are as likely to be a hand being lowered.

The earlier probe reported **31** such spans. Reproduced here, that number comes
from a probe with **no open-palm requirement at all**: at the same thresholds
that gives 30 motions, and 14 of those 30 are a closed hand falling (all four
fingers curled at the end), which no version of this gesture should ever fire
on. The 16 above are the same measurement with the open palm required, and they
are the honest denominator.

#### The table

Rows are `flick_window_sec`, columns `flick_travel`; each cell is
**positives caught (of the 15 eligible) / false fires across all 18 corpus
recordings**. Everything else is at the shipped defaults, with the edge veto and
the relaxed end pose in place.

At `flick_edge_margin` **0.08**:

| window \ travel | 1.0 | 1.2 | 1.5 | 1.8 | 2.0 |
|---|---|---|---|---|---|
| 0.35 | 3 / 0 | 3 / 0 | **3 / 0** | 2 / 0 | 2 / 0 |
| 0.45 | 4 / 2 | 4 / 1 | 3 / 0 | 3 / 0 | 2 / 0 |
| 0.50 | 3 / 2 | 3 / 2 | 2 / 0 | 2 / 0 | 2 / 0 |
| 0.60 | 3 / 2 | 3 / 1 | 3 / 1 | 3 / 1 | 3 / 0 |

At `flick_edge_margin` **0.10** (shipped):

| window \ travel | 1.0 | 1.2 | 1.5 | 1.8 | 2.0 |
|---|---|---|---|---|---|
| 0.35 | 3 / 0 | 3 / 0 | **3 / 0** | 2 / 0 | 2 / 0 |
| 0.45 | 4 / 2 | 4 / 1 | 3 / 0 | 3 / 0 | 2 / 0 |
| 0.50 | 3 / 2 | 3 / 2 | 2 / 0 | 2 / 0 | 2 / 0 |
| 0.60 | 2 / 2 | 2 / 1 | 2 / 1 | 2 / 1 | 2 / 0 |

At `flick_edge_margin` **0.12**:

| window \ travel | 1.0 | 1.2 | 1.5 | 1.8 | 2.0 |
|---|---|---|---|---|---|
| 0.35 | 3 / 0 | 3 / 0 | **3 / 0** | 2 / 0 | 2 / 0 |
| 0.45 | 3 / 2 | 3 / 1 | 2 / 0 | 2 / 0 | 2 / 0 |
| 0.50 | 3 / 2 | 3 / 2 | 2 / 0 | 2 / 0 | 1 / 0 |
| 0.60 | 2 / 2 | 2 / 1 | 2 / 1 | 2 / 1 | 2 / 0 |

**The best cell at zero corpus fires catches 3 of 15.** The brief's tie-break
(most positives, then the larger travel) picks window **0.35** and travel
**1.5** at the shipped margin - which is exactly what already shipped. The
window and travel were therefore left alone; the whole gain comes from the edge
veto and the relaxed end pose.

`flick_edge_margin` itself was picked from measurement, not from this table: at
the frame each of the two corpus false flicks fired on, the hand's lowest
landmark never came above **0.97** and **0.91** of the frame height, and the
deepest any open-palm motion in the live session reached was **0.89**. 0.10 is
the round number between 0.89 and 0.91. That is a 0.02 gap, and it is the
weakest number in the rule.

#### Before and after, on the same session

| | live, old rule | new rule, replayed |
|---|---|---|
| fires in the 7-minute session | 2 (121.14 s, 125.65 s) | 3 (121.1, 125.6, 127.9 s) |
| of the 15 eligible proxy motions | 2 | 3 |
| false fires on the 18 recordings | 0 | 0 |

#### The corpus, per recording (new rule, shipped defaults)

`core/build/replay <file> stats`: **flicks 0** on every one of
`take1/real_{cup_reach,nose_scratch,slide_down,slide_up,talking_hands,typing,waving}.csv`,
`real_{first,second,third,fourth,fifth,resize_first,resize_second}_session.csv`
and `probe_{pinch_move,pinch_open,pointing,volume_pose}.csv`. The gate holds.

#### Why the window/travel sweep cannot do better - the measurement that matters

The brief expected a longer window to measure MORE travel ("over a 0.5 s window
the same swipes measure 1.4 - 2.2"). Measured against the rule as built, it
measures **less**. The rule judges the two ENDS of the whole window, and half a
second before a swipe the hand was not yet raised - so the oldest sample is
already low, and the fall across the window shrinks or goes negative:

| motion | fall over the whole window, 0.35 s | 0.45 s | 0.50 s | deepest fall from any open palm inside 0.35 s |
|---|---|---|---|---|
| 121.8 s | 0.81 | 0.05 | -0.28 | 4.01 (from a hand 3.1x smaller: see below) |
| 123.2 s | 0.04 | -2.69 | -3.02 | 0.87 |
| 125.6 s | 0.68 | 0.47 | 0.47 | 1.07 |
| 126.5 s | 0.81 | -1.96 | -4.15 | 0.81 |
| 127.8 s | 0.79 | 0.80 | 0.79 | 0.80 |
| 159.9 s | 0.92 | 0.68 | 0.54 | 1.07 |

Two things follow, and neither is a constant that can be tuned:

1. **The owner's swipes are about 0.8 - 1.1 hand-lengths**, not 1.5. (Two
   motions measure 2.7 - 4.0, but only because the open-palm frame they are
   measured from is a hand 2 - 3x smaller than the one at the end, so
   "hand-lengths" there means little.) The travel would have to come down to
   about 0.8 to catch most of them.
2. **Judging the whole window is what loses them.** The obvious alternative -
   measure the deepest fall from ANY open palm inside the window, the last
   column - is a different RULE, which this task's brief does not name, so it
   was measured and not built.

What that alternative would cost, measured (best sub-span at 0.8 hand-lengths,
with the sideways limit, the not-a-fist end, the edge veto and idle-only all
still applied, but before the refractory): **7 of the 16 positives, and 6
candidate fires across 5 of the 18 corpus recordings** (`slide_up` 2,
`first_session` 2, `talking_hands` 1, `fourth_session` 1). The refractory after
release would remove some of those six, but not obviously all: the corpus gate
is zero and this does not reach it.

The one corpus recording that fires at the looser cells of the table is
`real_second_session.csv` at 294.0 s: a hand sweeping down and across, which at
one frame reads as a fall of 1.39 hand-lengths with only 0.59 sideways (inside
the 0.5 x 1.39 = 0.70 allowance), still well inside the picture (lowest landmark
0.69) and 1.5 s clear of any release. It is a genuine near-miss, not an artefact
- the positive and negative populations overlap on this rule, as the previous
session already suspected.

#### Where this leaves the gesture

Unchanged from the previous round in kind: the flick now fires on three of the
fifteen proxy motions instead of two, still with a clean corpus. That is better
than it was and it is not "works". Closing the rest needs either a rule the
brief does not name (the best sub-span above, whose cost is measured here) or -
the honest input - **a guided clip of deliberate swipes and deliberate
non-swipes** (`real_swipe_minimise` and the negatives, Task 7), so that the word
"positive" stops being a proxy.

### Task 4b, part two: hold-then-drop measured, then chosen and built

The window/travel table above is the whole of what tuning a bare swipe can do:
3 of 15 proxy positives at zero corpus fires, with the tie-break landing back on
the constants that already shipped. The reason it cannot do better is in the
table under "Why the window/travel sweep cannot do better" - the swipes are
0.78 - 1.07 hand-lengths and ordinary hand-lowering is the same size - so a
different SHAPE was measured instead, and then chosen.

**The variant:** an open palm held nearly still for `hold` seconds (open on
every frame, palm centre moving slower than `still` hand-lengths per second),
then within `drop_window` seconds a drop of at least `drop` hand-lengths below
where it was held, sideways at most 0.5 x the drop, not a fist and inside the
frame at the end. Refractory and IDLE-only as before.

Each cell is **positives caught (of the 15 eligible) / fires across all 18
corpus recordings**:

| hold | still | drop_window | drop | caught | corpus fires |
|---|---|---|---|---|---|
| 0.2 | 0.5 | 0.5 | 0.6 / 0.8 / 1.0 | 0 / 15 | 0 |
| 0.2 | 0.5 | 0.7 | 0.6 / 0.8 / 1.0 | 0 / 15 | 0 |
| 0.2 | 1.0 | 0.5 | 0.6 / 0.8 / 1.0 | 0 / 15 | 0 |
| 0.2 | 1.0 | 0.7 | 0.6 / 0.8 / 1.0 | 0 / 15 | 0 |
| 0.3 | 0.5 | 0.5 | 0.6 / 0.8 / 1.0 | 0 / 15 | 0 |
| 0.3 | 0.5 | 0.7 | 0.6 / 0.8 / 1.0 | 0 / 15 | 0 |
| 0.3 | 1.0 | 0.5 | 0.6 / 0.8 / 1.0 | 0 / 15 | 0 |
| 0.3 | 1.0 | 0.7 | 0.6 / 0.8 / 1.0 | 0 / 15 | 0 |

Every cell: **nothing fires, on the positives and on the corpus alike.** The
reason is the same in every one of them: **0 of the 15 eligible proxy positives
had a 0.2 s (or 0.3 s) still-open hold within 0.7 s before them, at either speed
limit; only 1 of the 15 had one anywhere in the 1.5 s before.** The owner raises
his hand and swipes in one motion. He does not pause - and nothing had ever told
him to.

The zeros are not a broken detector. Still-open holds are common in these
recordings, they simply never sit in front of a drop: 487 frames end a run of at
least 0.2 s in the positives file alone (169 in `real_first_session`, 91 in
`real_nose_scratch`), with the longest runs reaching 0.9 - 1.7 s.

**The decision (owner, after seeing this):** build hold-then-drop. The evidence
splits cleanly in two, and both halves matter:

- **The negative side is as good as it gets.** Zero on 40 minutes of ordinary
  hand motion at every setting tried, because a held open palm followed by a
  drop does not happen by accident. The bare swipe never managed that without
  also refusing the swipes.
- **The positive side is untested, and this recording cannot test it.** It says
  only that the owner does not *currently* pause. Whether pausing is comfortable
  is a question for a guided clip recorded under the new instruction, not for
  this one.

Shipped constants: `flick_hold_sec` 0.25 (the same pause `arm_hold_sec` already
asks for), `flick_still_speed` 1.0, `flick_drop` 0.8 (a little under the
measured 0.78 - 1.07, which the hold now makes safe to ask for),
`flick_drop_window_sec` 0.6. All four are proxies for the positive side.

**Corpus, per recording, with hold-then-drop shipped** (`replay <file> stats`):
**flicks 0** on all eighteen -
`take1/real_{cup_reach,nose_scratch,slide_down,slide_up,talking_hands,typing,waving}.csv`,
`real_{first,second,third,fourth,fifth,resize_first,resize_second}_session.csv`,
`probe_{pinch_move,pinch_open,pointing,volume_pose}.csv` - and **0 on
`real_rate_first_session.csv` too**, the owner's own session, which is said here
rather than hidden: that recording holds sixteen attempts at the OLD gesture and
not one hold, so the new rule is right to ignore it.

The study tool stays in the tree at `core/tools/tune_flick.c`. Its `armdrop`
mode is the reference this rule was built from, and it checks itself against
`gv_update` before printing anything - comparing, frame by frame, which frames
each of the two fires on, and reporting how many frames they decided
differently out of how many either fired on.

That check is run twice, because at the shipped constants **neither rule fires
on any of the nineteen recordings, and 0 == 0 demonstrates nothing**; the tool
says so in as many words. The second run uses a deliberately loose setting
(hold 0.03 s, still 50 hand-lengths/s, window 1.5 s, drop 0.05) where both fire:
**52 frames where either fired, 0 decided differently.** Comparing decisions
rather than counts is what found two real divergences, both since fixed in the
tool: it searched back by time alone where the core is limited by its
40-entry ring buffer, and it anchored the refractory on the last busy frame
rather than on the frame the machine arrives at idle. Removing either fix alone
brings the mismatches back, so both were load-bearing.

The tool shares the core's geometry, config and - since that check - the two
frame-level rules that decide what is admissible at all (`gv_frame_is_usable`
and `GV_MIN_HAND_SCALE`, lifted out of `gesture.c` into `gesture_internal.h`).
It still re-implements the flick decision itself, which is the point: the
constants can be swept without rebuilding the core.
