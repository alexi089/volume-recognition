# Pinch to resize the front window: design

Date: 2026-09-17. Status: approved by the owner; not built. First gesture of
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
} gv_gesture;
```

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
- **Arming needs a gap of at least ~0.3 hand-lengths.** Touching fingertips
  hide each other, which is where tracking is worst, and a tiny starting gap
  makes the ratio explode.
- **Stay pose:** thumb and index not curled; the other three are not checked,
  for the same reason volume's stay pose is lenient.
- **The thumb rule is not designed yet.** The code ignores the thumb today and
  no recording of a pinch-ready hand exists. Thresholds (candidates: thumb-tip
  distance from the index knuckle over hand size; thumb straightness; angle
  between thumb and index) are set from a measurement clip, the way the finger
  thresholds were: that clip is step 1 of the plan.

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
  other near the pinched end. Unmeasured until the clip exists.
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
