# volume-recognition

Raise two fingers in front of your Mac's webcam and slide your hand left or
right to turn the system volume down or up. Drop the pose and the volume stays
where you left it. Inspired by BMW's in-car gesture control; also a vehicle for
learning C.

## Status

Works end to end in tests and against the real volume backend. **Not yet tried
with a live hand in front of the camera**, because the session that built it had
no camera permission. Specifically:

| Piece | State |
|---|---|
| C core (filter, pose, state machine, glue, trace parser) | 123 checks across 5 suites, under ASan + UBSan |
| Photo -> MediaPipe -> ctypes -> C state machine | tested (`app/test_bridge.py`) |
| C -> CoreAudio -> real system volume | verified by hand: 0.50 -> 0.64, confirmed via AppleScript |
| Live camera loop (`make run`) | **unverified**: first run will trigger the macOS camera prompt |
| Tuning constants (gain, hold times, filter) | reasoned defaults; **not tuned on a real hand** |
| Real recorded traces, especially negatives | **none yet**; the four in `traces/` are synthetic |

## Requirements

macOS on Apple Silicon, Command Line Tools (`clang`, `make`, `swiftc`), and
[`uv`](https://docs.astral.sh/uv/). Python 3.12 is installed by `uv` into `.venv`.

## Use

```sh
make setup      # venv, mediapipe, and the 7.8 MB hand-landmark model
make test       # C suites, trace replays, Python <-> C test. No camera, volume untouched
make run        # camera preview + gesture control (+ HUD if built: make -C hud)
```

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
is in `core/` and is plain C11 with no dependencies beyond CoreAudio.

- **Pose.** A finger is extended when its tip is more than 1.15x as far from
  the wrist as its middle joint. Measured on real hands: extended fingers give
  1.32 - 1.48, curled ones 0.29 - 0.70.
- **Arming.** The pose must hold for 250 ms. A passing glimpse does nothing.
- **Sliding.** Volume follows the palm centre, measured in hand-lengths from
  where it armed, so distance from the camera does not change sensitivity.
  Three hand-lengths cover the full range. A One Euro filter removes jitter at
  rest without adding much lag in motion.
- **Release.** Only frames showing the pose can move the slider, so lowering
  your hand cannot jerk the volume. The pose may drop out for 150 ms before the
  slider lets go, and a stall longer than that releases instead of jumping.
- **Honest errors.** Outputs with no software volume (HDMI / DisplayPort
  monitors) are reported as such. After every write the value is read back; a
  Bluetooth headset was observed accepting writes and ignoring them, and that
  now surfaces as an error in the overlay instead of silently doing nothing.

## Reading the C, in order

Each file's comments explain the C idioms it uses the first time they appear.

1. `core/include/volume.h`, `core/src/volume_macos.c`, `core/tools/vol.c`:
   status-code enums, `static` file scope, out-parameters, calling a C system
   API, `strtof` with an end pointer.
2. `core/tests/test.h`: what macros are for, `do { } while (0)`, `#cond`.
3. `core/src/filter.h`, `filter.c`: a struct held by value, no `malloc`.
4. `core/src/pose.h`, `pose.c`: `const` pointers, lookup tables with
   designated initialisers, `sizeof array / sizeof element`.
5. `core/include/gesture.h`, `core/src/gesture.c`: an opaque type, who owns
   heap memory, a state machine.
6. `core/include/step.h`, `core/src/step.c`, `step_system.c`: function pointers
   as dependency injection, and why the real backend lives in its own file.
7. `core/src/trace.c`, `core/tools/replay.c`: parsing text without `strtok`,
   `fgets` and over-long lines, one exit path that frees everything.
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
| `finger_ratio` | 1.15 | lower = easier to count a finger as extended |
| `arm_hold_sec` | 0.25 | longer = fewer accidental arms, slower to start |
| `release_grace_sec` | 0.15 | longer = more tolerant of lost frames, slower to let go |
| `gain` | 1/3 | volume per hand-length of travel |
| `dead_band` | 0.01 | smallest change written to the system |
| `filter_min_cutoff` | 1.0 Hz | lower = steadier at rest, laggier |
| `filter_beta` | 4.0 | higher = less lag when moving fast |

## Recording traces

```sh
make run ARGS="--record traces/real_nose_scratch.csv"
```

Then add a line to `traces/manifest.txt` with the expectation (`up`, `down`, or
`none`). `make test` replays every listed trace through the C core. The most
valuable recordings are the ones that must do **nothing**: scratching your
nose, reaching for a cup, typing, waving, talking with your hands. Tune against
those, not against the synthetic traces.

## Known limits

- RGB webcam: degrades in dim light. BMW's system uses an infrared depth camera.
- One hand, the first MediaPipe finds.
- MediaPipe 1.0.1's CPU delegate aborts on macOS; the app uses the Metal (GPU)
  delegate, which requires 4-channel RGBA frames.
- The read-back check on volume writes is best-effort; its blind spots are
  listed in `core/include/volume.h`.
- macOS only. The C core is portable; `volume_macos.c` and the HUD are not.
