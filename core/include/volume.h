/*
 * volume.h - get/set the system output volume.
 *
 * Kept apart from gesture.h on purpose: the gesture core is portable C with no
 * side effects, while this is the one platform-specific, side-effecting piece.
 * Porting to another OS means writing another volume_<os>.c against this header.
 */
#ifndef VOLUME_H
#define VOLUME_H

#include <stddef.h>

/* Every function returns one of these. Zero is success, negatives are errors,
 * which is the usual C convention and lets callers write `if (vol_get(&v) < 0)`. */
typedef enum {
    VOL_OK                     =  0,
    VOL_ERR_NO_DEVICE          = -1, /* no default output device */
    VOL_ERR_NO_SOFTWARE_VOLUME = -2, /* device has no settable volume (HDMI, some DACs) */
    VOL_ERR_BAD_VALUE          = -3, /* NaN passed to vol_set, or NULL out-pointer */
    VOL_ERR_OS                 = -4  /* CoreAudio call failed; see vol_last_os_status() */
} vol_status;

/* Reads the current output volume into *out as a scalar in [0, 1]. */
vol_status vol_get(float *out);

/* Sets the output volume. v is clamped to [0, 1]. */
vol_status vol_set(float v);

/* Copies the default output device's name into buf (always NUL-terminated
 * when len > 0). Used to make error messages name the actual device. */
vol_status vol_device_name(char *buf, size_t len);

/* Human-readable text for a vol_status. Never returns NULL. */
const char *vol_strerror(vol_status s);

/* The raw OSStatus from the most recent failing CoreAudio call, for diagnosis. */
int vol_last_os_status(void);

#endif /* VOLUME_H */
