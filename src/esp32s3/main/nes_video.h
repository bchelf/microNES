#pragma once

#include "framebuffer.h"
#include <stdint.h>

// Convert one NES scanline (palette indices) to RGB565 (big-endian byte
// order for direct SPI output).  dst must hold at least NES_FRAME_WIDTH
// uint16_t values.
void nes_video_convert_scanline(const uint8_t *palette_row,
                                uint16_t *dst_rgb565_be);

// Convert count NES palette indices to RGB565 (big-endian byte order).
void nes_video_convert_pixels(const uint8_t *palette_src,
                              uint16_t *dst_rgb565_be,
                              uint16_t count);
