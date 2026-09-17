# volume-hud

A tiny, display-only **glass-style** heads-up display for a gesture
volume-control project. It is a separate native macOS program that the project's
Python process launches and feeds one JSON message per line on stdin. It shows a
compact glass **pill** in the top-left of the screen with a speaker glyph, a slim
level bar, and a percentage. It contains no gesture, audio, or camera logic.

> "Glass-style", never "Liquid Glass". This is built on the native
> `NSVisualEffectView` material available in this SDK, not Apple's macOS 26
> Liquid Glass APIs (which do not exist here). See "On macOS 26" below.

## What it looks like

- A small rounded pill (continuous corners, a hairline border that reads in both
  light and dark, and a soft drop shadow), positioned near the **top-left** of
  the main screen's visible area (about 18 pt in from the top and left, below the
  menu bar).
- Normal content is minimal: speaker glyph, slim level bar, percentage.
- **Axis hint:** while armed with no decided direction (`axis` null) the bar is
  shown at reduced emphasis (waiting for a direction). Once the direction is
  known (`axis` "x" or "y") the bar goes to full emphasis and a tiny directional
  arrow (`arrow.left.and.right` / `arrow.up.and.down`) appears next to the
  percentage. The bar is never rotated. The arrow sits in a fixed-width slot, so
  it never changes the pill's size.
- **Error:** when `error` is non-null the glyph changes shape (a
  speaker-with-exclamation-badge symbol), tints with the system warning colour,
  and a compact second line shows the message; the pill grows to fit it. Colour
  is never the only signal. Clearing the error removes the line and restores the
  speaker glyph.
- Adapts to light/dark (semantic colours + the visual-effect material). Respects
  **Reduce Motion** (no pulse, instant transitions) and **Reduce Transparency**
  (`NSVisualEffectView` becomes more opaque automatically).

## Options

```
volume-hud [--opacity 0.15-1.0] [--screen pointer|primary]
```

`--screen` picks the display. `pointer` (default): the display the mouse pointer
is on at the moment the HUD appears, re-chosen every time it appears. `primary`:
always the display with the menu bar. The first version used `NSScreen.main`,
which is not "the main display" but "the screen with keyboard focus"; on a
laptop plus an ultrawide it put the pill in the far corner of the ultrawide
while its owner watched the laptop. Placement was verified by asking the window
server for the window's bounds in both modes (18 pt from the left; 18 pt below
the top, or below the menu bar where there is one). From the app:
`make run ARGS="--hud-screen primary"`.

`--opacity` is how much of the blur material shows: 1.0 is the stock
`.hudWindow` look, lower lets more of what is behind the pill through and reads
as clearer glass. Default 0.6. Out-of-range values are clamped; a non-numeric
value, a missing value or an unknown option produces one warning on stderr and
the HUD starts anyway. Only the material fades: the glyph, bar and number sit
above the glass, not inside it. A soft white gradient over the top half gives
the edge-catching-light look. With the system's "Reduce transparency" on, the
material is solid and the highlight is off, whatever `--opacity` says. From the
app: `make run ARGS="--hud-opacity 0.4"`.

## stdin contract (as implemented)

One JSON **object** per line, UTF-8, newline-terminated. Flush after each line
(Python: `print(json.dumps(msg), flush=True)`). The real producer sends one line
every camera frame (~28-30/s), including a continuous stream while idle.

    {"state":"idle","volume":0.50,"error":null}
    {"state":"arming","volume":0.50,"axis":null,"error":null}
    {"state":"armed","volume":0.62,"axis":"x","error":null}
    {"state":"armed","volume":0.62,"axis":"x","error":"the output accepted the volume change but did not apply it"}

| field   | required | type / rule |
|---------|----------|-------------|
| state   | yes      | "idle" \| "arming" \| "armed". Missing / other type / unknown value → line rejected. |
| volume  | yes      | A **finite** JSON number, clamped into 0..1. Booleans, strings, and non-finite values → line rejected. |
| error   | no       | JSON null or string. Absent, null, or a string that is **empty after trimming** → no error. Other types → line rejected. |
| axis    | no       | "x" \| "y". Absent, null, or anything else (bad string, number, …) → treated as null; the line is **not** rejected. |

Details verified against this SDK's `JSONSerialization`:

- **Duplicate keys:** the **first** occurrence wins.
- **Overflow:** a positive exponent overflow such as `1e400` is rejected by the
  JSON reader (malformed). A negative overflow such as `-1e400` parses to `-inf`;
  because a non-finite volume is not usable it is likewise rejected. A huge but
  finite integer (e.g. `1e23`) is accepted and clamped to 1.0.
- **Glyph vs 0%:** the muted (slash) glyph is used exactly when the displayed
  percentage rounds to 0% (so `0.004` → "0%" shows the slash glyph, consistently).
- Unknown extra fields are ignored.

Rejected lines are ignored with one diagnostic to stderr and never crash the
program; empty/whitespace lines are skipped silently.

## Lifecycle and signals

- **Hidden** while idle. Fades in on "arming" with a subtle glyph pulse; the
  pulse stops and it settles on "armed". After returning to "idle" it lingers ~1s
  (so the final volume stays readable) then fades out.
