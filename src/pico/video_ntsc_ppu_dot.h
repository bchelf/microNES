#ifndef MICRONES_VIDEO_NTSC_PPU_DOT_H
#define MICRONES_VIDEO_NTSC_PPU_DOT_H

#include <stdint.h>

void video_ntsc_ppu_dot_test_init(void);
void video_ntsc_ppu_dot_test_start(void);

void video_ntsc_ppu_dot_emulator_init(void);
void video_ntsc_ppu_dot_precompute_palette(const uint8_t *palette_to_luma, int palette_size);
void video_ntsc_ppu_dot_start(void);
void video_ntsc_ppu_dot_stop(void);
void video_ntsc_ppu_dot_core1_entry(void);

#endif
