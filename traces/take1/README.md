# take1: first scripted recordings (2026-09-17), no handedness column

Recorded with `app/record_scripted.sh` before the recorder wrote MediaPipe's
left/right label. Kept because `real_nose_scratch.csv` is a known failing
negative: touching the face armed the slider three times and moved the volume
by +0.09 with the settings of commit 63b1382. The other four negatives were
clean (typing, cup reach and waving never armed; talking hands armed twice and
the axis lock kept the volume still). Not in `manifest.txt`: without the hand
label the fix (palm must face the camera) cannot be evaluated on these files.
