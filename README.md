# volume-recognition

Raise two fingers in front of your Mac's webcam and slide your hand left or
right to turn the system volume down or up. Drop the pose and the volume stays
where you left it. Inspired by BMW's in-car gesture control; also a vehicle for
learning C.

## Status

Tried live once (7.5 minutes, recorded). That session showed the first version
was not usable: slides kept dropping out, it was too twitchy, and the real
output volume never moved. The core has since been reworked against that
recording; **the reworked version has not yet been tried live.**

| Piece | State |
|---|---|
| C core (filter, pose, state machine, glue, trace parser) | 229 checks across 5 suites, under ASan + UBSan; pose tests mutation-checked |
| Photo -> MediaPipe -> ctypes -> C state machine | tested (`app/test_bridge.py`) |
| C -> CoreAudio -> real system volume | works on built-in speakers, and on AirPods in some states; see Known limits |
| Live camera loop (`make run`) | two live sessions. The second crashed after 8 minutes (MediaPipe GPU buffer leak); fixed by moving to the CPU delegate, soak-tested for 15,000 frames with a flat 116 MB footprint. Live frame rate with the preview on the CPU delegate: 29.5 fps |
| Dropping out mid-slide | on the recording: 69 -> 32 armed sessions, median 0.9 -> 2.4 s, under one second 40 -> 9. Proxy numbers: the recording has no intent labels |
| Tuning constants | reasoned from one recording of one hand; **not yet tuned on scripted recordings** |
| Real negative traces (typing, nose scratch, ...) | **none yet**; the four pass/fail traces in `traces/` are synthetic |

## Requirements

macOS on Apple Silicon, Command Line Tools (`clang`, `make`, `swiftc`), and
[`uv`](https://docs.astral.sh/uv/). Python 3.12 is installed by `uv` into `.venv`.

## Use

```sh
make setup      # venv, mediapipe, and the 7.8 MB hand-landmark model
make test       # C suites, trace replays, Python <-> C test, memory-growth test. No camera, volume untouched
make run        # camera preview + gesture control + the glass HUD (top-left)
```

`make run ARGS="--hud-opacity 0.4"` makes the HUD's glass clearer (0.15 - 1.0,
default 0.6). The HUD appears at the top-left of whichever display the mouse
pointer is on; `--hud-screen primary` pins it to the laptop's own display.
`--no-preview` hides the camera window and leaves only the HUD;
`--no-hud` does the opposite.

In the preview: grey = idle, amber = arming, green = armed. Hold two fingers up
(index and middle; ring and pinky curled) for a quarter of a second to arm,
slide, then close your hand or lower it to release. `q` or `Esc` quits.

If the camera cannot be opened, allow camera access for the app hosting your
terminal (VS Code or Terminal) in System Settings > Privacy & Security > Camera,
then restart that app.

## How it works

```
camera -> [Python] mirror -> MediaPipe -> 21 landmarks + timestamp
       -> [C] gv_step_system:  read system volume -> state machine -> set volume
       -> [Python] preview overlay, and one JSON line per frame to the HUD
```

Python is a pipe; it holds no gesture logic. Everything that decides anything
is in `core/` and is plain C11 with no dependencies beyond libm and CoreAudio.

- **Judging a finger.** Two measurements, because each alone is fooled by
  something real. *Reach* (tip vs middle joint, distance from the wrist) is
  fooled by a relaxed curl, which reads 1.1 - 1.17 against an extended finger's
  1.2 - 1.5. *Straightness* (knuckle-to-tip over bone length) is fooled by a
  finger curled towards the camera, which projects short but straight. So a
  finger is **extended** when it is long AND straight, **curled** when it is
  short OR bent, and in between is a band that nothing depends on.
- **Arming is strict.** Index and middle extended, ring and pinky *not*
  extended, held for 250 ms. An open palm, a pointing finger, a fist and a
  passing glimpse of the pose all do nothing.
- **Staying armed is lenient.** Only "index and middle have not closed". A
  relaxed hand's ring and pinky wander in and out of looking extended; demanding
  the full pose on every frame is what made the first version drop out.
