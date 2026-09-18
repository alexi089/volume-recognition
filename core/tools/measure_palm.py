"""Measure how often a recording shows an open palm: all four fingers
extended and the thumb out, by the exact rules core/src/pose.c uses for
gv_finger_is_extended and gv_thumb_is_out.

Reads each trace with an INLINE CSV loader (not core/tools/sweep.py's
load_trace, which imports app/bridge.py and loads the built shared library) so
this tool still runs when libgesture.dylib is stale or missing. Distances are
hypot(dx * aspect, dy), exactly as pose.c measures them, so a threshold here
means the same thing in C.

    .venv/bin/python core/tools/measure_palm.py
"""

import math
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# The same limits as core/tests/test_pose.c's LIM, so this tool's "open palm"
# and gv_pose_is_open_palm's "open palm" are the same claim.
EXTEND_REACH = 1.20
EXTEND_STRAIGHT = 0.93
THUMB_EXTEND_REACH = 1.05
THUMB_EXTEND_STRAIGHT = 0.67

FINGERS = {
    "index": (5, 6, 7, 8),
    "middle": (9, 10, 11, 12),
    "ring": (13, 14, 15, 16),
    "pinky": (17, 18, 19, 20),
}


class P:
    __slots__ = ("x", "y", "z")

    def __init__(self, x, y, z):
        self.x, self.y, self.z = x, y, z


def load_trace(path: Path):
    """Inline loader: same format as sweep.load_trace, no bridge import.

    Rows are t_sec,present,x0,y0,z0,...,x20,y20,z20[,hand]: 65 or 66
    comma-separated fields. A `# aspect=...` comment line gives the aspect;
    every other non-data line (the header, blank lines) is skipped.
    """
    aspect, rows = 16 / 9, []
    for line in path.open():
        if line.startswith("# aspect="):
            aspect = float(line.split("=")[1])
            continue
        v = line.strip().split(",")
        if len(v) not in (65, 66) or not v[0][:1].isdigit():
            continue
        f = [float(x) for x in v]
        rows.append([P(*f[2 + 3 * i : 5 + 3 * i]) for i in range(21)] if f[1] else None)
    return aspect, rows


def dist(lm, a, b, aspect):
    return math.hypot((lm[a].x - lm[b].x) * aspect, lm[a].y - lm[b].y)


def finger_is_extended(lm, joints, aspect):
    mcp, pip, dip, tip = joints
    to_pip = dist(lm, pip, 0, aspect)
    reach = dist(lm, tip, 0, aspect) / to_pip if to_pip > 1e-6 else 0.0
    along = dist(lm, mcp, pip, aspect) + dist(lm, pip, dip, aspect) + dist(lm, dip, tip, aspect)
    straight = dist(lm, mcp, tip, aspect) / along if along > 1e-6 else 0.0
    return reach > EXTEND_REACH and straight > EXTEND_STRAIGHT


def thumb_is_out(lm, aspect):
    to_mcp = dist(lm, 2, 0, aspect)
    reach = dist(lm, 4, 0, aspect) / to_mcp if to_mcp > 1e-6 else 0.0
    along = dist(lm, 1, 2, aspect) + dist(lm, 2, 3, aspect) + dist(lm, 3, 4, aspect)
    straight = dist(lm, 1, 4, aspect) / along if along > 1e-6 else 0.0
    return reach > THUMB_EXTEND_REACH and straight > THUMB_EXTEND_STRAIGHT


def is_open_palm(lm, aspect):
    return all(finger_is_extended(lm, joints, aspect) for joints in FINGERS.values()) and thumb_is_out(lm, aspect)


def measure_clip(name: str) -> None:
    path = ROOT / "traces" / f"{name}.csv"
    if not path.exists():
        # take1/ holds the guided negatives; the resize session lives at traces/ root
        path = ROOT / "traces" / "take1" / f"{name}.csv"
    if not path.exists():
        raise SystemExit(f"missing {path}")
    aspect, rows = load_trace(path)
    hand_frames = [lm for lm in rows if lm]
    palm_frames = sum(1 for lm in hand_frames if is_open_palm(lm, aspect))
    n = len(hand_frames)
    frac = palm_frames / n if n else 0.0
    print(f"{name:28s} hand frames {n:5d}   open palm {palm_frames:5d}   fraction {frac:.3f}")


def measure_fixture() -> None:
    src = (ROOT / "core" / "tests" / "fixtures.h").read_text()
    m = re.search(r"fixture_real_open_palm\s*=\s*\{(.*?)\n\};", src, re.S)
    nums = re.findall(r"\{\s*(-?[\d.]+)f?,\s*(-?[\d.]+)f?,\s*(-?[\d.]+)f?\s*\}", m.group(1))
    lm = [P(float(a), float(b), float(c)) for a, b, c in nums[:21]]
    aspect = 1.777778  # FIXTURE_REAL_ASPECT
    print(f"{'fixture_real_open_palm':28s} open palm: {is_open_palm(lm, aspect)}")


def main() -> None:
    for name in ["real_waving", "real_talking_hands", "real_cup_reach", "real_resize_second_session"]:
        measure_clip(name)
    measure_fixture()


if __name__ == "__main__":
    main()
