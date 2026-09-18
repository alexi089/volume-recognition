"""End-to-end test of the Python <-> C boundary, without a camera.

photo -> MediaPipe -> ctypes -> C state machine. Uses the pure gv_update path,
so the machine's real volume is never touched. Run with `make test`.
"""

import sys
from pathlib import Path

import cv2

sys.path.insert(0, str(Path(__file__).parent))

import bridge  # noqa: E402
from main import detect_photo, make_landmarker  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DT = 1.0 / 30.0

failures = 0


def check(cond: bool, label: str) -> None:
    global failures
    print(f"  {'ok  ' if cond else 'FAIL'} {label}")
    if not cond:
        failures += 1


def landmarks_from_photo(landmarker, name: str):
    bgr = cv2.imread(str(ROOT / "traces" / "fixtures" / f"{name}.jpg"))
    hand = detect_photo(landmarker, bgr)
    assert hand, f"no hand found in {name}.jpg"
    return hand, bgr.shape[1] / bgr.shape[0]


class Shifted:
    """A landmark moved by (dx, dy); quacks like MediaPipe's NormalizedLandmark."""

    def __init__(self, p, dx, dy=0.0):
        self.x, self.y, self.z = p.x + dx, p.y + dy, p.z


def run(gesture, hand, offsets, t, vertical=False):
    """Feed one frame per offset (normalised units, along x or y); returns (last output, t)."""
    out = None
    for d in offsets:
        moved = [Shifted(p, 0.0, d) if vertical else Shifted(p, d) for p in hand] if hand else None
        out = gesture.update(moved, t)
        t += DT
    return out, t


