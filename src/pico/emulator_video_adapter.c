#include "emulator_video_adapter.h"

#include "video_ntsc.h"

#include "pico/time.h"

#include <stdio.h>
#include <string.h>

static const uint8_t k_emulator_video_palette_rgb[64][3] = {
    { 0x7c, 0x7c, 0x7c }, { 0x00, 0x00, 0xfc }, { 0x00, 0x00, 0xbc }, { 0x44, 0x28, 0xbc },
    { 0x94, 0x00, 0x84 }, { 0xa8, 0x00, 0x20 }, { 0xa8, 0x10, 0x00 }, { 0x88, 0x14, 0x00 },
    { 0x50, 0x30, 0x00 }, { 0x00, 0x78, 0x00 }, { 0x00, 0x68, 0x00 }, { 0x00, 0x58, 0x00 },
    { 0x00, 0x40, 0x58 }, { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00 },

    { 0xbc, 0xbc, 0xbc }, { 0x00, 0x78, 0xf8 }, { 0x00, 0x58, 0xf8 }, { 0x68, 0x44, 0xfc },
    { 0xd8, 0x00, 0xcc }, { 0xe4, 0x00, 0x58 }, { 0xf8, 0x38, 0x00 }, { 0xe4, 0x5c, 0x10 },
    { 0xac, 0x7c, 0x00 }, { 0x00, 0xb8, 0x00 }, { 0x00, 0xa8, 0x00 }, { 0x00, 0xa8, 0x44 },
    { 0x00, 0x88, 0x88 }, { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00 },

    { 0xf8, 0xf8, 0xf8 }, { 0x3c, 0xbc, 0xfc }, { 0x68, 0x88, 0xfc }, { 0x98, 0x78, 0xf8 },
    { 0xf8, 0x78, 0xf8 }, { 0xf8, 0x58, 0x98 }, { 0xf8, 0x78, 0x58 }, { 0xfc, 0xa0, 0x44 },
    { 0xf8, 0xb8, 0x00 }, { 0xb8, 0xf8, 0x18 }, { 0x58, 0xd8, 0x54 }, { 0x58, 0xf8, 0x98 },
    { 0x00, 0xe8, 0xd8 }, { 0x78, 0x78, 0x78 }, { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00 },

    { 0xfc, 0xfc, 0xfc }, { 0xa4, 0xe4, 0xfc }, { 0xb8, 0xb8, 0xf8 }, { 0xd8, 0xb8, 0xf8 },
    { 0xf8, 0xb8, 0xf8 }, { 0xf8, 0xa4, 0xc0 }, { 0xf0, 0xd0, 0xb0 }, { 0xfc, 0xe0, 0xa8 },
    { 0xf8, 0xd8, 0x78 }, { 0xd8, 0xf8, 0x78 }, { 0xb8, 0xf8, 0xb8 }, { 0xb8, 0xf8, 0xd8 },
    { 0x00, 0xfc, 0xfc }, { 0xf8, 0xd8, 0xf8 }, { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00 },
};

enum {
    EMULATOR_VIDEO_LUMA_BLACK_MAX = 71,
    EMULATOR_VIDEO_LUMA_GRAY_MAX = 175,
};

static void emulator_video_adapter_set_error(PicoEmulatorVideoAdapter *adapter, const char *message) {
    snprintf(adapter->last_error, sizeof(adapter->last_error), "%s", message);
}

static uint8_t emulator_video_adapter_palette_luma(uint8_t nes_pixel) {
    const uint8_t *rgb = k_emulator_video_palette_rgb[nes_pixel & 0x3fu];
    return (uint8_t)((54u * rgb[0] + 183u * rgb[1] + 19u * rgb[2]) >> 8);
}

static uint8_t emulator_video_adapter_pixel_to_luma(uint8_t nes_pixel, int x, int y) {
    uint8_t luma = emulator_video_adapter_palette_luma(nes_pixel);
    (void)x;
    (void)y;
    if (luma <= EMULATOR_VIDEO_LUMA_BLACK_MAX) {
        return SMB2350_VIDEO_LUMA_BLACK;
    }
    if (luma <= EMULATOR_VIDEO_LUMA_GRAY_MAX) {
        return SMB2350_VIDEO_LUMA_GRAY;
    }
    return SMB2350_VIDEO_LUMA_WHITE;
}

static uint8_t emulator_video_adapter_count_bits64(uint64_t value) {
    uint8_t count = 0;

    while (value != 0) {
        count = (uint8_t)(count + (value & 1u));
        value >>= 1;
    }
    return count;
}

