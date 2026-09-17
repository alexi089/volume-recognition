#include "step.h"

gv_step_result gv_step(gv_ctx *ctx, const gv_frame *frame, const gv_volume_ops *ops)
{
    gv_step_result r = { { GV_IDLE, 0.0f, 0, GV_AXIS_NONE, GV_KIND_NONE, 1.0f }, VOL_OK };

    /* 1. Tell the core where the system volume is, so arming starts from the
     *    truth even if the volume keys were pressed since the last slide. The
     *    core ignores this while armed; asking anyway keeps this function free
     *    of any knowledge about the core's state. */
    float current;
    vol_status get_status = ops->get(&current);
    if (get_status == VOL_OK)
        gv_set_current_volume(ctx, current);

    /* 2. The pure part. */
    r.out = gv_update(ctx, frame);

    /* 3. Apply. A failed write is the more useful thing to report on a frame
     *    that wrote; otherwise report how the read went. */
    if (r.out.changed && r.out.kind == GV_KIND_VOLUME)
        r.vol = ops->set(r.out.volume); /* `changed` belongs to the active gesture; only volume's is a volume */
    else
        r.vol = get_status;

    return r;
}

/* `unsigned` is plenty for a struct size and maps to a plain c_uint in ctypes;
 * size_t would work too but differs in width between platforms. */
unsigned gv_abi_frame_size(void)       { return (unsigned)sizeof(gv_frame); }
unsigned gv_abi_config_size(void)      { return (unsigned)sizeof(gv_config); }
unsigned gv_abi_step_result_size(void) { return (unsigned)sizeof(gv_step_result); }