def main() -> int:
    print("test_bridge")
    with make_landmarker(image_mode=True) as landmarker:
        victory, aspect = landmarks_from_photo(landmarker, "victory")
        thumbs_up, _ = landmarks_from_photo(landmarker, "thumbs_up")

    # A real hand, held still, arms; sliding it right raises the volume.
    # gain and lock distance pinned: the expected volumes below are arithmetic on
    # them, not on the tuned defaults. A slide of h hand-lengths is worth gain * (h - lock).
    pinned = {"gain": 1 / 3, "gain_fast": 1 / 3, "axis_lock_hands": 0.5, "edge_rate": 0.0}  # flat gain, no edge push
    with bridge.Gesture(aspect=aspect, **pinned) as g:
        g.set_current_volume(0.30)
        out, t = run(g, victory, [0.0] * 15, 0.0)
        check(out.state == bridge.ARMED, "victory photo arms after half a second")
        check(abs(out.volume - 0.30) < 1e-6, "arming leaves the volume alone")

        hand = g.hand_scale(victory) / aspect  # one hand-length, in normalised x
        slide = [1.5 * hand * i / 18 for i in range(1, 19)] + [1.5 * hand] * 15
        out, t = run(g, victory, slide, t)
        want = 0.30 + (1.5 - 0.5) / 3
        check(abs(out.volume - want) < 0.02, f"1.5 hand-lengths right: 0.30 -> {out.volume:.3f} (want {want:.3f})")
        check(out.axis == bridge.AXIS_X, "the slide was taken as left/right")
        check(out.kind == bridge.KIND_VOLUME and abs(out.scale - 1.0) < 1e-6, "a volume slide reports kind volume, scale 1")

        out, t = run(g, None, [0.0] * 10, t)
        check(out.state == bridge.IDLE and out.kind == bridge.KIND_NONE, "losing the hand releases, and no gesture is reported")
        check(abs(out.volume - want) < 0.02, "and the volume stays where it was left")

    # Up is louder. Image y grows downwards, so "up" is a negative y offset here.
    with bridge.Gesture(aspect=aspect, **pinned) as g:
        g.set_current_volume(0.30)
        out, t = run(g, victory, [0.0] * 15, 0.0)
        hand_y = g.hand_scale(victory)  # one hand-length, in normalised y (frame heights)
        climb = [-1.5 * hand_y * i / 18 for i in range(1, 19)] + [-1.5 * hand_y] * 15
        out, t = run(g, victory, climb, t, vertical=True)
        check(out.axis == bridge.AXIS_Y, "a vertical slide is taken as up/down")
        check(abs(out.volume - want) < 0.02, f"1.5 hand-lengths up: 0.30 -> {out.volume:.3f} (want {want:.3f})")

    # A different hand shape never arms, wherever it goes.
    with bridge.Gesture(aspect=aspect) as g:
        g.set_current_volume(0.50)
        armed = changed = False
        t = 0.0
        for i in range(45):
            out, t = run(g, thumbs_up, [0.004 * i], t)
            armed |= out.state == bridge.ARMED
            changed |= bool(out.changed)
        check(not armed and not changed, "thumbs-up sliding across the frame does nothing")

    # Config crosses the boundary intact, and the C side is the one that validates it.
    with bridge.Gesture(aspect=aspect, gain=0.15, release_grace_sec=0.4) as g:
        c = g.config
        check(abs(c.gain - 0.15) < 1e-6 and abs(c.release_grace_sec - 0.4) < 1e-6, "overrides reach the C config")
        check(abs(c.pose.extend_reach - 1.20) < 1e-6 and abs(c.gate_max_speed - 30.0) < 1e-6, "nested and later fields line up (struct layout)")
        check(abs(c.pose.thumb_clear_min - 0.62) < 1e-6 and abs(c.arm_hold_sec - 0.25) < 1e-6,
              "the last nested field and the first field after the nested struct line up")
        check(abs(c.scale_min - 0.1) < 1e-6 and abs(c.resize_rate_max - 2.0) < 1e-6
              and abs(c.release_rollback_sec - 0.3) < 1e-6,
              "the resize fields line up")
        check(abs(c.resize_rate_gain - 4.5) < 1e-6 and abs(c.win_write_interval_sec - 0.06) < 1e-6,
              "the resize fields line up")
        check(abs(c.flick_travel - 1.5) < 1e-6, "the flick fields line up")
        check(abs(c.flick_refractory_sec - 1.5) < 1e-6,
              "the config lines up down to the very last field")
    try:
        bridge.Gesture(aspect=aspect, gain=float("nan"))
        check(False, "NaN gain is rejected")
    except ValueError:
        check(True, "NaN gain is rejected by gv_create")
    try:
        bridge.Gesture(aspect=aspect, no_such_field=1.0)
        check(False, "unknown field is rejected")
    except ValueError:
        check(True, "unknown config field is rejected")

    # The volume module answers through the bridge (read-only calls).
    status, name = bridge.device_name()
    check(status == 0 and isinstance(name, str), f"device name readable: {name!r}")
    check(bridge.strerror(-5).startswith("the output accepted"), "status codes map to text")

    # So does the window module. Nothing is held outside a resize, so the name
    # call must say so rather than hand back a stale or empty success.
    check(bridge.win_strerror(-3) == "the front window has a fixed size", "window status codes map to text")
    check("Accessibility" in bridge.win_strerror(-1), "the not-trusted text says where to allow it")
    # The one positive status: a minimise that has not landed yet is not an error.
    check("not yet" in bridge.win_strerror(bridge.WIN_PENDING), "pending reads as not-yet, not as a failure")
    status, name = bridge.win_target_name()
    check(status == -2 and name == "", "no window is held outside a resize")

    status, size = bridge.win_current_size()
    check(status == -2 and size == (0.0, 0.0), "no window frame outside a resize")

    # The replay verdict is C's (core/src/verdict.c); Python only asks it.
    check(bridge.verdict("shrink", 0.0, 0, 12, 0.7) and not bridge.verdict("grow", 0.0, 0, 12, 0.7),
          "the C verdict answers through the bridge")
    check(not bridge.verdict("none", 0.0, 0, 2, 1.0), "none fails on a resize change even when the scale came back")
    try:
        bridge.verdict("sideways", 0.0, 0, 0, 1.0)
        check(False, "an unknown expectation is rejected")
    except ValueError:
        check(True, "an unknown expectation is rejected")

    # The step result carries the window status, and an idle frame reports no trouble.
    with bridge.Gesture(aspect=aspect) as g:
        r = g.step(None, 0.0)
        check(r.win == 0 and r.out.kind == 0 and abs(r.out.scale - 1.0) < 1e-6,
              "an idle step: win ok, no gesture, scale 1")

    print(f"{'FAILED' if failures else 'passed'} ({failures} failures)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
