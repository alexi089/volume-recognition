# Air-gesture volume control: design

Date: 2026-09-17. Status: approved, in build.

## Goal

Raise two fingers in front of the Mac's webcam and move the hand left/right to
turn the system volume down/up. Inspired by BMW's in-car gesture control.
Secondary goal: learn C by writing the logic we own in C.

## Decisions

- **Platform:** macOS on Apple Silicon, built-in webcam. Apple clang + `make`,
  Command Line Tools only (no Xcode, no cmake).
- **Interaction:** continuous slider. Two fingers up = armed; volume tracks the
  hand's horizontal position relative to where it was armed; dropping the pose
  releases and the volume stays put. A slider has no return stroke, so the
  "swipe back looks like the opposite swipe" problem does not exist.
- **Split:** Python + MediaPipe for camera and hand landmarks; all gesture logic
  and the volume call live in a C library loaded through `ctypes`.

C does not make recognition faster or more accurate: the neural net is native
code inside MediaPipe and the webcam caps the loop at 30 fps. C is used because
the logic is a good C exercise and because CoreAudio is a C API.

## Architecture

```
camera frame -> [Python] flip to selfie view -> MediaPipe -> 21 landmarks + timestamp
             -> [C] gv_step_system(frame): vol_get -> gv_update -> vol_set if changed
             -> [Python] draw overlay: landmarks, state, volume bar
```

**Python is a pipe. No gesture logic in Python.** The C core takes landmarks in
and gives state + target volume out. It has no side effects and no knowledge of
where landmarks came from, so it can be tested from recorded files and the
front end can be swapped later without touching it.

## C API (core/include/gesture.h)

```c
typedef struct { float x, y, z; } gv_point;
typedef struct { gv_point lm[21]; int present; double t_sec; } gv_frame;
typedef enum { GV_IDLE, GV_ARMING, GV_ARMED } gv_state;
typedef struct { gv_state state; float volume; int changed; } gv_output;

gv_ctx   *gv_create(const gv_config *cfg);
void      gv_destroy(gv_ctx *ctx);
void      gv_set_current_volume(gv_ctx *ctx, float v);
gv_output gv_update(gv_ctx *ctx, const gv_frame *f);

int vol_get(float *out);   /* 0 ok, <0 error incl. "output has no software volume" */
int vol_set(float v);
```

## Behaviour (first design; superseded by "Round 2" below after the live session)

- **Finger extended:** `dist(tip, wrist) > k * dist(pip, wrist)`. Independent of
  hand rotation and image axes.
- **Armed pose:** index and middle extended, ring and pinky curled, thumb ignored.
- **Arming:** pose held continuously ~250 ms. Anchor = filtered palm x at that
  moment; anchor volume = current system volume.
- **Disarming:** pose or hand lost for ~150 ms (hysteresis, so one bad frame does
  not drop the slider). Volume stays.
- **Tracked point:** palm centre (mean of wrist + four MCP joints); steadier than
  fingertips.
- **Mapping:** `dx = (x - x_anchor) / hand_scale`, where
  `hand_scale = dist(wrist, middle MCP)`, so distance from the camera does not
  change sensitivity. `volume = clamp(anchor_volume + gain * dx, 0, 1)`.
- **Smoothing:** One Euro filter on palm x: heavy smoothing when the hand is
  still (kills jitter), light smoothing when it moves fast (limits lag).
- **Dead-band:** report `changed` only when the target moves >= 1%.
- **Mirroring:** Python flips the frame before MediaPipe, so the user's
  right = +x = louder and the preview reads like a mirror.

## Findings from the first spike (2026-09-17)

- MediaPipe 1.0.1 keeps the Tasks API (`vision.HandLandmarker`, modes IMAGE /
  VIDEO / LIVE_STREAM). The legacy `mp.solutions` module, including its drawing
  helpers, is gone; the overlay is drawn with OpenCV directly.
- On this Mac the **CPU delegate aborts** inside MediaPipe
  (`DrishtiMetalHelper ... Service is unavailable`, raised from
  `TensorsToDetectionsCalculator::Open`). The **GPU (Metal) delegate works**,
  but only with 4-channel `SRGBA` images; 3-channel `SRGB` aborts with
  `unsupported ImageFrame format: 1`. So: GPU delegate, BGR -> RGBA conversion.
- Blank 1280x720 frame: 2.2 ms/frame on the GPU delegate. That is the palm
  detector alone; frames containing a hand also run the landmark model and cost
  more. Not yet measured live.
