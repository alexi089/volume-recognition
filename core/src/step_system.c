/*
 * step_system.c - binds the glue to the real volume backend.
 *
 * Kept out of step.c so that the test binary can link step.c alone: it brings
 * its own fake backend and must not pull in CoreAudio.
 */
#include "step.h"

/* A function's name, used without calling it, is a pointer to that function. */
const gv_volume_ops gv_system_volume = { vol_get, vol_set };

gv_step_result gv_step_system(gv_ctx *ctx, const gv_frame *frame)
{
    return gv_step(ctx, frame, &gv_system_volume);
}
