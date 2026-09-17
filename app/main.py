"""Webcam -> MediaPipe hand landmarks -> C gesture core -> system volume.

This file is a pipe. It moves frames from the camera to MediaPipe, landmarks
from MediaPipe to the C library, and the C library's answer to the screen. All
gesture logic and the volume call itself live in core/ (C).

    make run                      run with the preview window and the HUD
    make run ARGS="--record traces/slide_up.csv"

Quit with q or Esc in the preview window, or Ctrl-C.
"""

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

import cv2
import mediapipe as mp
from mediapipe.tasks.python import BaseOptions, vision

import bridge

ROOT = Path(__file__).resolve().parents[1]
MODEL_PATH = ROOT / "app" / "models" / "hand_landmarker.task"
HUD_PATH = ROOT / "hud" / "build" / "volume-hud"
HUD_STALL_SEC = 2.0
CONNECTIONS = [(c.start, c.end) for c in vision.HandLandmarksConnections.HAND_CONNECTIONS]
STATE_COLOURS = {bridge.IDLE: (160, 160, 160), bridge.ARMING: (0, 200, 255), bridge.ARMED: (80, 220, 80)}


def make_landmarker(image_mode: bool = False) -> vision.HandLandmarker:
    if not MODEL_PATH.exists():
        sys.exit(f"Model file missing: {MODEL_PATH}\nRun `make model` to download it.")
    options = vision.HandLandmarkerOptions(
        base_options=BaseOptions(
            model_asset_path=str(MODEL_PATH),
            # CPU, not GPU: on macOS the Metal delegate keeps a pixel buffer per
            # frame until the landmarker closes, and the process dies after a
            # few minutes. The CPU delegate costs 4.5 ms a frame and stays flat.
            # requirements.txt has the full story; app/test_leak.py guards it.
            delegate=BaseOptions.Delegate.CPU,
        ),
        running_mode=vision.RunningMode.IMAGE if image_mode else vision.RunningMode.VIDEO,
        num_hands=1,
    )
    return vision.HandLandmarker.create_from_options(options)


CAPTURE_SIZE = (1280, 720)
PREVIEW_WIDTH = 960


def detect(landmarker: vision.HandLandmarker, bgr, ts_ms: int):
    """One frame through MediaPipe; returns the 21 landmarks of the first hand, or None.
    The one place a frame is handed to MediaPipe, shared by the camera loop and the leak test."""
    rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
    result = landmarker.detect_for_video(mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb), ts_ms)
    return result.hand_landmarks[0] if result.hand_landmarks else None


def detect_photo(landmarker: vision.HandLandmarker, bgr):
    """As detect(), for a landmarker made with image_mode=True (tests and fixture tools)."""
    rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
    result = landmarker.detect(mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb))
    return result.hand_landmarks[0] if result.hand_landmarks else None


def open_camera(index: int) -> cv2.VideoCapture:
    cap = cv2.VideoCapture(index)
    # The camera defaults to 1920x1080. MediaPipe shrinks every frame to a few
    # hundred pixels before looking at it, so the extra resolution buys nothing
    # and costs a flip, a colour conversion and a window redraw per frame: the
    # first live session ran at 21 fps with the preview against 27 without.
    # This is a request; the camera may answer with something else, and the
    # aspect ratio is read from the frames actually delivered.
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, CAPTURE_SIZE[0])
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, CAPTURE_SIZE[1])
    ok, _ = cap.read() if cap.isOpened() else (False, None)
    if not ok:
        cap.release()
        sys.exit(
            "Could not read from the camera.\n"
            "macOS grants camera access per app: allow it for the app hosting this\n"
            "terminal (VS Code or Terminal) under System Settings > Privacy & Security >\n"
            "Camera, then restart that app."
        )
    return cap


