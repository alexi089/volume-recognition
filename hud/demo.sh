#!/usr/bin/env bash
#
# demo.sh - drive volume-hud the way the real producer does: one line EVERY
# frame (~30/s, repeating the current state every ~33 ms), not one line per
# state change. This exercises the fade-out-under-idle-flood path (HIGH-1) and
# the re-grip-during-fade path (HIGH-2) so a regression would be visible.
#
# Requires a logged-in GUI (Aqua) session; the window is drawn by the window
# server. Run:  make -C hud demo    (or: ./hud/demo.sh)

set -uo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
HUD="$DIR/build/volume-hud"

if [ ! -x "$HUD" ]; then
    echo "volume-hud not built; run: make -C hud all" >&2
    exit 1
fi

# Emit `count` copies of one line at ~30/s.
frames() {
    local count="$1"; local line="$2"; local i=0
    while [ "$i" -lt "$count" ]; do
        printf '%s\n' "$line"
        sleep 0.033
        i=$((i + 1))
    done
}

line() {  # state volume axis(null|x|y) error(null|"...")
    printf '{"state":"%s","volume":%s,"axis":%s,"error":%s}' "$1" "$2" "$3" "$4"
}

generate() {
    # idle stream: HUD stays hidden (~1s)
    frames 30 "$(line idle 0.50 null null)"
    # arming: fades in + subtle pulse; axis undecided (~1s)
    frames 30 "$(line arming 0.50 null null)"
    # armed, still no direction: bar at reduced emphasis (~0.6s)
    frames 18 "$(line armed 0.50 null null)"
    # direction commits to x: full emphasis + tiny arrow; sweep the volume
    for v in 0.55 0.62 0.70 0.80 0.90 1.00 0.66 0.33 0.10 0.00 0.45; do
        frames 6 "$(line armed "$v" '"x"' null)"
    done
    # release: idle stream begins (~0.6s, INSIDE the 1s linger)...
    frames 18 "$(line idle 0.45 '"x"' null)"
    # ...then a quick re-grip inside the linger: must NOT have hidden (HIGH-1/2)
    frames 24 "$(line armed 0.52 '"x"' null)"
    # error appears: glyph changes shape (badge) + orange tint + second line
    frames 45 "$(line armed 0.52 '"x"' '"the output accepted the volume change but did not apply it"')"
    # error clears: second line removed, glyph back to the speaker variant
    frames 24 "$(line armed 0.52 '"x"' null)"
    # a malformed line (ignored, one stderr diagnostic) and a blank line (skipped)
    printf '%s\n' 'not json at all'; sleep 0.033
    printf '\n'; sleep 0.033
    # final idle: keep streaming idle ~2.5s so the fade-out happens WHILE idle
    # lines are still arriving (the HIGH-1 scenario)
    frames 75 "$(line idle 0.52 null null)"
}

echo "streaming ~30 lines/s into volume-hud (EOF will quit it)..."
"$HUD" < <(generate) &
HUD_PID=$!
trap 'kill -TERM "$HUD_PID" 2>/dev/null' EXIT INT TERM
wait "$HUD_PID"
status=$?
echo "demo finished; volume-hud exited $status"
exit "$status"
