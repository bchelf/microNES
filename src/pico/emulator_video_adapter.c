#include "emulator_video_adapter.h"

#include "video_ntsc.h"

#include "pico/time.h"

#include <stdio.h>
#include <string.h>

// NES palette indices are mapped to 8 logical grayscale levels before scanout.
// This improves tonal separation on monochrome composite output while keeping
// the fast path table-driven and cheap enough for Pico.
static const uint8_t k_emulator_video_palette_to_gray[64] = {
    SMB2350_VIDEO_LUMA_MID,         SMB2350_VIDEO_LUMA_BLACK,      SMB2350_VIDEO_LUMA_BLACK,      SMB2350_VIDEO_LUMA_BLACK,
    SMB2350_VIDEO_LUMA_BLACK,       SMB2350_VIDEO_LUMA_BLACK,      SMB2350_VIDEO_LUMA_BLACK,      SMB2350_VIDEO_LUMA_BLACK,
    SMB2350_VIDEO_LUMA_BLACK,       SMB2350_VIDEO_LUMA_BLACK,      SMB2350_VIDEO_LUMA_BLACK,      SMB2350_VIDEO_LUMA_BLACK,
    SMB2350_VIDEO_LUMA_BLACK,       SMB2350_VIDEO_LUMA_BLACK,      SMB2350_VIDEO_LUMA_BLACK,      SMB2350_VIDEO_LUMA_BLACK,

    SMB2350_VIDEO_LUMA_BRIGHT,      SMB2350_VIDEO_LUMA_MID_BRIGHT, SMB2350_VIDEO_LUMA_DARK,       SMB2350_VIDEO_LUMA_DARK,
    SMB2350_VIDEO_LUMA_MID_BRIGHT,  SMB2350_VIDEO_LUMA_DARK,       SMB2350_VIDEO_LUMA_MID_DARK,   SMB2350_VIDEO_LUMA_MID,
    SMB2350_VIDEO_LUMA_MID_BRIGHT,  SMB2350_VIDEO_LUMA_MID_BRIGHT, SMB2350_VIDEO_LUMA_MID_BRIGHT, SMB2350_VIDEO_LUMA_MID_BRIGHT,
    SMB2350_VIDEO_LUMA_DARK,        SMB2350_VIDEO_LUMA_DARK,       SMB2350_VIDEO_LUMA_DARK,       SMB2350_VIDEO_LUMA_DARK,

    SMB2350_VIDEO_LUMA_WHITE,       SMB2350_VIDEO_LUMA_BRIGHT,     SMB2350_VIDEO_LUMA_MID_BRIGHT, SMB2350_VIDEO_LUMA_MID_BRIGHT,
    SMB2350_VIDEO_LUMA_WHITE,       SMB2350_VIDEO_LUMA_BRIGHT,     SMB2350_VIDEO_LUMA_BRIGHT,     SMB2350_VIDEO_LUMA_VERY_BRIGHT,
    SMB2350_VIDEO_LUMA_VERY_BRIGHT, SMB2350_VIDEO_LUMA_VERY_BRIGHT, SMB2350_VIDEO_LUMA_BRIGHT,    SMB2350_VIDEO_LUMA_WHITE,
    SMB2350_VIDEO_LUMA_MID_BRIGHT,  SMB2350_VIDEO_LUMA_MID_BRIGHT, SMB2350_VIDEO_LUMA_DARK,       SMB2350_VIDEO_LUMA_DARK,

    SMB2350_VIDEO_LUMA_WHITE,       SMB2350_VIDEO_LUMA_WHITE,      SMB2350_VIDEO_LUMA_WHITE,      SMB2350_VIDEO_LUMA_WHITE,
    SMB2350_VIDEO_LUMA_WHITE,       SMB2350_VIDEO_LUMA_WHITE,      SMB2350_VIDEO_LUMA_WHITE,      SMB2350_VIDEO_LUMA_WHITE,
    SMB2350_VIDEO_LUMA_WHITE,       SMB2350_VIDEO_LUMA_WHITE,      SMB2350_VIDEO_LUMA_VERY_BRIGHT, SMB2350_VIDEO_LUMA_WHITE,
    SMB2350_VIDEO_LUMA_BRIGHT,      SMB2350_VIDEO_LUMA_BRIGHT,     SMB2350_VIDEO_LUMA_MID_DARK,   SMB2350_VIDEO_LUMA_MID_DARK,
};