class Hud:
    """Feeds the native HUD process one JSON line per frame. Optional: if the
    binary is not built, or it exits, the app carries on without it."""

    def __init__(self, enabled: bool, opacity: float | None = None, screen: str | None = None):
        self._proc = None
        if not enabled:
            return
        if not HUD_PATH.exists():
            print(f"HUD not built ({HUD_PATH.relative_to(ROOT)}); run `make -C hud`. Continuing without it.")
            return
        command = [str(HUD_PATH)]
        if opacity is not None:
            command += ["--opacity", str(opacity)]  # validated and clamped by the HUD itself
        if screen is not None:
            command += ["--screen", screen]
        self._proc = subprocess.Popen(command, stdin=subprocess.PIPE)
        # Non-blocking: a HUD that is alive but has stopped reading would
        # otherwise fill the 64 KB pipe and then block write() forever, freezing
        # the camera loop with it. Each line is far below PIPE_BUF (512 bytes),
        # so a write either goes through whole or raises BlockingIOError; it can
        # never leave half a JSON line in the pipe.
        self._fd = self._proc.stdin.fileno()
        os.set_blocking(self._fd, False)
        self._blocked_since = None

    def send(self, state: int, volume: float, error: str | None, axis: int = bridge.AXIS_NONE) -> None:
        if self._proc is None:
            return
        message = {"state": bridge.STATE_NAMES[state], "volume": round(volume, 4), "error": error,
                   "axis": bridge.AXIS_NAMES[axis]}
        line = json.dumps(message) + "\n"
        try:
            os.write(self._fd, line.encode())
            self._blocked_since = None
        except BlockingIOError:
            # The HUD is not keeping up. Drop this frame's line (the next one
            # supersedes it anyway) and give up if it stays stuck.
            now = time.monotonic()
            if self._blocked_since is None:
                self._blocked_since = now
            elif now - self._blocked_since > HUD_STALL_SEC:
                print(f"HUD stopped reading for {HUD_STALL_SEC:.0f} s; continuing without it.")
                self._terminate()
        except OSError:  # includes BrokenPipeError: the HUD exited
            print("HUD process exited; continuing without it.")
            self._terminate()

    def close(self) -> None:
        if self._proc is None:
            return
        try:
            self._proc.stdin.close()  # EOF is the HUD's signal to quit
            self._proc.wait(timeout=2)
            self._proc = None
        except (OSError, subprocess.TimeoutExpired):
            self._terminate()

    def _terminate(self) -> None:
        self._proc.kill()
        self._proc.wait()  # reap it; a killed child is a zombie until waited for
        self._proc = None


class Recorder:
    """Writes two files per session:

    <name>.csv         what the camera saw: every frame's landmarks, the input
                       format of core/tools/replay.c.
    <name>.events.csv  what the app did with it: state, target volume and the
                       status of that frame's volume call. The first live
                       session could not answer "did the writes fail, or were
                       they never made?" because only the first file existed.
    """

    def __init__(self, path: str | None, aspect: float):
        self._file = self._events = None
        if path is None:
            return
        trace = Path(path)
        trace.parent.mkdir(parents=True, exist_ok=True)
        self._file = open(trace, "w")
        self._file.write(f"# aspect={aspect:.6f}\n")
        cols = ",".join(f"x{i},y{i},z{i}" for i in range(bridge.NUM_LANDMARKS))
        self._file.write(f"t_sec,present,{cols}\n")
        self._events_path = trace.with_suffix(".events.csv")  # created on the first event

    def write(self, t_sec: float, landmarks, step=None) -> None:
        if self._file is None:
            return
        if landmarks:
            values = ",".join(f"{p.x:.6f},{p.y:.6f},{p.z:.6f}" for p in landmarks)
        else:
            values = ",".join(["0"] * (3 * bridge.NUM_LANDMARKS))
        self._file.write(f"{t_sec:.6f},{1 if landmarks else 0},{values}\n")
        if step is not None:
            if self._events is None:  # tools that record landmarks only leave no empty file behind
                self._events = open(self._events_path, "w")
                self._events.write("t_sec,state,axis,target_volume,changed,vol_status,vol_status_text\n")
            self._events.write(
                f"{t_sec:.6f},{bridge.STATE_NAMES[step.out.state]},{bridge.AXIS_NAMES[step.out.axis] or '-'},"
                f"{step.out.volume:.4f},"
                f"{step.out.changed},{step.vol},{bridge.strerror(step.vol)}\n"
            )

    def close(self) -> None:
        for f in (self._file, self._events):
            if f is not None:
                f.close()
        self._file = self._events = None


