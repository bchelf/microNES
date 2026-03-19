#ifndef MICRONES_SCANLINE_H
#define MICRONES_SCANLINE_H

#include "framebuffer.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t frame_index;
    uint16_t y;
    bool ready;
    uint8_t pixels[NES_FRAME_WIDTH];
} NesScanline;

#endif
