/*
 * vol - tiny command-line front end to volume.h.
 *
 *   vol get          print the output volume, 0.00 - 1.00
 *   vol set 0.35     set it
 *   vol name         print the output device's name
 *
 * Exists so the CoreAudio code can be exercised, and stepped through in the
 * debugger, without the camera or Python anywhere in the picture.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "volume.h"

static int fail(vol_status s)
{
    char name[128];
    if (vol_device_name(name, sizeof name) == VOL_OK)
        fprintf(stderr, "vol: %s (output: %s)", vol_strerror(s), name);
    else
        fprintf(stderr, "vol: %s", vol_strerror(s));
    if (s == VOL_ERR_OS)
        fprintf(stderr, " [OSStatus %d]", vol_last_os_status());
    fputc('\n', stderr);
    return 1;
}

static int usage(void)
{
    fputs("usage: vol get | vol set <0..1> | vol name\n", stderr);
    return 2;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "get") == 0) {
        float v;
        vol_status s = vol_get(&v);
        if (s != VOL_OK)
            return fail(s);
        printf("%.2f\n", v);
        return 0;
    }

    if (argc == 3 && strcmp(argv[1], "set") == 0) {
        /* strtof reports where it stopped parsing; if that is not the end of
         * the string, the argument was not a clean number ("0.5abc", ""). */
        char *end;
        errno = 0;
        float v = strtof(argv[2], &end);
        if (errno != 0 || end == argv[2] || *end != '\0')
            return usage();
        vol_status s = vol_set(v);
        return s == VOL_OK ? 0 : fail(s);
    }

    if (argc == 2 && strcmp(argv[1], "name") == 0) {
        char name[128];
        vol_status s = vol_device_name(name, sizeof name);
        if (s != VOL_OK)
            return fail(s);
        puts(name);
        return 0;
    }

    return usage();
}