- **Sliding, sideways or up and down.** Right and up are louder. The two are
  not added together: measured on real slides, a sideways sweep wanders 0.7 -
  1.6 hand-lengths vertically (an arm swings in an arc), which added in would
  be 13 - 32% of unasked-for volume. The first movement of at least half a
  hand-length that is at least twice the travel on the other axis picks the
  axis for that slide; a diagonal picks nothing. Travel is counted from that
  half hand-length, so there is no jump and small adjustments stay possible.
  Vertical travel counts 1.78x (the aspect ratio), because the picture has that
  much less room top to bottom. Travel is measured in hand-lengths, so distance
  from the camera does not change sensitivity; a One Euro filter removes jitter
  at rest without adding much lag in motion.
- **Precise when slow, quick when fast.** One fixed gain failed both ways live:
  1/3 per hand-length was too twitchy to set a level, 0.2 too slow to get
  anywhere. So the gain follows the hand's speed, like a mouse pointer: 0.15 per
  hand-length at or below 1 hand-length/s (about seven hand-lengths for the
  full range), rising to 0.6 at 5/s (under two). The price: out fast and back
  slow is a net change. It is a relative control, not a physical slider.
- **Running out of picture.** At a desk the picture is about 8 hand-lengths wide
  and 4.6 tall, and the hand is two tall, so a slide reaches the edge long
  before the volume reaches the end. Hold the hand at the edge the slide was
  heading for and the volume keeps moving at 0.35 per second.
- **Release.** Close your hand or take it out of frame. Only good frames can
  move the slider, so lowering your hand cannot jerk the volume. Good frames may
  be missing for 250 ms before the slider lets go, and a stall longer than that
  releases instead of jumping.
- **Frames that cannot be true are ignored.** While armed, a frame is believed
  only if the hand is 0.7x - 1.4x the size it was in the last believed frame
  and has moved less than 30 hand-lengths/s in the plane. That ends a slide when
  the detector jumps to a different hand (it took the volume from 78% to 0% in
  one frame in the first recording) instead of following it.
- **The volume cannot jump.** It moves at most 2.0 per second, and lost frames
  earn credit for at most 0.1 s, so no single step exceeds 0.2 whatever the
  detector does. With headphones on, this is the safety net.
- **Honest errors.** Outputs with no software volume (HDMI / DisplayPort
  monitors) are reported as such. After every write the value is read back, and
  a write that did not take shows as a red banner across the preview.

## Reading the C, in order

Each file's comments explain the C idioms it uses the first time they appear.

1. `core/include/volume.h`, `core/src/volume_macos.c`, `core/tools/vol.c`:
   status-code enums, `static` file scope, out-parameters, calling a C system
   API, `strtof` with an end pointer.
2. `core/tests/test.h`: what macros are for, `do { } while (0)`, `#cond`.
3. `core/src/filter.h`, `filter.c`: a struct held by value, no `malloc`.
4. `core/src/pose.h`, `pose.c`: `const` pointers, lookup tables with
   designated initialisers, `sizeof array / sizeof element`, guarding a
   division.
5. `core/include/gesture.h`, `core/src/gesture.c`: an opaque type, who owns
   heap memory, a state machine.
6. `core/include/step.h`, `core/src/step.c`, `step_system.c`: function pointers
   as dependency injection, and why the real backend lives in its own file.
7. `core/src/trace.c`, `core/tools/replay.c`: parsing text without `strtok`,
   `fgets` and over-long lines, a `realloc`-grown array, a `qsort` comparator,
   one exit path that frees everything.
8. `app/bridge.py`: the C ABI seen from outside, and the struct-size check
   that guards it.

Tests build with `-fsanitize=address,undefined`. Try breaking something on
purpose (read `lm[21]`, skip the `gv_destroy`) and run `make -C core test` to
see what the sanitizers report.

In VS Code: install the recommended extensions when prompted (CodeLLDB for
debugging, clangd for C IntelliSense), then use the Run and Debug panel; the
"C: state machine tests" configuration is a good place to set a breakpoint in
`gv_update` and step through an arm / slide / release cycle.

## Tuning

All constants are in `gv_default_config()` in `core/src/gesture.c`:

