"""ctypes binding to core/build/libgesture.dylib.

Everything here is plumbing: struct layouts that mirror the C headers, function
signatures, and a thin wrapper that owns the C context. No gesture logic.

The struct definitions below MUST match core/include/gesture.h and step.h
field for field. ctypes cannot check that, so the library exports the sizes it
was compiled with and `_check_abi` compares them at import time; a mismatch
would otherwise show up as quietly corrupted values.
"""

import ctypes as C
from pathlib import Path

LIB_PATH = Path(__file__).resolve().parents[1] / "core" / "build" / "libgesture.dylib"

NUM_LANDMARKS = 21
IDLE, ARMING, ARMED = 0, 1, 2
STATE_NAMES = {IDLE: "idle", ARMING: "arming", ARMED: "armed"}
AXIS_NONE, AXIS_X, AXIS_Y = 0, 1, 2
AXIS_NAMES = {AXIS_NONE: None, AXIS_X: "x", AXIS_Y: "y"}
KIND_NONE, KIND_VOLUME, KIND_RESIZE = 0, 1, 2
KIND_NAMES = {KIND_NONE: None, KIND_VOLUME: "volume", KIND_RESIZE: "resize"}
VOL_OK = 0
WIN_OK = 0
WIN_ERR_AX = -5


class Point(C.Structure):
    _fields_ = [("x", C.c_float), ("y", C.c_float), ("z", C.c_float)]


class Frame(C.Structure):
    _fields_ = [
        ("lm", Point * NUM_LANDMARKS),
        ("present", C.c_int),
        ("handedness", C.c_int),
        ("t_sec", C.c_double),
    ]


class Output(C.Structure):
    _fields_ = [("state", C.c_int), ("volume", C.c_float), ("changed", C.c_int), ("axis", C.c_int),
                ("kind", C.c_int), ("scale", C.c_float)]


class PoseLimits(C.Structure):
    _fields_ = [
        ("extend_reach", C.c_float),
        ("extend_straight", C.c_float),
        ("curl_reach", C.c_float),
        ("curl_straight", C.c_float),
        ("thumb_extend_reach", C.c_float),
        ("thumb_extend_straight", C.c_float),
        ("thumb_curl_reach", C.c_float),
        ("thumb_curl_straight", C.c_float),
        ("pinch_gap_min", C.c_float),
        ("thumb_clear_min", C.c_float),
    ]


class Config(C.Structure):
    _fields_ = [
        ("aspect", C.c_float),
        ("pose", PoseLimits),
        ("arm_hold_sec", C.c_float),
        ("release_grace_sec", C.c_float),
        ("gain", C.c_float),
        ("gain_fast", C.c_float),
        ("accel_speed_lo", C.c_float),
        ("accel_speed_hi", C.c_float),
        ("edge_margin", C.c_float),
        ("edge_rate", C.c_float),
        ("axis_lock_hands", C.c_float),
        ("axis_dominance", C.c_float),
        ("dead_band", C.c_float),
        ("max_slew", C.c_float),
        ("gate_scale_min", C.c_float),
        ("gate_scale_max", C.c_float),
        ("gate_max_speed", C.c_float),
        ("filter_min_cutoff", C.c_float),
        ("filter_beta", C.c_float),
        ("filter_d_cutoff", C.c_float),
        ("scale_min", C.c_float),
        ("scale_max", C.c_float),
        ("scale_dead_band", C.c_float),
        ("scale_max_slew", C.c_float),
        ("release_rollback_sec", C.c_float),
    ]


class ReplaySummary(C.Structure):
    """gv_replay_summary (core/src/verdict.h): what a replay observed."""
    _fields_ = [("volume_delta", C.c_float), ("volume_changes", C.c_int),
                ("resize_changes", C.c_int), ("final_scale", C.c_float)]


class StepResult(C.Structure):
    _fields_ = [("out", Output), ("vol", C.c_int), ("win", C.c_int)]


def _load() -> C.CDLL:
    if not LIB_PATH.exists():
        raise SystemExit(f"{LIB_PATH} not found. Build it with `make -C core`.")
    lib = C.CDLL(str(LIB_PATH))

    # Without restype/argtypes ctypes assumes every function takes and returns
    # int, which truncates pointers and mangles floats and structs.
    lib.gv_default_config.restype = Config
    lib.gv_default_config.argtypes = []
    lib.gv_create.restype = C.c_void_p
    lib.gv_create.argtypes = [C.POINTER(Config)]
    lib.gv_destroy.restype = None
    lib.gv_destroy.argtypes = [C.c_void_p]
    lib.gv_set_current_volume.restype = None
    lib.gv_set_current_volume.argtypes = [C.c_void_p, C.c_float]
    lib.gv_update.restype = Output
    lib.gv_update.argtypes = [C.c_void_p, C.POINTER(Frame)]
    lib.gv_step_system.restype = StepResult
    lib.gv_step_system.argtypes = [C.c_void_p, C.POINTER(Frame)]
    lib.gv_hand_scale.restype = C.c_float
    lib.gv_hand_scale.argtypes = [C.POINTER(Frame), C.c_float]
    lib.vol_strerror.restype = C.c_char_p
    lib.vol_strerror.argtypes = [C.c_int]
    lib.vol_device_name.restype = C.c_int
    lib.vol_device_name.argtypes = [C.c_char_p, C.c_size_t]
    lib.gv_expectation_parse.restype = C.c_int
    lib.gv_expectation_parse.argtypes = [C.c_char_p, C.POINTER(C.c_int)]
    lib.gv_verdict.restype = C.c_int
    lib.gv_verdict.argtypes = [C.c_int, C.POINTER(ReplaySummary)]
    lib.win_strerror.restype = C.c_char_p
    lib.win_strerror.argtypes = [C.c_int]
    lib.win_last_ax_error.restype = C.c_int
    lib.win_last_ax_error.argtypes = []
    lib.win_target_name.restype = C.c_int
    lib.win_target_name.argtypes = [C.c_char_p, C.c_size_t]
    for fn in (lib.gv_abi_frame_size, lib.gv_abi_config_size, lib.gv_abi_step_result_size):
        fn.restype = C.c_uint
        fn.argtypes = []
    return lib


