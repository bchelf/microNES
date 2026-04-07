#ifndef MICRONES_EMULATOR_VIDEO_ADAPTER_H
#define MICRONES_EMULATOR_VIDEO_ADAPTER_H

#include "nes.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool initialized;
    bool debug_overlay_enabled;
    uint64_t rendered_frames;
    uint64_t rendered_scanlines;
    uint64_t profile_render_frame_us_total;
    uint64_t profile_step_scanline_us_total;
    // Convert timing is now measured on core 1; this field stays zero so the
    // existing reporting format in main.c continues to compile unchanged.
    uint64_t profile_convert_scanline_us_total;
    uint64_t frames_with_visible_output;
    uint32_t last_frame_source_nonzero_pixels;
    uint32_t last_frame_visible_nonblack_pixels;
    uint32_t last_frame_visible_white_pixels;
    uint32_t last_frame_visible_gray_pixels;
    uint8_t last_frame_min_pixel;
    uint8_t last_frame_max_pixel;
    uint8_t last_frame_unique_color_count;
    int16_t last_frame_first_visible_x;
    int16_t last_frame_first_visible_y;
    char last_error[160];
    Nes nes;
} PicoEmulatorVideoAdapter;

// NES palette-to-gray mapping shared with core1_video for composite conversion.
extern const uint8_t k_emulator_video_palette_to_gray[64];

bool emulator_video_adapter_init(
    PicoEmulatorVideoAdapter *adapter,
    const uint8_t *rom_image,
    size_t rom_image_size
);
bool emulator_video_adapter_render_frame(PicoEmulatorVideoAdapter *adapter);
bool emulator_video_adapter_step_frame(PicoEmulatorVideoAdapter *adapter);
void emulator_video_adapter_present_frame(PicoEmulatorVideoAdapter *adapter);
const char *emulator_video_adapter_last_error(const PicoEmulatorVideoAdapter *adapter);

#endif