| Field | Default | Effect |
|---|---|---|
| `pose.extend_reach` / `extend_straight` | 1.20 / 0.93 | both must be exceeded to call a finger extended |
| `pose.curl_reach` / `curl_straight` | 1.00 / 0.75 | falling below either calls it curled |
| `arm_hold_sec` | 0.25 | longer = fewer accidental arms, slower to start |
| `release_grace_sec` | 0.25 | longer = more tolerant of lost frames, slower to let go |
| `gain` / `gain_fast` | 0.15 / 0.6 | volume per hand-length when slow / when fast; equal = a plain fixed gain |
| `accel_speed_lo` / `accel_speed_hi` | 1.0 / 5.0 | hand-lengths per second where the gain starts and stops rising |
| `axis_lock_hands` / `axis_dominance` | 0.5 / 2.0 | travel that picks an axis, and how far ahead of the other axis it must be |
| `edge_margin` / `edge_rate` | 0.04 / 0.35 | how close to the picture's edge counts, and volume per second while held there (0 = off) |
| `dead_band` | 0.01 | smallest change written to the system |
| `max_slew` | 2.0 /s | fastest the volume may move |
| `gate_scale_min` / `max` | 0.7 / 1.4 | believable frame-to-frame change in hand size |
| `gate_max_speed` | 30 | believable palm speed, hand-lengths per second |
| `filter_min_cutoff` | 1.0 Hz | lower = steadier at rest, laggier |
| `filter_beta` | 4.0 | higher = less lag when moving fast |

Four of them can be tried live without rebuilding:

```sh
make run ARGS="--gain 0.12 --gain-fast 0.8 --arm-hold 0.3 --grace 0.3"
```

To see what a change does to a recording before trying it on a hand:

```sh
make -C core build/replay && core/build/replay traces/real_first_session.csv stats
```

## Recording traces

```sh
make run ARGS="--record traces/real_nose_scratch.csv"
```

This writes two files: `real_nose_scratch.csv` (what the camera saw: every
frame's landmarks, replayable through the C core) and
`real_nose_scratch.events.csv` (what the app did: state, target volume and the
status of each volume write).

Then add a line to `traces/manifest.txt` with the expectation (`up`, `down`, or
`none`). `make test` replays every listed trace. What is needed next, about
45 s each:

- positives: `real_slide_up`, `real_slide_down` (arm, slide slowly, close your
  hand; five times);
- negatives, which matter most: `real_typing`, `real_nose_scratch`,
  `real_cup_reach`, `real_waving`, `real_talking_hands`. Each must produce
  **zero** volume change.

Thresholds get tuned to minimise dropped slides on the positives *subject to*
zero changes on every negative. `traces/real_first_session.csv` is the first
live session: no intent labels, so it is a tuning corpus (`stats` only).

## Known limits

- **AirPods sometimes ignore volume writes.** In that state the virtual main
  volume, the per-channel volume and AppleScript's `set volume` all return
  success and change nothing, with or without audio playing, with or without
  this app running. The same AirPods accepted writes an hour earlier. The cause
  is not yet known (in-ear detection is the leading guess, unverified). There
  is no software workaround; the app shows a red banner.
- RGB webcam: degrades in dim light. BMW's system uses an infrared depth camera.
- One hand, the first MediaPipe finds.
- MediaPipe is pinned to 0.10.35 and runs on the CPU delegate (4.5 ms a frame).
  On macOS the GPU (Metal) delegate keeps about one pixel buffer per frame until
  the landmarker closes, in 0.10.35 and 1.0.1 alike; the second live session
  crashed after eight minutes because of it. 1.0.1's CPU delegate aborts at
  start-up, hence the pin. `app/test_leak.py` guards this; run it before moving
  the pin.
- MediaPipe 0.10.35 contains a Google usage-telemetry uploader ("clearcut"); it
  logs a failed upload attempt at exit. Whether it can be disabled has not been
  looked into.
- The read-back check on volume writes is best-effort; its blind spots are
  listed in `core/include/volume.h`.
- The HUD (`hud/`, Swift + AppKit) is glass-style, built on `NSVisualEffectView`.
  It is not Apple's Liquid Glass, which needs macOS 26 and its SDK. Nobody who
  built it could see the screen; its logic is tested (74 checks) but its looks
  were judged only by the owner. See `hud/README.md`.
- macOS only. The C core is portable; `volume_macos.c` and the HUD are not.
