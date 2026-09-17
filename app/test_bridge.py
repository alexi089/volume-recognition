"""End-to-end test of the Python <-> C boundary, without a camera.

photo -> MediaPipe -> ctypes -> C state machine. Uses the pure gv_update path,
so the machine's real volume is never touched. Run with `make test`.
"""

import sys
from pathlib import Path

import cv2
import mediapipe as mp

sys.path.insert(0, str(Path(__file__).parent))

import bridge  # noqa: E402
from main import make_landmarker  # noqa: E402

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
    rgba = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGBA)
    result = landmarker.detect(mp.Image(image_format=mp.ImageFormat.SRGBA, data=rgba))
    assert result.hand_landmarks, f"no hand found in {name}.jpg"
    return result.hand_landmarks[0], bgr.shape[1] / bgr.shape[0]


class Shifted:
    """A landmark moved sideways; quacks like MediaPipe's NormalizedLandmark."""

    def __init__(self, p, dx):
        self.x, self.y, self.z = p.x + dx, p.y, p.z


def run(gesture, hand, offsets, t):
    """Feed one frame per offset (in normalised x); returns (last output, t)."""
    out = None
    for dx in offsets:
        out = gesture.update([Shifted(p, dx) for p in hand] if hand else None, t)
        t += DT
    return out, t


def main() -> int:
    print("test_bridge")
    with make_landmarker(image_mode=True) as landmarker:
        victory, aspect = landmarks_from_photo(landmarker, "victory")
        thumbs_up, _ = landmarks_from_photo(landmarker, "thumbs_up")

    # A real hand, held still, arms; sliding it right raises the volume.
    # gain pinned: the expected volume below is arithmetic on it, not on the tuned default
    with bridge.Gesture(aspect=aspect, gain=1 / 3) as g:
        g.set_current_volume(0.30)
        out, t = run(g, victory, [0.0] * 15, 0.0)
        check(out.state == bridge.ARMED, "victory photo arms after half a second")
        check(abs(out.volume - 0.30) < 1e-6, "arming leaves the volume alone")

        hand = g.hand_scale(victory) / aspect  # one hand-length, in normalised x
        slide = [1.5 * hand * i / 18 for i in range(1, 19)] + [1.5 * hand] * 15
        out, t = run(g, victory, slide, t)
        check(abs(out.volume - 0.80) < 0.02, f"1.5 hand-lengths right: 0.30 -> {out.volume:.3f} (want 0.80)")

        out, t = run(g, None, [0.0] * 10, t)
        check(out.state == bridge.IDLE, "losing the hand releases")
        check(abs(out.volume - 0.80) < 0.02, "and the volume stays where it was left")

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

    print(f"{'FAILED' if failures else 'passed'} ({failures} failures)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