def _check_abi(lib: C.CDLL) -> None:
    pairs = [
        ("gv_frame", lib.gv_abi_frame_size(), C.sizeof(Frame)),
        ("gv_config", lib.gv_abi_config_size(), C.sizeof(Config)),
        ("gv_step_result", lib.gv_abi_step_result_size(), C.sizeof(StepResult)),
    ]
    bad = [f"{name}: C has {c} bytes, Python has {py}" for name, c, py in pairs if c != py]
    if bad:
        raise SystemExit("bridge.py is out of step with the C headers:\n  " + "\n  ".join(bad))


_lib = _load()
_check_abi(_lib)


def strerror(status: int) -> str:
    return _lib.vol_strerror(status).decode()


def device_name() -> tuple[int, str]:
    buf = C.create_string_buffer(128)
    status = _lib.vol_device_name(buf, len(buf))
    return status, buf.value.decode(errors="replace")


def verdict(expect: str, volume_delta: float, volume_changes: int, resize_changes: int, final_scale: float) -> bool:
    """Did a replay meet its manifest expectation (up|down|grow|shrink|none)? The rule is C's."""
    code = C.c_int()
    if not _lib.gv_expectation_parse(expect.encode(), C.byref(code)):
        raise ValueError(f"unknown expectation {expect!r}")
    seen = ReplaySummary(volume_delta, volume_changes, resize_changes, final_scale)
    return bool(_lib.gv_verdict(code, C.byref(seen)))


def win_strerror(status: int) -> str:
    return _lib.win_strerror(status).decode()


def win_last_ax_error() -> int:
    """The raw AXError behind the last WIN_ERR_AX, for diagnosis."""
    return _lib.win_last_ax_error()


def win_target_name() -> tuple[int, str]:
    """The app whose window a resize holds. When none is held the status is -2, and the
    name is the app a failed attempt tried (so an error can name it), or empty."""
    buf = C.create_string_buffer(128)
    status = _lib.win_target_name(buf, len(buf))
    return status, buf.value.decode(errors="replace")


def _fill(frame: Frame, landmarks, t_sec: float, handedness: int = 0) -> None:
    frame.t_sec = t_sec
    frame.present = 1 if landmarks else 0
    frame.handedness = handedness if landmarks else 0
    if landmarks:
        for i, p in enumerate(landmarks):
            frame.lm[i].x, frame.lm[i].y, frame.lm[i].z = p.x, p.y, p.z


class Gesture:
    """Owns one C gv_ctx. Use as a context manager so it is always destroyed."""

    def __init__(self, aspect: float, **overrides: float):
        """overrides: top-level gv_config fields by name, e.g. gain=0.15. They
        are passed through untouched; the C side validates the whole config."""
        cfg = _lib.gv_default_config()
        cfg.aspect = aspect
        for name, value in overrides.items():
            if not hasattr(cfg, name):
                raise ValueError(f"gv_config has no field {name!r}")
            setattr(cfg, name, value)
        self._ctx = _lib.gv_create(C.byref(cfg))
        if not self._ctx:
            raise ValueError("gv_create rejected the config (or ran out of memory); see gesture.h")
        self.config = cfg
        self._aspect = aspect
        self._frame = Frame()  # reused every call; the C side only reads it

    def __enter__(self) -> "Gesture":
        return self

    def __exit__(self, *exc) -> None:
        _lib.gv_destroy(self._ctx)
        self._ctx = None

    def step(self, landmarks, t_sec: float, handedness: int = 0) -> StepResult:
        """One camera frame: sync with system volume, update, apply.
        handedness: +1 right, -1 left, 0 unknown, as the landmark model labels it."""
        _fill(self._frame, landmarks, t_sec, handedness)
        return _lib.gv_step_system(self._ctx, C.byref(self._frame))

    def update(self, landmarks, t_sec: float, handedness: int = 0) -> Output:
        """The pure state machine only; never touches the system volume."""
        _fill(self._frame, landmarks, t_sec, handedness)
        return _lib.gv_update(self._ctx, C.byref(self._frame))

    def set_current_volume(self, volume: float) -> None:
        _lib.gv_set_current_volume(self._ctx, volume)

    def hand_scale(self, landmarks) -> float:
        frame = Frame()
        _fill(frame, landmarks, 0.0)
        return _lib.gv_hand_scale(C.byref(frame), self._aspect)