- The hide is armed on the **transition** into idle and is **not** reset by the
  flood of subsequent idle lines; re-gripping within the linger cancels it and
  stays visible. This logic lives in a pure, testable reducer
  (`Sources/VisibilityModel.swift`) so these paths are covered by tests rather
  than only by eye.
- Borderless, floating, non-activating panel: never takes focus, ignores mouse
  events, shows on all Spaces and over full-screen apps, no Dock icon or menu bar
  (activation policy `.accessory`).
- **Quitting:** EOF on stdin (parent closed the pipe or the parent process died),
  or **SIGINT** / **SIGTERM**, all cause a clean exit (code 0). With no Dock icon
  a stuck process would be invisible, so the signal handlers matter.

## Build / test / demo

Command Line Tools only — no Xcode, no SwiftPM, no XCTest. Uses `/usr/bin/swiftc`.

    make -C hud all     # build build/volume-hud
    make -C hud test    # build + run the parser & reducer tests (build/test_message)
    make -C hud demo    # stream ~30 lines/s (the real producer's cadence) into the HUD
    make -C hud clean

`make demo` needs a logged-in GUI session to draw the window. It has a `trap`
that kills the HUD it started.

### Toolchain constraint on this machine (CLT 16.2) and the SwiftBridging workaround

This machine's Command Line Tools 16.2 install is internally inconsistent: the
compiler (swiftlang-6.0.3.1.10) is newer than the bundled SDKs, and
`usr/include/swift/` ships **both** a stale `module.modulemap` (Aug 2023) and
`bridging.modulemap` (Dec 2024), each defining `module SwiftBridging`. Importing
Foundation/AppKit therefore fails with `redefinition of module 'SwiftBridging'`,
which then cascades into a misleading `this SDK is not supported by the compiler`
error.

The proper fix is to remove the stale `module.modulemap`, but it is a root-owned
system file. The Makefile instead applies a **VFS overlay** (`-vfsoverlay`) that
maps only that one stale file to an empty modulemap:

- It touches **no** system file (the overlay + empty modulemap are generated in
  `hud/build/`).
- It is applied **only** when the duplicate is actually present, so it is a
  **no-op on a healthy toolchain**.
- The real fix is to **repair the Command Line Tools install** (reinstall CLT, or
  install a matching Swift toolchain). Remove the workaround once that is done.

## On macOS 26 (Liquid Glass)

This HUD uses `NSVisualEffectView` (material `.hudWindow`, `.behindWindow`,
`.active`) with a continuous-corner rounded shape, a hairline border, and a soft
custom layer shadow. On macOS 26, the glass pill could instead adopt Apple's
Liquid Glass (e.g. `NSGlassEffectView` / SwiftUI `glassEffect`), which would
supply the material, shape, edge highlight and shadow as one system-provided
effect — the border and hand-rolled shadow here would largely go away. Those APIs
do not exist in this SDK and cannot run on this OS, so they are deliberately not
referenced, and the result is never called "Liquid Glass".

## Structure

- `Sources/HudMessage.swift` — the stdin contract + parser/validator. Foundation
  only, no AppKit; the data boundary.
- `Sources/HudOptions.swift` — command-line options (`--opacity`, `--screen`). Foundation only.
- `Sources/ScreenChooser.swift` — which display to appear on. Pure geometry.
- `Sources/VisibilityModel.swift` — pure reducer for the show/fade/linger/pulse
  lifecycle. Foundation only, no AppKit; the animation state machine.
- `Sources/LevelBarView.swift`, `HudContentView.swift`, `HudPanel.swift` — the UI.
- `Sources/StdinReader.swift` — background-thread line reader.
- `Sources/MessageCoalescer.swift` — collapses a burst to one main-thread apply.
- `Sources/AppController.swift` — executes reducer commands; diffs content.
- `Sources/main.swift` — entry point; `.accessory` policy; signal handlers.
- `Tests/test_message.swift` — plain-executable tests for the three Foundation-only
  cores (parser, reducer, options).

## What was and was NOT verified

Verified on this machine:

- `make clean all test` builds clean (zero warnings) under Swift 6 strict
  concurrency, and the tests pass (65/65, exit 0), covering the parser edge cases
  above and the reducer sequences (idle-flood → exactly one fade-out ~1s after
  release; re-grip during fade-out stays visible; re-grip within the linger never
  hides; repeated identical messages produce no repeated commands).
- `make demo` starts, streams the full ~30 lines/s sequence, writes exactly one
  stderr diagnostic for the malformed line, and exits 0 on EOF with no stray
  process.
- **SIGINT** and **SIGTERM** each terminate a running HUD (stdin held open)
  cleanly, exit code 0, within ~150 ms.

NOT verified:

- **The on-screen appearance.** There is no Screen Recording permission in this
  environment and the window cannot be seen or screenshotted from the shell, so
  the visuals — glass material, continuous corners, hairline, soft shadow,
  top-left placement, fades, arming pulse, light/dark colours, the axis emphasis
  and directional arrow, and the error glyph/line — are implemented to the AppKit
  API contract but were **not** confirmed visually.