static void emulator_video_adapter_set_error(PicoEmulatorVideoAdapter *adapter, const char *message) {
    snprintf(adapter->last_error, sizeof(adapter->last_error), "%s", message);
}

static uint64_t emulator_video_adapter_now_us(void *user) {
    (void)user;
    return time_us_64();
}

#if SMB2350_ENABLE_PICO_VIDEO_STATS
static uint8_t emulator_video_adapter_pixel_to_luma(uint8_t nes_pixel, int x, int y) {
    (void)x;
    (void)y;
    return k_emulator_video_palette_to_gray[nes_pixel & 0x3fu];
}

static uint8_t emulator_video_adapter_count_bits64(uint64_t value) {
    uint8_t count = 0;

    while (value != 0) {
        count = (uint8_t)(count + (value & 1u));
        value >>= 1;
    }
    return count;
}
#endif

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

    nes_set_profile_clock(&adapter->nes, emulator_video_adapter_now_us, NULL);
    nes_reset(&adapter->nes);
    adapter->initialized = true;
    adapter->debug_overlay_enabled = false;
    adapter->last_frame_first_visible_x = -1;
    adapter->last_frame_first_visible_y = -1;
    emulator_video_adapter_set_error(adapter, "");
    return true;
}

bool emulator_video_adapter_render_frame(PicoEmulatorVideoAdapter *adapter) {
    uint64_t frame_started_us;
    uint64_t step_scanline_us_total = 0;
    uint64_t convert_scanline_us_total = 0;
#if SMB2350_ENABLE_PICO_VIDEO_STATS
    uint64_t color_mask = 0;
    uint32_t source_nonzero_pixels = 0;
    uint32_t visible_nonblack_pixels = 0;
    uint32_t visible_white_pixels = 0;
    uint32_t visible_gray_pixels = 0;
    uint8_t min_pixel = 0xffu;
    uint8_t max_pixel = 0x00u;
    int16_t first_visible_x = -1;
    int16_t first_visible_y = -1;
#endif

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
#if SMB2350_ENABLE_PICO_VIDEO_STATS
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
            if (luma >= SMB2350_VIDEO_LUMA_BRIGHT) {
                ++visible_white_pixels;
            } else if (luma != SMB2350_VIDEO_LUMA_BLACK) {
                ++visible_gray_pixels;
            }
        }
        video_ntsc_write_visible_scanline_indexed_luma(
            (int)scanline->y,
            scanline->pixels,
            SMB2350_VIDEO_VISIBLE_WIDTH,
            k_emulator_video_palette_to_gray,
            64
        );
#else
        video_ntsc_write_visible_scanline_indexed_luma(
            (int)scanline->y,
            scanline->pixels,
            SMB2350_VIDEO_VISIBLE_WIDTH,
            k_emulator_video_palette_to_gray,
            64
        );
#endif
        convert_scanline_us_total += time_us_64() - started_us;
        ++adapter->rendered_scanlines;
    }

    video_ntsc_present();
    ++adapter->rendered_frames;
    adapter->profile_render_frame_us_total += time_us_64() - frame_started_us;
    adapter->profile_step_scanline_us_total += step_scanline_us_total;
    adapter->profile_convert_scanline_us_total += convert_scanline_us_total;
#if SMB2350_ENABLE_PICO_VIDEO_STATS
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
#else
    adapter->last_frame_source_nonzero_pixels = 0;
    adapter->last_frame_visible_nonblack_pixels = 0;
    adapter->last_frame_visible_white_pixels = 0;
    adapter->last_frame_visible_gray_pixels = 0;
    adapter->last_frame_min_pixel = 0;
    adapter->last_frame_max_pixel = 0;
    adapter->last_frame_unique_color_count = 0;
    adapter->last_frame_first_visible_x = -1;
    adapter->last_frame_first_visible_y = -1;
#endif
    return true;
}

const char *emulator_video_adapter_last_error(const PicoEmulatorVideoAdapter *adapter) {
    return adapter->last_error;
}
