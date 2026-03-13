#ifndef SMB2350_PPU_H
#define SMB2350_PPU_H

#include "cart.h"
#include "framebuffer.h"
#include "scanline.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t ctrl;
    uint8_t mask;
    uint8_t status;
    uint8_t oam_addr;
    uint8_t oam[256];
    uint8_t nametables[2048];
    uint8_t palette[32];
    uint8_t read_buffer;
    uint16_t vram_addr;
    uint16_t temp_addr;
    uint8_t fine_x;
    bool write_toggle;
    uint8_t scroll_x;
    uint8_t scroll_y;
    int scanline;
    int cycle;
    uint64_t frame_count;
    bool frame_ready;
    bool scanline_ready;
    bool nmi_pending;
    uint64_t completed_frame_count;
    bool completed_frame_ready;
    uint64_t last_completed_frame_hash;
    uint32_t last_completed_nonzero_pixels;
    uint64_t first_nonblank_frame_index;
    uint64_t first_nonblank_frame_hash;
    bool sprite0_hit_ever;
    uint64_t sprite0_hit_count;
    uint64_t sprite0_opaque_pixel_count;
    uint64_t sprite0_background_overlap_count;
    uint64_t first_sprite0_hit_frame;
    int first_sprite0_hit_scanline;
    int first_sprite0_hit_x;
    uint64_t first_sprite0_opaque_frame;
    int first_sprite0_opaque_scanline;
    int first_sprite0_opaque_x;
    uint64_t sprite_composited_pixel_count;
    uint64_t frames_with_sprite_pixels;
    uint32_t last_completed_sprite_pixels;
    uint64_t first_frame_with_sprite_pixels;
    uint8_t max_scanline_sprite_count;
    NesFrameBuffer frame_buffer;
    NesScanline scanline_buffer;
} Ppu;

void ppu_init(Ppu *ppu);
void ppu_reset(Ppu *ppu);
void ppu_step_cycles(Ppu *ppu, NesCartridge *cartridge, uint32_t cycles);
uint8_t ppu_cpu_read(Ppu *ppu, NesCartridge *cartridge, uint16_t addr);
void ppu_cpu_write(Ppu *ppu, NesCartridge *cartridge, uint16_t addr, uint8_t value);

const NesFrameBuffer *ppu_framebuffer(const Ppu *ppu);
const NesScanline *ppu_scanline(const Ppu *ppu);

#endif
