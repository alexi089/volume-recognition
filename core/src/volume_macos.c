/*
 * volume_macos.c - system output volume through CoreAudio.
 *
 * CoreAudio models everything as "objects" with "properties". A property is
 * addressed by three numbers (selector, scope, element) and read or written as
 * raw bytes with AudioObjectGetPropertyData / AudioObjectSetPropertyData. There
 * is no vol_get() in the OS; we ask the system object for the default output
 * device, then ask that device for its "virtual main volume" property.
 *
 * "Virtual main volume" is the same value the volume keys and the menu-bar
 * slider control. It exists only on devices with software volume, so HDMI /
 * DisplayPort monitors and some USB DACs do not have it.
 */
#include "volume.h"

#include <math.h>

#include <AudioToolbox/AudioHardwareService.h> /* ..._VirtualMainVolume */
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

/* How far the read-back value may sit from the requested one before we call
 * the write ignored. Devices may legitimately snap to coarse steps (1/16 =
 * 0.0625 is common), so the tolerance has to be wider than one such step. */
#define VOL_IGNORED_TOLERANCE 0.08f

/* `static` at file scope means "private to this .c file": the linker will not
 * expose these names, so they cannot collide with anything else in the program. */
static OSStatus g_last_os_status = noErr;

static const AudioObjectPropertyAddress k_default_output_addr = {
    kAudioHardwarePropertyDefaultOutputDevice,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain,
};

static const AudioObjectPropertyAddress k_volume_addr = {
    kAudioHardwareServiceDeviceProperty_VirtualMainVolume,
    kAudioDevicePropertyScopeOutput,
    kAudioObjectPropertyElementMain,
};

static const AudioObjectPropertyAddress k_name_addr = {
    kAudioObjectPropertyName,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain,
};

static vol_status default_output_device(AudioDeviceID *out)
{
    AudioDeviceID dev = kAudioObjectUnknown;
    UInt32 size = sizeof dev;

    OSStatus os = AudioObjectGetPropertyData(kAudioObjectSystemObject, &k_default_output_addr,
                                             0, NULL, &size, &dev);
    if (os != noErr) {
        g_last_os_status = os;
        return VOL_ERR_OS;
    }
    if (dev == kAudioObjectUnknown)
        return VOL_ERR_NO_DEVICE;

    *out = dev;
    return VOL_OK;
}

vol_status vol_get(float *out)
{
    if (out == NULL)
        return VOL_ERR_BAD_VALUE;

    AudioDeviceID dev;
    vol_status s = default_output_device(&dev);
    if (s != VOL_OK)
        return s;

    if (!AudioObjectHasProperty(dev, &k_volume_addr))
        return VOL_ERR_NO_SOFTWARE_VOLUME;

    Float32 v = 0.0f;
    UInt32 size = sizeof v;
    OSStatus os = AudioObjectGetPropertyData(dev, &k_volume_addr, 0, NULL, &size, &v);
    if (os != noErr) {
        g_last_os_status = os;
        return VOL_ERR_OS;
    }

    *out = v;
    return VOL_OK;
}

vol_status vol_set(float v)
{
    /* NaN compares false against everything, so a clamp would let it through. */
    if (isnan(v))
        return VOL_ERR_BAD_VALUE;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;

    AudioDeviceID dev;
    vol_status s = default_output_device(&dev);
    if (s != VOL_OK)
        return s;

    if (!AudioObjectHasProperty(dev, &k_volume_addr))
        return VOL_ERR_NO_SOFTWARE_VOLUME;

    /* "The query failed" and "the answer was no" are different problems and
     * get different codes; folding them together would hide a real CoreAudio
     * error behind a message about the device. */
    Boolean settable = false;
    OSStatus os = AudioObjectIsPropertySettable(dev, &k_volume_addr, &settable);
    if (os != noErr) {
        g_last_os_status = os;
        return VOL_ERR_OS;
    }
    if (!settable)
        return VOL_ERR_NO_SOFTWARE_VOLUME;

    Float32 value = v;
    os = AudioObjectSetPropertyData(dev, &k_volume_addr, 0, NULL, sizeof value, &value);
    if (os != noErr) {
        g_last_os_status = os;
        return VOL_ERR_OS;
    }

    /* noErr does not mean the device applied the value, so read it back. */
    Float32 actual = 0.0f;
    UInt32 size = sizeof actual;
    os = AudioObjectGetPropertyData(dev, &k_volume_addr, 0, NULL, &size, &actual);
    if (os != noErr) {
        g_last_os_status = os;
        return VOL_ERR_OS;
    }
    if (fabsf(actual - value) > VOL_IGNORED_TOLERANCE)
        return VOL_ERR_IGNORED;

    return VOL_OK;
}

vol_status vol_device_name(char *buf, size_t len)
{
    if (buf == NULL || len == 0)
        return VOL_ERR_BAD_VALUE;
    buf[0] = '\0';

    AudioDeviceID dev;
    vol_status s = default_output_device(&dev);
    if (s != VOL_OK)
        return s;

    /* The name comes back as a CFStringRef we own ("Get" here follows the
     * CoreAudio rule, not the CoreFoundation one), so it must be released. */
    CFStringRef name = NULL;
    UInt32 size = sizeof name;
    OSStatus os = AudioObjectGetPropertyData(dev, &k_name_addr, 0, NULL, &size, &name);
    if (os != noErr) {
        g_last_os_status = os;
        return VOL_ERR_OS;
    }
    if (name == NULL)
        return VOL_OK; /* the device simply has no name; buf is already "" */

    Boolean ok = CFStringGetCString(name, buf, (CFIndex)len, kCFStringEncodingUTF8);
    CFRelease(name);
    if (!ok) {
        buf[0] = '\0';
        return VOL_ERR_OS;
    }
    return VOL_OK;
}

const char *vol_strerror(vol_status s)
{
    switch (s) {
    case VOL_OK:                     return "ok";
    case VOL_ERR_NO_DEVICE:          return "no default audio output device";
    case VOL_ERR_NO_SOFTWARE_VOLUME: return "this output has no software volume control";
    case VOL_ERR_BAD_VALUE:          return "invalid argument";
    case VOL_ERR_OS:                 return "CoreAudio call failed";
    case VOL_ERR_IGNORED:            return "the output accepted the volume change but did not apply it";
    }
    return "unknown error";
}

int vol_last_os_status(void)
{
    return (int)g_last_os_status;
}