- Python 3.12 venv via `uv`; system default 3.14 was not tried.
- `mediapipe` depends on `opencv-contrib-python`. Installing `opencv-python`
  alongside it makes two packages fight over the `cv2` module.

## Findings from the build (2026-09-17)

- **Outputs without software volume, confirmed on hardware:** a DisplayPort
  monitor exposes no virtual main volume property at all; reported as
  `VOL_ERR_NO_SOFTWARE_VOLUME`.
- **A failure mode the design did not anticipate:** a Bluetooth headset
  advertised a settable volume, returned `noErr` on every write, and changed
  nothing; AppleScript's `set volume` was ignored the same way, while the same
  write on the built-in speakers applied. Later in the day the same headset
  accepted writes normally, so it is a transient device state, not a property
  of the device. `vol_set` now reads the value back and returns
  `VOL_ERR_IGNORED` when it differs by more than 0.08. Best-effort; its blind
  spots are listed in `core/include/volume.h`.
- **Aspect ratio had to become an input.** Landmark x and y are normalised by
  width and height separately, so they are different physical units on a 16:9
  frame. Every distance in `pose.c` takes the frame aspect; without it a hand's
  measured size changes as it rotates.
- **The finger threshold is measured, not guessed:** extended fingers give a
  tip/PIP distance ratio of 1.32 - 1.48 on the fixture photos, curled ones
  0.29 - 0.70; the threshold is 1.15.
- **A bug the tests caught before it shipped:** the dead-band swallowed the last
  step, so the slider could not reach exactly 0 or 1. The ends of the range
  bypass it.
- **End-to-end on the real backend:** fixture landmarks slid 0.45 hand-lengths
  through `gv_step_system` moved the system volume 0.50 -> 0.64, confirmed
  independently through AppleScript.

## Findings from the first live session (2026-09-17)

7.5 minutes, 9,556 frames, recorded to `traces/real_first_session.csv`.

- **The real volume never moved.** The AirPods sat at 0.50 before and after.
  Controlled experiments on the AirPods in that state, each restoring the
  volume afterwards:
  - app not running, no audio: virtual main volume write returns `noErr`,
    read-back unchanged. So the camera app is not the cause.
  - 7 s of silence streaming, `DeviceIsRunningSomewhere` = 1 throughout, a
    write every 0.5 s: all ignored. So "no active stream" is not the cause.
    (An earlier version of this test waited only 1.5 s and proved nothing.)
  - per-channel `kAudioDevicePropertyVolumeScalar` on elements 1 and 2:
    `noErr`, unchanged. There is no alternative software write path.
  - AppleScript `set volume`: ignored as well.
  The same code moved the same AirPods 0.50 -> 0.64 an hour earlier. The
  distinguishing condition is not yet known; in-ear detection is the leading
  guess and is unverified. Outcome in code: a full-width red banner, and a
  per-frame `<name>.events.csv` beside every recording with the volume status.
- **The slider kept letting go.** 69 armed sessions, median 0.9 s, 40 under 1 s.
  58 of 69 ended on pose flicker (40 ring/pinky read as extended, 18
  index/middle read as curled), 11 on the hand leaving. The 1.15 threshold came
  from photos of tight fists (curled = 0.29 - 0.70); a relaxed real hand's
  curled ring/pinky read 0.74 median and up to 1.17, a continuum.
- **Glitch frames.** 1.9% of armed frames had a hand size below 0.6x or above
  1.6x of normal; one moved the volume 0.57 in a single frame.
- **Too sensitive.** Full range was about a third of the frame width.
- **21 fps** with the 1080p preview against 27 fps headless; MediaPipe itself
  takes 7.6 ms per frame.
- Landmark jitter is small: the stillest second while armed had a palm-x
  standard deviation worth 0.39% of volume, unfiltered.

## Round 2: what changed after the live session (2026-09-17)

Owner's decision on release: strict two-finger pose to arm; once armed, stay
armed while index and middle remain extended; release by closing the hand or
leaving the frame.

