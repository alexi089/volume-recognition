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
| Swipe the window away to minimise | **Yes, an open palm swiped DOWN.** Sideways swipes stay reserved for the media gestures planned next |

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

## Swipe down to minimise: a "flick"

A flick is a new class of gesture: it does not arm and hold, it **fires once**.

- **Pose:** open palm: index, middle, ring and pinky extended and the thumb
  out (`gv_pose_is_open_palm`, new, measured on `fixture_real_open_palm` and
  the open-palm frames of the recordings).
- **Motion:** over the last `flick_window_sec` (0.35 s) of frames, the palm
  centre moved DOWN by at least `flick_travel` (1.5 hand-lengths, vertical travel
  scaled by the aspect ratio as the slide does), with sideways travel at most
  `flick_max_sideways` (0.5) times the downward travel; the open palm is
  present on the first and last frame of that span, and no frame in the span
  is missing for longer than the release grace.
- **When:** only from IDLE, and at least `flick_refractory_sec` (1.0) after the
  previous flick AND after the previous release of any gesture. The second
  condition is deliberate: lowering the hand after a volume slide or a resize
  is a fast downward motion, often with the hand opening, and it must not
  minimise the window just adjusted.
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
bar. The refractory after release is the first line of defence; if the corpus
still fires, the candidates are a longer refractory, a larger travel, or
requiring the palm to face the camera.

## What must not change

- The volume gesture and its recordings stay green.
- All logic in C; Python and the HUD only learn the new `gesture` value.
- One state machine; the flick is checked in `gesture.c`'s idle path, not as a
  parallel machine.

## Config and ABI

`gv_config` (mirrored field by field in `app/bridge.py`, order verified by eye):
`scale_max_slew` is replaced in place by `resize_rate_max`; appended at the
end: `resize_dead_zone`, `resize_rate_gain`, `win_write_interval_sec`,
`flick_travel`, `flick_window_sec`, `flick_max_sideways`,
`flick_refractory_sec`. `gv_kind` gains `GV_KIND_MINIMIZE = 3`. `gv_output`
and `gv_step_result` keep their layout.

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
