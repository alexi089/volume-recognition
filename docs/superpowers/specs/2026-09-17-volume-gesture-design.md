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

## Behaviour

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

- **The live camera loop has never been run.** The session that built this had
  no camera permission (`OpenCV: not authorized to capture video`); it has to be
  launched from VS Code's or Terminal's own shell.
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
