"""Measure the thumb on the four probe clips (app/record_pinch_probe.sh).

Prints, per clip, the 5th / 50th / 95th percentile of each candidate measurement,
then the frames to lift as test fixtures. Distances are hypot(dx * aspect, dy),
exactly as core/src/pose.c measures them, so a threshold chosen here means the
same thing in C.

    .venv/bin/python core/tools/measure_thumb.py
"""

import math
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "core" / "tools"))

from sweep import load_trace  # noqa: E402

CLIPS = ["probe_pinch_open", "probe_pinch_move", "probe_volume_pose", "probe_pointing"]
FINGERS = {"index": (5, 6, 7, 8), "middle": (9, 10, 11, 12), "ring": (13, 14, 15, 16), "pinky": (17, 18, 19, 20)}


def measures(lm, aspect):
    def d(a, b):
        return math.hypot((lm[a].x - lm[b].x) * aspect, lm[a].y - lm[b].y)

    scale = d(0, 9)
    if scale < 1e-6:
        return None
    m = {
        "thumb_out": d(4, 5) / scale,
        "thumb_reach": d(4, 0) / max(d(2, 0), 1e-9),
        "thumb_straight": d(1, 4) / max(d(1, 2) + d(2, 3) + d(3, 4), 1e-9),
        "gap": d(4, 8) / scale,
        "hand_scale": scale,
    }
    for name, (mcp, pip, dip, tip) in FINGERS.items():
        m[f"{name}_reach"] = d(tip, 0) / max(d(pip, 0), 1e-9)
        m[f"{name}_straight"] = d(mcp, tip) / max(d(mcp, pip) + d(pip, dip) + d(dip, tip), 1e-9)
    return m


def pct(values, p):
    v = sorted(values)
    return v[min(len(v) - 1, int(len(v) * p))]


def main() -> None:
    per_clip = {}
    for clip in CLIPS:
        path = ROOT / "traces" / f"{clip}.csv"
        if not path.exists():
            raise SystemExit(f"missing {path}; run app/record_pinch_probe.sh")
        aspect, rows = load_trace(path)
        # line numbers: load_trace skips the two header lines, and rows are in file order
        frames = []
        for line_no, line in enumerate(path.open(), start=1):
            if line[:1].isdigit():
                frames.append(line_no)
        got = [(frames[i], measures(lm, aspect)) for i, (_, lm, _hand) in enumerate(rows) if lm]
        got = [(n, m) for n, m in got if m]
        per_clip[clip] = got
        print(f"== {clip}: hand in {len(got)}/{len(rows)} frames, aspect {aspect:.3f}")
        if len(got) < len(rows) / 2:
            print("   WARNING: a hand in under half the frames; re-record this clip")
        if not got:
            continue
        for key in got[0][1]:
            vals = [m[key] for _, m in got]
            print(f"   {key:16s} p5 {pct(vals, .05):.3f}   p50 {pct(vals, .5):.3f}   p95 {pct(vals, .95):.3f}")

    def closest_to_median(clip, key):
        got = per_clip[clip]
        med = statistics.median(m[key] for _, m in got)
        return min(got, key=lambda nm: abs(nm[1][key] - med))

    print("\nfixture frames (1-based line numbers):")
    n, m = closest_to_median("probe_pinch_open", "thumb_out")
    print(f"   pinch_open         probe_pinch_open line {n}   thumb_out {m['thumb_out']:.3f} gap {m['gap']:.3f}")
    n, m = min(per_clip["probe_pinch_move"], key=lambda nm: nm[1]["gap"])
    print(f"   pinch_closed       probe_pinch_move line {n}   thumb_out {m['thumb_out']:.3f} gap {m['gap']:.3f}")
    n, m = closest_to_median("probe_pointing", "thumb_out")
    print(f"   pointing_thumb_in  probe_pointing line {n}   thumb_out {m['thumb_out']:.3f} gap {m['gap']:.3f}")


if __name__ == "__main__":
    main()
