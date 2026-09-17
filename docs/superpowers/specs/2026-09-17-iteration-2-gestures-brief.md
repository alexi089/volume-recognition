# Iteration 2: more gestures (brief, not yet a design)

Date: 2026-09-17. Status: **ideas captured from the owner; nothing designed or
built.** This exists so the ideas are not lost while iteration 1 (volume) is
finished. It needs a proper brainstorm, then a spec and a plan, before any code.

## What the owner asked for

1. **Pinch to resize the frontmost application's window.** Thumb and index
   extended and apart, then brought together: the window shrinks. Pinched, then
   spread apart: the window grows. The distance between the two fingertips is
   the control.
2. **Four-finger swipe left / right for previous / next**: videos in a
   playlist, songs, pages. The owner mused about the structure behind each
   (linked list for pages, queue for a playlist).

## Honest technical notes

- **We do not own the playlist or the page stack.** The target app does. What
  we can do is send it "next" / "previous": media keys for music and video,
  arrow keys or Cmd-[ / Cmd-] for pages. How the app stores its items is its
  business; we never see it. (A toy playlist of our own, to learn linked lists
  and queues in C, would be a separate, optional exercise.)
- **Both actions need macOS Accessibility permission.** Resizing another app's
  window goes through the AX API (`AXUIElementSetAttributeValue` with
  `kAXSizeAttribute`; a C API, which suits the project). Posting media or key
  events goes through `CGEventPost` / HID system-defined events, which recent
  macOS also gates behind Accessibility. The permission attaches to the app
  hosting the process (VS Code, Terminal), like the camera did.
- **The swipe brings back the return-stroke problem.** It was the first thing
  flagged on day one and the reason volume became a slider: after a swipe right
  the hand travels back left, which looks like a swipe left. A discrete swipe
  needs an arming pose, a speed threshold, a direction decided once, and a
  refractory period; and it needs negative recordings more than anything else.
- **Every gesture needs its own arming pose, and they must not overlap.**
  Volume: index and middle extended, ring and pinky not. The open palm (four
  fingers extended) was deliberately made unable to arm volume, so it is free
  for the swipe. Pinch needs thumb and index extended with the other three
  curled; the thumb is currently ignored entirely, and its geometry differs
  (its reach and straightness thresholds will not be the fingers'). The risky
  overlaps: pinch vs a relaxed two-finger pose; open palm vs "staying armed" in
  a volume slide (the stay rule tolerates an open palm on purpose).
- **Architecture.** The C core becomes a multi-gesture recogniser:
  `IDLE -> ARMING(kind) -> ARMED(kind)`, arbitration by pose, and an output that
  is an action (`set volume`, `resize by factor`, `next`, `previous`) rather
  than a volume. `step.c`'s injected backend grows from `{get, set}` volume to a
  small table of actions, each with a fake for tests. The plausibility gate,
  slew limiting and the trace / replay tooling carry over unchanged.
- **Pinch specifics to decide:** absolute (window size follows fingertip
  distance) or relative (like the volume accumulator); which corner stays
  fixed; minimum and maximum sizes; what "frontmost window" means when the
  preview window or the HUD is frontmost; fingertip distance is noisier than
  palm position, and the two fingertips occlude each other when pinched.
- **Feedback.** The HUD contract is already a JSON line per frame; it would gain
  a `gesture` field and a second visual for resize.

## Before starting

Iteration 1 is not finished: scripted recordings (positives and, above all,
negatives) have not been made, the constants are tuned on unlabeled sessions,
and the AirPods' intermittent refusal of volume writes is unexplained. Adding
gestures multiplies the false-positive surface, so the negative recordings
matter even more for iteration 2 than for iteration 1.
