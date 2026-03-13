#ifndef SMB2350_FRAMEBUFFER_H
#define SMB2350_FRAMEBUFFER_H

#include <stdint.h>

#define NES_FRAME_WIDTH 256
#define NES_FRAME_HEIGHT 240

typedef struct {
    uint64_t frame_index;
    uint8_t pixels[NES_FRAME_WIDTH * NES_FRAME_HEIGHT];
} NesFrameBuffer;

static inline uint8_t *nes_framebuffer_scanline(NesFrameBuffer *framebuffer, uint16_t y) {
    return &framebuffer->pixels[(uint32_t)y * NES_FRAME_WIDTH];
}

static inline const uint8_t *nes_framebuffer_scanline_const(const NesFrameBuffer *framebuffer, uint16_t y) {
    return &framebuffer->pixels[(uint32_t)y * NES_FRAME_WIDTH];
}

#endif
