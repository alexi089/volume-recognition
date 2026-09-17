#!/usr/bin/env bash
#
# record_pinch_probe.sh - four 10 s clips that measure the thumb for the resize pose.
#
# The code ignored the thumb until the resize gesture needed it, so there was no
# data on what a thumb that is "out" or "in" measures on a real hand. This
# records the four hand shapes the rule has to tell apart, then prints the
# measurements (core/tools/measure_thumb.py) the thresholds are chosen from.
#
# Run from the repo root, with someone at the camera. Not part of `make test`.

set -uo pipefail
cd "$(dirname "$0")/.."

# Two copies would fight over the camera and the volume.
while pgrep -x volume-hud >/dev/null || ps -Ao args= | grep -q "^.venv/bin/python app/main.py"; do
    echo "another copy of the app is running; quit it first..."
    sleep 3
done

make -s setup core || exit 1

# name | instruction
CLIPS=(
"probe_pinch_open|HOLD the resize pose: thumb and index OUT and apart, other three fingers curled. Move it around a little."
"probe_pinch_move|Same pose: slowly PINCH the two fingertips together, then SPREAD them wide. Repeat."
"probe_volume_pose|HOLD the volume pose: index and middle up, thumb wherever it naturally sits."
"probe_pointing|POINT at the screen with your index finger: thumb tucked in, other fingers curled."
)

for clip in "${CLIPS[@]}"; do
    IFS='|' read -r name prompt <<< "$clip"
    echo "=== $name"
    .venv/bin/python app/main.py --no-hud --record "traces/$name.csv" --duration 10 --lead-in 6 --prompt "$prompt" \
        2> >(grep -v -E '^(W0000|I0000|INFO:|WARNING: All log|.*clearcut|=== Source|wireless/android)' >&2) || exit $?
done

.venv/bin/python core/tools/measure_thumb.py
