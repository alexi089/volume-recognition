# Pinch to resize the front window: design

Date: 2026-09-17. Status: approved by the owner; the recogniser half is built
(gesture definitions, resize gesture), the window half is not. Sections marked
"measured" were revised from real clips after approval; the revisions narrow the
gesture's range and are listed under "What building it changed". First gesture of
iteration 2 (the four-finger swipe follows and is out of scope here).
Supersedes the pinch half of `2026-09-17-iteration-2-gestures-brief.md`.

## Goal

Hold thumb and index out, other fingers curled, in front of the webcam; spread
the two fingertips and the frontmost application window grows, bring them
together and it shrinks, like pinch-zoom on a phone. Close the hand or leave the
frame and the window stays as it is. Secondary goal, as before: the logic is C.

## Decisions (owner, 2026-09-17)

| Question | Decision |
|---|---|
| Which gesture first | Pinch to resize |
| Mapping | Phone-style: window scale = fingertip gap now / gap when it armed. Release and re-arm to go further |
| Arming pose | Thumb and index extended, middle, ring and pinky curled, held still |
| What stays fixed | The window's centre |
| Core architecture | One recogniser; each gesture is a small definition |

## Feasibility, verified before designing (2026-09-17)

- `AXIsProcessTrusted()` returns 1 for processes launched from the build
  session's host: Accessibility permission is already granted there.
- Asking the system-wide element for the focused application fails
  (`kAXErrorCannotComplete`, -25204). Working route, all plain C: take the owner
  pid of the first layer-0 on-screen window from `CGWindowListCopyWindowInfo`,
  `AXUIElementCreateApplication(pid)`, then `kAXFocusedWindowAttribute`.
- A real write works and can be verified: the front window (Chrome at that
  moment) went 2144x1387 -> 2084x1347 -> back, each read back exactly.
- Writing the size alone leaves the top-left corner where it was, so a fixed
  centre means writing position as well.

## Architecture

```
camera -> [Python] MediaPipe -> landmarks + handedness + timestamp
       -> [C] gv_step:  recogniser -> { state, kind, volume | scale, changed }
                        -> backend for that kind: volume (CoreAudio) | window (Accessibility)
       -> [Python] overlay; one JSON line per frame to the HUD
```

Python stays a pipe. Nothing about gestures or windows is decided there.

### The recogniser

`gesture.c` keeps the state machine and everything gestures share: the
usable-frame check, arm-hold and release-grace timing, the plausibility gate
(hand size and 2D speed against the last believed frame), "frames that fail
freeze, they do not move anything", and release. A gesture is a definition:

```c
typedef struct {
    gv_kind kind;                                        /* GV_KIND_VOLUME, GV_KIND_RESIZE */
    int  (*can_arm)(const gv_frame *, const gv_config *);
    int  (*can_stay)(const gv_frame *, const gv_config *);
    void (*on_armed)(gv_ctx *, const gv_frame *);        /* capture anchors */
    void (*on_frame)(gv_ctx *, const gv_frame *, double dt, gv_output *);
    int  (*on_release)(gv_ctx *, int pose_ended);        /* optional: a last word */
} gv_gesture;
```

(As built the hooks also take the filtered palm position and the time, there is
an `on_idle` reset hook, and `on_frame` returns `changed`; `gesture_internal.h`
is the reference.)

- Volume moves into `gestures/volume.c` unchanged in behaviour. This refactor
  lands first, alone, with every existing check green.
- While idle the definitions are tried in a fixed order; the first whose arm
  pose holds starts arming, and that kind is kept until release. Only one
  gesture can be armed at a time, by construction.
- The arm poses are mutually exclusive: resize needs the middle finger curled
  and the thumb out; volume needs the middle finger extended; pointing has the
  thumb in; an open palm arms neither. A test asserts that no fixture satisfies
  two definitions' `can_arm`.
- `gv_output` gains `kind` and `scale` (1.0 at arming). `volume` keeps its
  meaning for the volume gesture.

### The resize gesture (`gestures/resize.c`)

- **Measure:** `gap = dist(thumb tip, index tip) / hand_scale`, in hand-lengths,
  so distance from the camera does not matter; One Euro filtered.
- **Scale:** `gap / gap_at_arm`, clamped to 0.25 - 4.0, with a 1% dead-band.
- **Rate limit in log space** (about 2x per second, credit across lost frames
  capped as for volume), so a tracking glitch is a ramp that the release timer
  cuts short, not a window snapping across the screen.
- **Arming needs a gap of at least 0.73 hand-lengths (measured).** Touching fingertips
  hide each other, which is where tracking is worst, and a tiny starting gap
  makes the ratio explode.
- **Arming also needs the thumb tip clear of the curled fingers** (at least 0.62
  hand-lengths from the nearest joint of the middle, ring and pinky; measured).
  A thumb resting on the fist while pointing is as long and straight as one
  held out; only where its tip is tells them apart.
- **Stay pose (measured, revised):** thumb not in, index not curled, middle not
  extended. A full pinch and a fist are nearly the same shape in 2D, and a fist
  must never mean "shrink to the minimum"; see "What building it changed".
