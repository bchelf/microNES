#ifndef MICRONES_HOST_PNG_WRITE_H
#define MICRONES_HOST_PNG_WRITE_H

#include <stdbool.h>
#include <stdint.h>

bool host_write_png_gray8(
    const char *path,
    const uint8_t *pixels,
    int width,
    int height,
    int stride
);

bool host_write_png_rgb24(
    const char *path,
    const uint8_t *pixels,
    int width,
    int height,
    int stride
);

#endif