bool emulator_video_adapter_init(
    PicoEmulatorVideoAdapter *adapter,
    const uint8_t *rom_image,
    size_t rom_image_size
) {
    memset(adapter, 0, sizeof(*adapter));
    nes_init(&adapter->nes);

    if (!nes_load_cartridge_memory(&adapter->nes, rom_image, rom_image_size)) {
        emulator_video_adapter_set_error(adapter, nes_last_error(&adapter->nes));
        nes_destroy(&adapter->nes);
        return false;
    }

    nes_reset(&adapter->nes);
    adapter->initialized = true;
    adapter->debug_overlay_enabled = false;
    adapter->last_frame_first_visible_x = -1;
    adapter->last_frame_first_visible_y = -1;
    emulator_video_adapter_set_error(adapter, "");
    return true;
}

bool emulator_video_adapter_render_frame(PicoEmulatorVideoAdapter *adapter) {
    uint8_t luma_pixels[SMB2350_VIDEO_VISIBLE_WIDTH];
    uint64_t frame_started_us;
    uint64_t step_scanline_us_total = 0;
    uint64_t convert_scanline_us_total = 0;
    uint64_t color_mask = 0;
    uint32_t source_nonzero_pixels = 0;
    uint32_t visible_nonblack_pixels = 0;
    uint32_t visible_white_pixels = 0;
    uint32_t visible_gray_pixels = 0;
    uint8_t min_pixel = 0xffu;
    uint8_t max_pixel = 0x00u;
    int16_t first_visible_x = -1;
    int16_t first_visible_y = -1;

    if (!adapter->initialized) {
        emulator_video_adapter_set_error(adapter, "emulator video adapter is not initialized");
        return false;
    }

    frame_started_us = time_us_64();
    video_ntsc_begin_frame();

    for (int line = 0; line < SMB2350_VIDEO_VISIBLE_HEIGHT; ++line) {
        const NesScanline *scanline;
        uint64_t started_us;

        started_us = time_us_64();
        if (!nes_step_scanline(&adapter->nes)) {
            emulator_video_adapter_set_error(adapter, nes_last_error(&adapter->nes));
            return false;
        }
        step_scanline_us_total += time_us_64() - started_us;

        scanline = nes_scanline_buffer(&adapter->nes);
        started_us = time_us_64();
        for (int x = 0; x < SMB2350_VIDEO_VISIBLE_WIDTH; ++x) {
            uint8_t nes_pixel = scanline->pixels[x];
            uint8_t luma = emulator_video_adapter_pixel_to_luma(nes_pixel, x, (int)scanline->y);

            color_mask |= 1ull << (nes_pixel & 0x3fu);
            if (nes_pixel != 0u) {
                ++source_nonzero_pixels;
            }
            if (nes_pixel < min_pixel) {
                min_pixel = nes_pixel;
            }
            if (nes_pixel > max_pixel) {
                max_pixel = nes_pixel;
            }
            if (luma != SMB2350_VIDEO_LUMA_BLACK) {
                ++visible_nonblack_pixels;
                if (first_visible_x < 0) {
                    first_visible_x = (int16_t)x;
                    first_visible_y = (int16_t)scanline->y;
                }
            }
            if (luma == SMB2350_VIDEO_LUMA_GRAY) {
                ++visible_gray_pixels;
            } else if (luma == SMB2350_VIDEO_LUMA_WHITE) {
                ++visible_white_pixels;
            }
            luma_pixels[x] = luma;
        }
        video_ntsc_write_visible_scanline_luma((int)scanline->y, luma_pixels, SMB2350_VIDEO_VISIBLE_WIDTH);
        convert_scanline_us_total += time_us_64() - started_us;
        ++adapter->rendered_scanlines;
    }

    video_ntsc_present();
    ++adapter->rendered_frames;
    adapter->profile_render_frame_us_total += time_us_64() - frame_started_us;
    adapter->profile_step_scanline_us_total += step_scanline_us_total;
    adapter->profile_convert_scanline_us_total += convert_scanline_us_total;
    adapter->last_frame_source_nonzero_pixels = source_nonzero_pixels;
    adapter->last_frame_visible_nonblack_pixels = visible_nonblack_pixels;
    adapter->last_frame_visible_white_pixels = visible_white_pixels;
    adapter->last_frame_visible_gray_pixels = visible_gray_pixels;
    adapter->last_frame_min_pixel = min_pixel == 0xffu ? 0u : min_pixel;
    adapter->last_frame_max_pixel = max_pixel;
    adapter->last_frame_unique_color_count = emulator_video_adapter_count_bits64(color_mask);
    adapter->last_frame_first_visible_x = first_visible_x;
    adapter->last_frame_first_visible_y = first_visible_y;
    if (visible_nonblack_pixels != 0u) {
        ++adapter->frames_with_visible_output;
    }
    return true;
}

const char *emulator_video_adapter_last_error(const PicoEmulatorVideoAdapter *adapter) {
    return adapter->last_error;
}