- **Release rolls back (added):** when the gesture ends because the hand changed
  shape while still in view, it ends on the scale it reported 0.3 s
  (`release_rollback_sec`) before its last good frame, sent as one final
  `changed` frame with `kind` still RESIZE. Closing the hand is how every
  gesture here ends, and on the way to a fist the fingertips pass through every
  smaller gap. A hand that leaves the frame rolls nothing back. A resize shorter
  than the rollback ends at scale 1.0: nothing happened.
- **The thumb rule was set from a measurement clip**, the way the finger
  thresholds were; see "Thumb measurements" below.

### Thumb measurements (2026-09-17, four 10 s probe clips, hand found in every frame)

5th / 50th / 95th percentiles; distances as `pose.c` measures them.

| Measurement | pinch-ready, held | pinch and spread | pointing, thumb in |
|---|---|---|---|
| thumb tip to index knuckle, over hand size | 0.73 / 0.83 / 0.89 | 0.05 / 0.55 / 1.00 | 0.84 / 1.11 / 1.28 |
| thumb reach (tip-wrist over MCP-wrist) | 1.72 / 1.81 / 1.88 | 1.55 / 1.82 / 1.92 | 0.12 / 0.36 / 0.53 |
| thumb straightness (CMC-tip over bone length) | 0.96 / 0.97 / 0.98 | 0.90 / 0.97 / 0.99 | 0.33 / 0.39 / 0.44 |
| fingertip gap, hand-lengths | 1.46 / 1.61 / 1.77 | 0.13 / 1.46 / 2.06 | n/a |
| index reach | 1.30 / 1.32 / 1.36 | 0.57 / 1.32 / 1.37 | 0.91 / 1.24 / 1.31 |
| middle reach | 0.39 / 0.44 / 0.62 | 0.22 / 0.29 / 0.38 | unreliable (hand foreshortened) |

What they decided:

- **The measure first proposed does not work.** Thumb tip to index knuckle is
  LARGER with the thumb tucked in (it lies across the curled fingers, away from
  the index knuckle) than in the pinch-ready pose. Per the plan's rule the
  alternatives were evaluated instead.
- **Thumb out = long AND straight**, the same shape of rule as the fingers.
  Thresholds by the midpoint rule: reach above 1.05 (midway between 0.53 and
  1.55) and straightness above 0.67 (midway between 0.44 and 0.90). **Thumb in**
  = reach below 0.55 OR straightness below 0.45.
- **Pinching curls the index finger**: its reach falls to 0.57 at the pinched
  end, which the finger rule calls curled. The first design therefore left the
  index out of the stay pose. That did not survive the fist fixture; see "What
  building it changed".
- **Minimum gap to arm: 0.73 hand-lengths** (half the 5th percentile of the held
  pose).
- **A hand only opens so far.** Armed at the typical 1.6 hand-lengths the gap
  can reach about 2.06, a scale of 1.3x. Arming with the fingers closer together
  helps less than hoped: on the real clip the arm pose holds on 14 of 18 frames
  at a gap of 1.2 - 1.5 and on 2 of 10 at 0.9 - 1.2 (the index stops reading as
  extended), so about 1.7x is the most one spread gives. Growing a window a lot
  means re-arming. This is a property of hands, not a tuning problem; the README
  says so.
- The third clip was meant to be the volume pose but shows the middle finger
  curled, i.e. a second pinch-ready clip. Not needed: the recorded sessions hold
  ample volume-pose data, and volume and resize are separated by the middle
  finger (extended in one, not extended in the other).
- Pointing at the screen foreshortens the hand and the landmarks get unreliable
  (middle reach up to 4.5). The pose rules only ever see such frames as
  negatives, which is the safe direction.

### What building it changed (2026-09-17, measured)

Bad news first: the gesture is narrower than the approved design said.

- **A fist reads as a full pinch.** The recorded fist keeps its thumb "out"
  (reach 1.27, straightness 0.81) with the index curled and a fingertip gap of
  0.07; the full pinch has 1.57 / 0.95, index curled, gap 0.04. With the index
  left out of the stay pose, closing the hand shrank the window to 0.51x and
  held it. Hence: index not curled rejoins the stay pose, and release rolls
  back.
- **The cost is range.** Replayed on the real pinch-and-spread clip, the stay
  pose holds on every frame with a gap of 0.9 or more, 10 of 12 at 0.6 - 0.9,
  6 of 26 at 0.3 - 0.6 and 2 of 86 below 0.3. From a typical 1.6 start one pinch
  reaches roughly 0.4x - 0.55x, not the 0.25x clamp; further means re-arming.
- **A snap pinch does nothing, by construction.** In that clip each pinch goes
  from a gap of 1.8 to 0.2 in about 0.2 s, as fast as a hand closes. The scale
  (limited to 1 octave per second) reaches 0.85 - 0.91, the pose ends, and the
  rollback returns it to where it was: five sessions, net change nil. The clip
  was recorded with no window responding, so it says nothing about how a user
  pinches with feedback. That a deliberate, watched pinch works is a hypothesis
  until the live check. If it fails there, the candidates are a faster slew
  limit, a shorter rollback, or a measure that separates fist from pinch, which
  needs a recording of both from the same starting pose.
