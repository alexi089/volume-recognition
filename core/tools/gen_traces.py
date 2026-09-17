"""Writes synthetic traces to traces/: real hands (the fixture photos), scripted motion.

They exercise the CSV format, the replay tool and the manifest end to end, and
they are NOT a substitute for recordings of a real hand in front of the camera:
they contain no landmark jitter, no motion blur, and no in-between hand shapes.
Record real sessions with `make run ARGS="--record traces/<name>.csv"` and add
them to traces/manifest.txt. Run from the repo root:

    .venv/bin/python core/tools/gen_traces.py
"""

import sys
from pathlib import Path

import cv2

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "app"))

from main import Recorder, detect_photo, make_landmarker  # noqa: E402

DT = 1.0 / 30.0


class Shifted:
    def __init__(self, p, dx):
        self.x, self.y, self.z = p.x + dx, p.y, p.z


def ramp(a: float, b: float, seconds: float) -> list[float]:
    n = round(seconds / DT)
    return [a + (b - a) * i / n for i in range(1, n + 1)]


def write(name: str, aspect: float, segments) -> None:
    """segments: list of (hand or None, [x offsets]) played back to back."""
    rec = Recorder(str(ROOT / "traces" / name), aspect)
    t = 0.0
    for hand, offsets in segments:
        for dx in offsets:
            rec.write(t, [Shifted(p, dx) for p in hand] if hand else None)
            t += DT
    rec.close()
    print(f"wrote traces/{name}")


def main() -> None:
    hands = {}
    with make_landmarker(image_mode=True) as landmarker:
        for name in ("victory", "thumbs_up", "pointing_up"):
            bgr = cv2.imread(str(ROOT / "traces" / "fixtures" / f"{name}.jpg"))
            hands[name] = detect_photo(landmarker, bgr)
    v, fist, point = hands["victory"], hands["thumbs_up"], hands["pointing_up"]
    still = [0.0] * 15

    write("synthetic_slide_right.csv", 1.0,
          [(None, still), (v, still), (v, ramp(0.0, 0.20, 0.6)), (v, [0.20] * 15), (fist, still)])
    write("synthetic_slide_left.csv", 1.0,
          [(None, still), (v, still), (v, ramp(0.0, -0.20, 0.6)), (v, [-0.20] * 15), (fist, still)])
    write("synthetic_other_shapes_sweep.csv", 1.0,
          [(point, ramp(-0.2, 0.4, 1.0)), (fist, ramp(0.3, -0.3, 1.0)), (None, still)])
    # The pose flashes past for 100 ms mid-sweep: too short to arm.
    write("synthetic_glimpse_of_pose.csv", 1.0,
          [(fist, ramp(-0.2, 0.0, 0.5)), (v, ramp(0.0, 0.05, 0.1)), (fist, ramp(0.05, 0.3, 0.5))])


if __name__ == "__main__":
    main()
