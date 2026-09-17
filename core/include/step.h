/*
 * step.h - one call per camera frame: the glue between the pure gesture core
 * and the side-effecting volume module.
 *
 * The volume backend is passed in as a pair of function pointers instead of
 * being called directly. In production that pair is the real CoreAudio
 * functions (gv_system_volume); in tests it is a fake that records calls, so
 * the glue can be tested without touching the machine's actual volume. This
 * is C's version of dependency injection.
 */
#ifndef STEP_H
#define STEP_H

#include "gesture.h"
#include "volume.h"

typedef struct {
    /* `vol_status (*get)(float *)` reads: get is a pointer to a function
     * taking float* and returning vol_status. */
    vol_status (*get)(float *out);
    vol_status (*set)(float v);
} gv_volume_ops;

/* The real backend: { vol_get, vol_set }. */
extern const gv_volume_ops gv_system_volume;

typedef struct {
    gv_output  out;
    vol_status vol; /* result of this frame's volume call; VOL_OK if none failed */
} gv_step_result;

/* Sync the core with the current system volume, feed it the frame, and apply
 * the new volume if it changed. */
gv_step_result gv_step(gv_ctx *ctx, const gv_frame *frame, const gv_volume_ops *ops);

/* gv_step with the real backend; what the Python side calls. */
gv_step_result gv_step_system(gv_ctx *ctx, const gv_frame *frame);

/* sizeof the structs that cross the FFI boundary. The Python side compares
 * these with its own ctypes definitions at load time, because a layout
 * mismatch between the two otherwise shows up as silently corrupted values. */
unsigned gv_abi_frame_size(void);
unsigned gv_abi_config_size(void);
unsigned gv_abi_step_result_size(void);

#endif /* STEP_H */
