"""The landmarker must be able to run indefinitely.

The second live session ended in a crash after eight minutes: MediaPipe's GPU
delegate on macOS keeps about one IOSurface-backed pixel buffer per frame for
as long as the landmarker is open (they are released only when it closes), so
the process grew by megabytes a frame until CVPixelBufferCreate failed with
-6662 (kCVReturnAllocationFailed) and MediaPipe aborted. This runs the same
code path the camera loop uses and requires the memory footprint to stay flat.
Run with `make test`.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).parent))

from main import detect, make_landmarker  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
FRAMES = 400
ALLOWED_GROWTH_MB = 100  # the leak was about 3.6 MB per 720p frame: 1,400 MB over this run


def footprint_mb() -> float:
    out = subprocess.run(["footprint", "-p", str(os.getpid()), "--summary"], capture_output=True, text=True).stdout
    match = re.search(r"phys_footprint:\s+([\d.]+)\s*(KB|MB|GB)", out)
    if not match:
        raise SystemExit("could not read the footprint (is this macOS?)")
    return float(match.group(1)) * {"KB": 1 / 1024, "MB": 1, "GB": 1024}[match.group(2)]


def main() -> int:
    print("test_leak")
    photo = cv2.resize(cv2.imread(str(ROOT / "traces" / "fixtures" / "victory.jpg")), (1280, 720))
    found = 0
    with make_landmarker() as landmarker:
        for i in range(50):  # warm-up: model load, first-use allocations
            detect(landmarker, np.roll(photo, i % 40 - 20, axis=1), i * 33)
        before = footprint_mb()
        for i in range(50, 50 + FRAMES):
            found += detect(landmarker, np.roll(photo, i % 40 - 20, axis=1), i * 33) is not None
        after = footprint_mb()

    growth = after - before
    ok_hand = found > FRAMES * 0.9
    ok_leak = growth < ALLOWED_GROWTH_MB
    print(f"  {'ok  ' if ok_hand else 'FAIL'} hand found in {found}/{FRAMES} frames")
    print(f"  {'ok  ' if ok_leak else 'FAIL'} footprint {before:.0f} -> {after:.0f} MB over {FRAMES} frames "
          f"(grew {growth:.0f} MB, allowed {ALLOWED_GROWTH_MB})")
    failures = (not ok_hand) + (not ok_leak)
    print(f"{'FAILED' if failures else 'passed'} ({failures} failures)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