- **Pointing with a resting thumb armed the resize.** The thumb limits came from
  a clip of pointing AT the camera (hand foreshortened, thumb reach 0.12 -
  0.53). Seen from the front, the stock pointing-up photo has thumb reach 1.54,
  straightness 0.93 and a gap of 1.02: every rule said yes. Thumb clearance
  separates them: 0.10 / 0.18 / 0.28 on the pointing clip and 0.24 on the photo,
  against 1.00 / 1.21 / 1.33 and 0.97 / 1.20 / 1.40 on the two held clips.
  Threshold 0.62 by the midpoint rule. Frontal-view negative evidence is that
  one photo. With the rule the held clips still arm on 300 of 300 and 295 of
  299 frames, and the pointing clip on 0 of 300.
- **False arming on recordings that contain no resize:** five volume-only
  sessions and seven scripted clips, about 45,000 frames (25 minutes): 6 resize
  sessions, 2.3 s in total, scale between 0.98 and 1.12. The rollback undoes
  the short ones, but a window would visibly twitch first. Volume's own numbers
  on those recordings are unchanged.
- `gv_step` writes the volume only when the change is volume's; `changed`
  belongs to whichever gesture is armed.

### The window backend (`window_macos.c`, behind `window.h`)

- **Acquire at arming:** the frontmost layer-0 on-screen window whose owner is
  not this process (that excludes the camera preview; the HUD is not layer 0).
  Its element, original size and centre are kept for the whole gesture, even if
  the user clicks elsewhere. A window whose size is not settable is reported
  then (`WIN_ERR_NOT_RESIZABLE`), not discovered later.
- **Apply:** size = original x scale, clamped between 320x240 and the visible
  frame of the screen holding the window's centre; position rewritten so the
  centre stays put and the window stays inside that visible frame.
- **Verify:** read the size back after writing; a write that did not take is
  `WIN_ERR_IGNORED`, shown in the overlay and HUD. (CoreAudio taught this: a
  success code is not evidence.)
- **Release:** the element is released; nothing else happens.
- Status codes, `win_strerror`, last raw `AXError` for diagnosis: the same shape
  as `volume.h`. Not trusted for Accessibility -> a distinct status whose text
  tells the user which app to allow.

### Glue (`step.c`)

The injected backend grows from volume `{get, set}` to a table
`{ volume ops, window ops }`. Per frame: sync volume while not armed (as now);
update; on the frame resize arms, acquire; on `changed`, apply through the
backend of the armed kind; on release, release the target. Each backend has a
recording fake; the tests never touch a real window or the real volume.

### Python and the HUD

`bridge.py` mirrors the new structs (its size check still guards added or
removed fields; field order is verified by eye). The overlay shows
`RESIZE x1.32` and the target app's name. The HUD message gains
`"gesture": "volume" | "resize"` and `"scale"`; unknown fields are already
ignored by the HUD, so old and new stay compatible. The HUD shows a resize
glyph and a percentage for that gesture.

## Testing

- The volume refactor is judged by the existing suites staying green, replay
  results unchanged, and no behaviour test edited.
- Resize: unit tests on real fixtures lifted from the measurement clip
  (pinch-ready, pinched, volume pose, pointing, open palm); state-machine tests
  in the existing style (arm, spread, pinch, release, glitch, gap after lost
  frames, no arming from a pinched start, mutual exclusion with volume);
  mutation checks on the new logic.
- Window backend: clamping and centre arithmetic are pure functions tested
  without a window; the Accessibility calls are exercised by a small CLI
  (`win get | scale <f>`) the way `vol` exercises CoreAudio.
- `replay` learns `grow` / `shrink` expectations and prints the final scale.

## Risks and unknowns

- Fingertip gap is noisier than palm position, and the two tips occlude each
  other near the pinched end. The stay pose now lets go before that region.
- Whether a pinch made while watching the window is slow enough to register is
  untested (see "What building it changed").
- The rollback delays nothing but does mean the final size is decided 0.3 s
  before the hand visibly lets go; a user who adjusts in that last 0.3 s and
  then closes the hand loses the adjustment.
- "Frontmost window" can surprise; the overlay and HUD name the target.
- Accessibility is granted to the build session's host. Launched from VS Code's
  own terminal or debugger, the grant belongs to VS Code and may be missing.
- Some apps enforce their own minimum sizes or aspect ratios; the read-back
  check will report those as ignored writes until handled specifically.
- Rate of Accessibility writes (up to ~30 per second) is unmeasured.
- Iteration 1's known failing negative (touching the face arms the volume
  slider) is unfixed, and more gestures mean more ways to false-trigger;
  negative recordings matter more now, not less.

## Out of scope

The swipe; resizing width and height separately; moving windows; choosing among
several windows; outlining the target window before resizing it.