- **Two measurements per finger.** Reach (tip vs PIP distance from the wrist)
  is fooled by a relaxed curl; straightness (MCP-to-tip over bone length) is
  fooled by a finger curled towards the camera, which projects short but
  straight (the thumbs-up photo's ring finger reads 0.93). MediaPipe's z does
  not rescue it: 3D straightness on the same finger is still 0.89. Extended =
  reach > 1.20 AND straightness > 0.93; curled = reach < 1.00 OR straightness
  < 0.75; between them is an ambiguous band.
- **Strict to arm, lenient to stay.** Arm: index and middle extended, ring and
  pinky not extended. Stay: index and middle not curled. No per-finger memory.
  A prototype showed per-finger hysteresis with the old stay rule changed
  nothing (69 -> 69 sessions); the stay rule is what matters.
- **Plausibility gate while armed.** Hand size within 0.7x - 1.4x of the last
  believed frame (real: 0.75 - 1.26 at the 0.5th / 99.5th percentile; glitches:
  0.01 and 10.8), and movement in the plane under 30 hand-lengths/s from the
  last believed frame (real sweeps: 12.5 at p99, 28 at p99.9). Two earlier
  versions were wrong and the recording showed it: gating size against the
  arming moment nearly doubled dropped slides (41 sessions against 23 ungated),
  because a hand tilts and drifts over seconds; and a 15/s limit rejected real
  sweeps, which then caught up in one jump.
- **Slew limit.** 2.0 per second, with at most 0.1 s of credit across a gap, so
  no single step exceeds 0.2. Without the cap, a 0.23 s gap earned a 0.46 jump.
- **Gain** 1/3 -> 0.2 (superseded the same day by speed-dependent gain, see
  "Round 3" below); **release grace** 150 -> 250 ms; **capture** 1280x720
  with a 960 px preview (22.1 -> 28.7 fps with the preview, no hand in view).
- **A naive target, corrected.** The plan asked for "no single-frame volume
  step above 0.05". At 21 fps a real 12 hand-lengths/s sweep legitimately moves
  about 0.1 per frame. The honest bound is the slew limit's 0.2.

Result on the recording, old core against new: 69 -> 32 armed sessions, median
0.9 -> 2.4 s, under one second 40 -> 9, biggest single-frame step 0.673 ->
0.200. These are proxies: the recording has no intent labels, and a longer
session is only probably a better one. Scripted recordings with known intent,
negatives above all, are the real test and do not exist yet.

Considered and held back: MediaPipe's trained GestureRecognizer (Victory /
Closed_Fist / Open_Palm). It may well beat hand-written geometry. It would move
the key decision out of the C this project exists to teach, and "Victory" is a
spread V rather than two fingers up. It is the fallback if scripted recordings
cannot reach clean positives with zero negative triggers.

## Round 3: up/down, speed-dependent gain, the edge of the picture (2026-09-17)

