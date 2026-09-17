#!/usr/bin/env bash
#
# record_scripted.sh - record the scripted clips the tuning needs, one after
# another, each with its instruction on the preview and a get-ready countdown.
#
#   app/record_scripted.sh            all seven clips (about 6 minutes)
#   app/record_scripted.sh typing     just the clips whose name contains "typing"
#
# Two clips must change the volume (up, down). Five must NOT change it at all:
# they are the ordinary things a person does in front of a camera, and they are
# what the thresholds get tuned against. Afterwards each clip is replayed
# through the C core and checked against its expectation.
#
# Run from the repo root. Quit a clip early with q; Ctrl-C stops the whole run.

set -uo pipefail
cd "$(dirname "$0")/.."

SECONDS_PER_CLIP=40
LEAD_IN=7
FILTER="${1:-}"

# name | expectation | instruction
CLIPS=(
"real_slide_up|up|RAISE the volume: two fingers up, slide right (or up), close your hand. Repeat about 5 times."
"real_slide_down|down|LOWER the volume: two fingers up, slide left (or down), close your hand. Repeat about 5 times."
"real_typing|none|Just TYPE on the keyboard and use the trackpad, as you normally would."
"real_nose_scratch|none|TOUCH YOUR FACE: scratch your nose, rub your eyes, fix your hair, rest your chin on your hand."
"real_cup_reach|none|REACH FOR THINGS: pick up a cup or your phone, drink, put it back, stretch."
"real_waving|none|GESTURE AT THE SCREEN: wave hello, point, thumbs up, open palm, a peace sign in passing. Do NOT hold two fingers still."
"real_talking_hands|none|TALK WITH YOUR HANDS as if you were on a video call explaining something."
)

# Two instances would fight over the volume and show two HUDs, and rebuilding a
# library that a running copy has loaded can crash it. So: wait first, build after.
while pgrep -f "app/main.py" >/dev/null; do
    echo "another copy of the app is running; quit it (q in its preview) and this will continue..."
    sleep 3
done

make -s setup core hud || exit 1

failed=0
for clip in "${CLIPS[@]}"; do
    IFS='|' read -r name expect prompt <<< "$clip"
    [[ -n "$FILTER" && "$name" != *"$FILTER"* ]] && continue
    echo "=== $name (expect: $expect)"
    .venv/bin/python app/main.py --record "traces/$name.csv" --duration "$SECONDS_PER_CLIP" \
        --lead-in "$LEAD_IN" --prompt "$prompt" 2> >(grep -v -E '^(W0000|I0000|INFO:|WARNING: All log|.*clearcut|=== Source|wireless/android)' >&2)
    status=$?
    if [[ $status -ne 0 ]]; then
        echo "    the app exited with status $status; stopping" >&2
        exit "$status"
    fi
    core/build/replay "traces/$name.csv" "$expect" || failed=1
done
echo
[[ $failed -eq 0 ]] && echo "every clip met its expectation" || echo "at least one clip did NOT meet its expectation (that is what the tuning is for)"
exit $failed
