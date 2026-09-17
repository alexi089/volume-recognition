"""Replay the scripted recordings through the C core under different settings.

The tuning rule: minimise dropped slides on the positive clips SUBJECT TO zero
change, of the volume or of a window, on every negative clip. This prints, for each candidate config,
what every clip did, so that choice is made from numbers rather than feel.

    .venv/bin/python core/tools/sweep.py                 # the shipped defaults
    .venv/bin/python core/tools/sweep.py arm_hold_sec=0.4 pose.extend_reach=1.25
    .venv/bin/python core/tools/sweep.py --grid          # a small built-in grid

Uses the pure gv_update path: the real system volume is never touched.
"""

import csv
import itertools
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "app"))

import bridge  # noqa: E402

START_VOLUME = 0.5


class P:
    __slots__ = ("x", "y", "z")

    def __init__(self, x, y, z):
        self.x, self.y, self.z = x, y, z


def load_trace(path: Path):
    aspect, rows = 16 / 9, []
    for line in path.open():
        if line.startswith("# aspect="):
            aspect = float(line.split("=")[1])
            continue
        v = line.strip().split(",")
        # 65 fields, or 66 since recordings carry MediaPipe's left/right label
        if len(v) not in (65, 66) or not v[0][:1].isdigit():
            continue
        f = [float(x) for x in v]
        hand = int(f[65]) if len(f) == 66 else 0
        rows.append((f[0], [P(*f[2 + 3 * i : 5 + 3 * i]) for i in range(21)] if f[1] else None, hand))
    return aspect, rows


def manifest():
    """(name, expectation) for every real_* clip listed in traces/manifest.txt, else every real_*.csv found."""
    listed = []
    for line in (ROOT / "traces" / "manifest.txt").read_text().splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0].startswith("real_") and not line.lstrip().startswith("#"):
            listed.append((parts[0][:-4], parts[1]))
    if listed:
        return listed
    guess = {"real_slide_up": "up", "real_slide_down": "down"}
    return [(p.stem, guess.get(p.stem, "none")) for p in sorted((ROOT / "traces").glob("real_*.csv"))
            if "session" not in p.stem and not p.stem.endswith(".events")]


def make_gesture(aspect: float, overrides: dict) -> bridge.Gesture:
    top = {k: v for k, v in overrides.items() if "." not in k}
    g = bridge.Gesture(aspect, **top)
    nested = {k: v for k, v in overrides.items() if "." in k}
    if nested:  # pose.* fields: rebuild the context with the nested struct edited
        cfg = g.config
        for key, value in nested.items():
            outer, inner = key.split(".")
            setattr(getattr(cfg, outer), inner, value)
        bridge._lib.gv_destroy(g._ctx)
        g._ctx = bridge._lib.gv_create(bridge.C.byref(cfg))
        if not g._ctx:
            raise SystemExit(f"gv_create rejected {overrides}")
    return g


def run_clip(name: str, overrides: dict):
    aspect, rows = load_trace(ROOT / "traces" / f"{name}.csv")
    with make_gesture(aspect, overrides) as g:
        g.set_current_volume(START_VOLUME)
        sessions, armings, prev, t0, volume = [], 0, bridge.IDLE, 0.0, START_VOLUME
        volume_changes = resize_changes = resize_arms = 0
        final_scale = 1.0
        for t, lm, hand in rows:
            o = g.update(lm, t, hand)
            if o.state == bridge.ARMING and prev == bridge.IDLE:
                armings += 1
            if o.state == bridge.ARMED and prev != bridge.ARMED:
                t0 = t
                resize_arms += o.kind == bridge.KIND_RESIZE
            if prev == bridge.ARMED and o.state != bridge.ARMED:
                sessions.append(t - t0)
            # as core/tools/replay.c counts them: by kind, and a resize's last
            # word (on the frame it goes idle) is its final scale
            if o.changed and o.kind == bridge.KIND_VOLUME:
                volume_changes += 1
            if o.changed and o.kind == bridge.KIND_RESIZE:
                resize_changes += 1
                final_scale = o.scale
            prev, volume = o.state, o.volume
        if prev == bridge.ARMED:
            sessions.append(rows[-1][0] - t0)
    return {"armings": armings, "sessions": sessions, "changes": volume_changes + resize_changes,
            "volume_changes": volume_changes, "resize_changes": resize_changes, "resize_arms": resize_arms,
            "delta": volume - START_VOLUME, "final_scale": final_scale}


def verdict(expect: str, r: dict) -> bool:
    return bridge.verdict(expect, r["delta"], r["volume_changes"], r["resize_changes"], r["final_scale"])


def report(overrides: dict, clips, verbose: bool) -> tuple[int, int, int]:
    bad_neg = false_changes = short = 0
    lines = []
    for name, expect in clips:
        r = run_clip(name, overrides)
        ok = verdict(expect, r)
        n = len(r["sessions"])
        med = statistics.median(r["sessions"]) if n else 0.0
        under1 = sum(1 for s in r["sessions"] if s < 1.0)
        if expect == "none":
            bad_neg += not ok
            false_changes += r["changes"]
        else:
            short += under1
        lines.append(f"    {'ok  ' if ok else 'FAIL'} {name:22s} expect {expect:4s} armings {r['armings']:3d}  armed {n:2d}"
                     f" (median {med:4.1f}s, {under1} under 1s)  changes {r['changes']:4d}  volume {r['delta']:+.2f}"
                     f"  resize arms {r['resize_arms']:2d}  scale {r['final_scale']:.2f}")
    label = " ".join(f"{k}={v}" for k, v in overrides.items()) or "shipped defaults"
    print(f"{label}\n  negatives failing: {bad_neg}, false changes (volume or window): {false_changes}, short positive slides: {short}")
    if verbose:
        print("\n".join(lines))
    return bad_neg, false_changes, short


def main() -> None:
    clips = manifest()
    if not clips:
        raise SystemExit("no real_* clips found; record them with app/record_scripted.sh")
    args = sys.argv[1:]
    if args and args[0] == "--grid":
        grid = {
            "arm_hold_sec": [0.25, 0.35, 0.5],
            "pose.extend_reach": [1.20, 1.25, 1.30],
            "pose.extend_straight": [0.93, 0.95],
        }
        results = []
        for combo in itertools.product(*grid.values()):
            overrides = dict(zip(grid.keys(), combo))
            results.append((report(overrides, clips, verbose=False), overrides))
        print("\nbest (fewest failing negatives, then false changes, then short slides):")
        for score, overrides in sorted(results, key=lambda r: r[0])[:5]:
            print(f"  {score}  {overrides}")
        return
    overrides = {}
    for a in args:
        key, _, value = a.partition("=")
        overrides[key] = float(value)
    report(overrides, clips, verbose=True)


if __name__ == "__main__":
    main()