AXIS_HINTS = {bridge.AXIS_NONE: "move left/right or up/down", bridge.AXIS_X: "left / right", bridge.AXIS_Y: "up / down"}


def draw_overlay(frame, landmarks, state: int, volume: float, error: str | None, fps: float,
                 axis: int = bridge.AXIS_NONE) -> None:
    h, w = frame.shape[:2]
    colour = STATE_COLOURS[state]
    if landmarks:
        pts = [(int(p.x * w), int(p.y * h)) for p in landmarks]
        for a, b in CONNECTIONS:
            cv2.line(frame, pts[a], pts[b], colour, 2)
        for x, y in pts:
            cv2.circle(frame, (x, y), 4, (255, 255, 255), -1)

    label = f"{bridge.STATE_NAMES[state].upper()}  {fps:4.1f} fps"
    if state == bridge.ARMED:
        label += f"   {AXIS_HINTS[axis]}"
    cv2.putText(frame, label, (12, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.8, colour, 2)

    # The bar is blended over the camera picture rather than painted on it, so
    # the hand stays visible behind it. (The real glass is the native HUD; this
    # is the debug view.)
    bar_w, bar_h, x0, y0 = w - 24, 10, 12, h - 26
    glass = frame.copy()
    cv2.rectangle(glass, (x0, y0), (x0 + bar_w, y0 + bar_h), (255, 255, 255), -1)
    cv2.addWeighted(glass, 0.18, frame, 0.82, 0, dst=frame)
    glass = frame.copy()
    cv2.rectangle(glass, (x0, y0), (x0 + int(bar_w * volume), y0 + bar_h), colour, -1)
    cv2.addWeighted(glass, 0.65, frame, 0.35, 0, dst=frame)
    cv2.putText(frame, f"{round(volume * 100)}%", (x0, y0 - 8), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 1,
                cv2.LINE_AA)

    if error:
        # A failed volume write means the whole app is doing nothing useful, so
        # it gets a banner across the frame rather than a line in the corner.
        cv2.rectangle(frame, (0, 44), (w, 44 + 56), (40, 40, 200), -1)
        cv2.putText(frame, "VOLUME NOT CHANGING", (12, 68), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (255, 255, 255), 2)
        cv2.putText(frame, error, (12, 92), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (255, 255, 255), 1)


def draw_prompt(frame, prompt: str, remaining: float, recording: bool) -> None:
    """Guided recording: what to do and for how long, across the bottom third of the
    preview where it does not cover a raised hand."""
    h, w = frame.shape[:2]
    top, bottom = h - 140, h - 62  # clear of the volume bar and its percentage
    shade = frame.copy()
    cv2.rectangle(shade, (0, top), (w, bottom), (120, 60, 20) if recording else (40, 40, 40), -1)
    cv2.addWeighted(shade, 0.75, frame, 0.25, 0, dst=frame)
    head = f"RECORDING  {remaining:4.0f}s left" if recording else f"GET READY  starts in {remaining:3.0f}s"
    cv2.putText(frame, head, (12, top + 26), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 2, cv2.LINE_AA)
    cv2.putText(frame, prompt, (12, top + 58), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 1, cv2.LINE_AA)


class ErrorLatch:
    """Keeps the last error on screen for a moment. Volume status only appears
    on the frames that actually wrote, so without this the banner would blink
    on and off at frame rate. Display persistence only; nothing here decides
    anything about the gesture."""

    HOLD_SEC = 2.0

    def __init__(self):
        self._text = None
        self._until = 0.0

    def update(self, error: str | None, now: float) -> str | None:
        if error:
            self._text, self._until = error, now + self.HOLD_SEC
        return self._text if now < self._until else None


def describe_error(status: int) -> str | None:
    if status == bridge.VOL_OK:
        return None
    _, name = bridge.device_name()
    return f"{bridge.strerror(status)} ({name.strip()})" if name.strip() else bridge.strerror(status)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--camera", type=int, default=0, help="camera index (default 0)")
    parser.add_argument("--record", metavar="CSV", help="also write landmarks to this file")
    parser.add_argument("--gain", type=float, help="volume per hand-length when moving slowly (default 0.15)")
    parser.add_argument("--gain-fast", type=float, help="volume per hand-length when moving fast (default 0.6)")
    parser.add_argument("--arm-hold", type=float, metavar="SEC", help="how long the pose must hold to arm (default 0.25)")
    parser.add_argument("--grace", type=float, metavar="SEC", help="how long the pose may be missing before release (default 0.25)")
    parser.add_argument("--duration", type=float, metavar="SEC", help="quit by itself after this long (guided recording)")
    parser.add_argument("--lead-in", type=float, metavar="SEC", default=0.0,
                        help="show the prompt for this long before recording and gesture control start")
    parser.add_argument("--prompt", help="instruction shown on the preview during a guided recording")
    parser.add_argument("--no-hud", action="store_true", help="do not launch the native HUD")
    parser.add_argument("--hud-opacity", type=float, metavar="0.15-1.0",
                        help="how solid the HUD's glass is (default 0.6; lower is clearer)")
    parser.add_argument("--hud-screen", choices=["pointer", "primary"],
                        help="which display the HUD appears on: the one the mouse pointer is on (default), "
                             "or always the primary display")
    parser.add_argument("--no-preview", action="store_true", help="do not show the camera preview window (quit with Ctrl-C)")
    args = parser.parse_args()

    cap = open_camera(args.camera)
    hud = recorder = None
    try:
        ok, first = cap.read()
        if not ok:
            sys.exit("Camera stopped delivering frames.")
        aspect = first.shape[1] / first.shape[0]

        hud = Hud(enabled=not args.no_hud, opacity=args.hud_opacity, screen=args.hud_screen)
        recorder = Recorder(args.record, aspect)
        start = time.monotonic()
        last_ts_ms, fps, last_frame_t = -1, 0.0, start
        latch = ErrorLatch()

        # Tuning flags go straight into the C config; nothing is interpreted here.
        flags = {"gain": args.gain, "gain_fast": args.gain_fast, "arm_hold_sec": args.arm_hold,
                 "release_grace_sec": args.grace}
        overrides = {k: v for k, v in flags.items() if v is not None}

        with make_landmarker() as landmarker, bridge.Gesture(aspect, **overrides) as gesture:
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

                landmarks = detect(landmarker, bgr, ts_ms)

                # Guided recording: nothing is recorded and nothing can move the
                # volume until the lead-in is over, so a clip holds only the
                # behaviour it was asked for, and its clock starts at zero.
                t_sec = now - start - args.lead_in
                live = t_sec >= 0.0
                if args.duration is not None and t_sec >= args.duration:
                    break
                if live:
                    step = gesture.step(landmarks, t_sec)
                    error = latch.update(describe_error(step.vol), now)
                    recorder.write(t_sec, landmarks, step)
                    hud.send(step.out.state, step.out.volume, error, step.out.axis)

                dt = now - last_frame_t
                last_frame_t = now
                if dt > 0:
                    fps = 0.9 * fps + 0.1 * (1.0 / dt)

                if args.no_preview:
                    continue
                if bgr.shape[1] > PREVIEW_WIDTH:
                    scale = PREVIEW_WIDTH / bgr.shape[1]
                    bgr = cv2.resize(bgr, None, fx=scale, fy=scale, interpolation=cv2.INTER_AREA)
                if live:
                    draw_overlay(bgr, landmarks, step.out.state, step.out.volume, error, fps, step.out.axis)
                if args.prompt:
                    remaining = (args.duration - t_sec) if (live and args.duration) else -t_sec
                    draw_prompt(bgr, args.prompt, max(remaining, 0.0), recording=live)
                cv2.imshow("volume gesture", bgr)
                if cv2.waitKey(1) & 0xFF in (ord("q"), 27):
                    break
    except KeyboardInterrupt:
        pass
    finally:
        # Runs on every way out, including sys.exit and Ctrl-C.
        if recorder is not None:
            recorder.close()
        if hud is not None:
            hud.close()
        cap.release()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
