"""Webcam -> MediaPipe hand landmarks -> on-screen overlay.

Spike for step 1 of the build: proves the camera opens, MediaPipe returns 21
landmarks per frame, and shows the frame rate. No gesture logic lives here, and
none ever will: this file is a pipe between the camera and the C core.

Run with `make run`. Quit with q or Esc.
"""

import sys
import time
from pathlib import Path

import cv2
import mediapipe as mp
from mediapipe.tasks.python import BaseOptions, vision

MODEL_PATH = Path(__file__).parent / "models" / "hand_landmarker.task"
CONNECTIONS = [(c.start, c.end) for c in vision.HandLandmarksConnections.HAND_CONNECTIONS]


def make_landmarker() -> vision.HandLandmarker:
    if not MODEL_PATH.exists():
        sys.exit(f"Model file missing: {MODEL_PATH}\nRun `make model` to download it.")
    options = vision.HandLandmarkerOptions(
        base_options=BaseOptions(
            model_asset_path=str(MODEL_PATH),
            # The CPU delegate aborts inside MediaPipe 1.0.1 on macOS
            # ("DrishtiMetalHelper ... Service is unavailable"). Metal works.
            delegate=BaseOptions.Delegate.GPU,
        ),
        running_mode=vision.RunningMode.VIDEO,
        num_hands=1,
    )
    return vision.HandLandmarker.create_from_options(options)


def open_camera(index: int = 0) -> cv2.VideoCapture:
    cap = cv2.VideoCapture(index)
    ok, _ = cap.read() if cap.isOpened() else (False, None)
    if not ok:
        sys.exit(
            "Could not read from the camera.\n"
            "macOS grants camera access per app: allow it for the app hosting this\n"
            "terminal (VS Code or Terminal) under System Settings > Privacy & Security >\n"
            "Camera, then restart that app."
        )
    return cap


def draw_hand(frame, landmarks) -> None:
    h, w = frame.shape[:2]
    pts = [(int(p.x * w), int(p.y * h)) for p in landmarks]
    for a, b in CONNECTIONS:
        cv2.line(frame, pts[a], pts[b], (80, 200, 80), 2)
    for x, y in pts:
        cv2.circle(frame, (x, y), 4, (255, 255, 255), -1)


def main() -> None:
    cap = open_camera()
    start = time.monotonic()
    last_ts_ms = -1
    fps, last_frame_t = 0.0, start

    with make_landmarker() as landmarker:
        while True:
            ok, bgr = cap.read()
            if not ok:
                sys.exit("Camera stopped delivering frames.")

            # Selfie view: the user's right is +x, and the preview reads like a mirror.
            bgr = cv2.flip(bgr, 1)

            # VIDEO mode requires strictly increasing integer timestamps.
            now = time.monotonic()
            ts_ms = max(int((now - start) * 1000), last_ts_ms + 1)
            last_ts_ms = ts_ms

            # The Metal delegate only accepts 4-channel images.
            rgba = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGBA)
            result = landmarker.detect_for_video(
                mp.Image(image_format=mp.ImageFormat.SRGBA, data=rgba), ts_ms
            )

            if result.hand_landmarks:
                draw_hand(bgr, result.hand_landmarks[0])

            dt = now - last_frame_t
            last_frame_t = now
            if dt > 0:
                fps = 0.9 * fps + 0.1 * (1.0 / dt)
            label = f"{fps:4.1f} fps   hand: {'yes' if result.hand_landmarks else 'no'}"
            cv2.putText(bgr, label, (12, 28), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 2)

            cv2.imshow("volume gesture", bgr)
            if cv2.waitKey(1) & 0xFF in (ord("q"), 27):
                break

    cap.release()
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