From the second and third live sessions ("make it faster", "up and down should
work", "our fingers are lost ... volume only moves so much").

- **Axis lock, not axis sum.** A slide runs left/right or up/down (right and up
  are louder). The first movement of at least 0.5 hand-lengths that is at least
  twice the travel on the other axis picks the axis for that slide; a diagonal
  picks nothing. The two are not added: recorded sideways slides wander 0.7 -
  1.6 hand-lengths vertically. Travel is counted from the signed lock distance,
  not from where the hand is on the frame that settles it (frames are discrete
  and a fast move can be far past the line; using the position threw travel
  away). Simulated and not adopted: requiring the hand to be still before
  arming; it made arming markedly harder for an unclear gain. Known cost: in an
  up/down slide, lowering the hand to leave lowers the volume until the pose
  ends; closing the hand first avoids it.
- **Speed-dependent gain.** The slider is an accumulator: each believed frame
  adds gain(speed) x travel since the last one, gain rising linearly from 0.15 at
  1 hand-length/s to 0.6 at 5 (recorded median armed speed 1.3, 90th percentile
  6.1). Clamped as it accumulates, so pushing past an end builds no dead zone.
  With the two gains equal it reduces to the old position mapping, which is how
  the tests pin it. The price, tested and stated: out fast and back slow is a net
  change.
- **Vertical travel x aspect.** The third session's picture measured 8.1 by 4.6
  hand-lengths with a hand two tall; vertical travel counts 1.78x so the same
  fraction of the picture gives the same change on either axis.
- **Edge push.** In that session a slide moved the volume 0.18 at the median and
  4 of 12 ended at an edge. Within 4% of the edge the slide is heading for, the
  volume keeps moving at 0.35/s. Sideways the furthest landmark counts; upwards
  the two raised fingertips; downwards the wrist. "Heading for" is judged by
  travel, because a raised hand's wrist is often at or below the bottom of the
  picture and sliding up from there must not be dragged down. Credit per frame
  is capped like the slew limit's.
- **HUD.** Top-left minimal glass pill (owner's spec), `--opacity` because nobody
  but the owner can see it, and the display chosen deliberately: `NSScreen.main`
  is the screen with keyboard focus, not the main display, and had put the pill
  in the far corner of a 5120-wide second monitor.

A third review (13 commits) found no logic defects and these documentation
errors, since fixed: the README still described the fixed 0.2 gain and
"five hand-lengths", and left the three features above undocumented.

## The crash in the second live session (2026-09-17)

After 476 s (11,330 frames) the app aborted inside MediaPipe:
`CVPixelBufferCreate` returned -6662 (`kCVReturnAllocationFailed`) in
`ConvertFromImageFrame`, reached from `ImageCloneCalculator`. Investigation:

- Reproduced headlessly with synthetic 1080p frames: IOSurface regions grew by
  about two per frame (4,068 after 2,000 frames; footprint 469 MB -> 31 GB ->
  62 GB). At 720p with a hand in view, about one per frame.
- `mp.Image` frees itself in `__del__`, so the references are not ours.
- Running mode makes no difference: VIDEO, IMAGE and LIVE_STREAM all retain the
  same number.
- The surfaces are released the moment the landmarker closes (1,021 regions ->
  21, three cycles running). So it is an unbounded cache inside the graph, not
  memory lost for good.
- MediaPipe 0.10.35's GPU delegate does exactly the same (5.8 GB after 1,500
  frames). The leak belongs to the macOS GPU path, not to 1.0.1.
- 0.10.35's CPU delegate works here: 4.5 ms a frame against 2.8 on the GPU,
  footprint flat at 118 MB, no IOSurfaces, hand found in 1,500 of 1,500 frames
  (GPU: 1,477). 1.0.1's CPU delegate aborts at start-up, which is what had
  forced the GPU delegate on day one.

Fix: pin `mediapipe==0.10.35`, CPU delegate, plain RGB frames; every MediaPipe
call goes through `detect()` / `detect_photo()` in `app/main.py`.
`app/test_leak.py` runs that path for 400 frames and fails if the footprint
grows by more than 100 MB (it grew 1,417 MB before the fix and 0 - 1 MB after);
a 15,000-frame soak held 115 - 118 MB. Recycling the landmarker periodically
was considered and rejected: it would reset hand tracking mid-slide.

The crash went unnoticed for several minutes because the launch command ended
in `| grep ...; echo`, so the task reported exit code 0. Launch commands now use
`set -o pipefail`.

## Changes from the plan

- `vol_*` lives in its own `volume.h` rather than in `gesture.h`, keeping the
  gesture header portable.
- Added `step.c`: the per-frame glue (read volume, update, apply) is C too, with
  the volume backend injected as function pointers so it is testable with a
  fake. Python makes one call per frame.
- `replay.c` is a tool under `core/tools/` driven by `traces/manifest.txt`,
  not a test file.
- Added after the plan, at the owner's request: a separate native glass-style
  HUD process (`hud/`, Swift + `NSVisualEffectView`) fed JSON lines by Python.
  Apple's Liquid Glass APIs need macOS 26 and its SDK; this machine has macOS
  15.1 and Command Line Tools only, so the HUD uses the older native material.

## Still unverified

- **The reworked core has not been tried live.** Everything in "Round 2" was
  measured by replaying the first session's recording.
- Landmark jitter at arm's length in room light, and with it every tuning
  constant: gain, hold and grace times, filter parameters.
- No real recorded traces exist yet. The four in `traces/` are synthetic: real
  hand shapes, scripted motion, no jitter.

## Testing

- C unit tests (plain C, no framework) for filter, pose, and state machine,
  built with `-Wall -Wextra -Werror -fsanitize=address,undefined`.
- Trace replay: `main.py --record` writes landmark sessions to CSV;
  `tools/replay.c` feeds them to `gv_update`. Negative traces (nose scratch,
  reaching for a cup, typing, waving) must produce zero volume change; positive
  traces must move the volume in the right direction.
- Still-image fixtures in `traces/fixtures/` (MediaPipe's published
  victory / thumbs_up / pointing_up photos) check the pose classifier against
  real landmarks.

## Out of scope for v1

Discrete or velocity-scaled swipes, other gestures, menu-bar packaging,
low-light/IR operation, Raspberry Pi or in-car hardware, an Apple Vision front
end.
