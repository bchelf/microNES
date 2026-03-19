#include "emulator_video_adapter.h"

#include "core1_video.h"
#include "video_ntsc.h"

#include "pico/time.h"

#include <stdio.h>
#include <string.h>

// NES palette indices are mapped to 8 logical grayscale levels before scanout.
// This improves tonal separation on monochrome composite output while keeping
// the fast path table-driven and cheap enough for Pico.
const uint8_t k_emulator_video_palette_to_gray[64] = {
    MICRONES_VIDEO_LUMA_MID,         MICRONES_VIDEO_LUMA_BLACK,      MICRONES_VIDEO_LUMA_BLACK,      MICRONES_VIDEO_LUMA_BLACK,
    MICRONES_VIDEO_LUMA_BLACK,       MICRONES_VIDEO_LUMA_BLACK,      MICRONES_VIDEO_LUMA_BLACK,      MICRONES_VIDEO_LUMA_BLACK,
    MICRONES_VIDEO_LUMA_BLACK,       MICRONES_VIDEO_LUMA_BLACK,      MICRONES_VIDEO_LUMA_BLACK,      MICRONES_VIDEO_LUMA_BLACK,
    MICRONES_VIDEO_LUMA_BLACK,       MICRONES_VIDEO_LUMA_BLACK,      MICRONES_VIDEO_LUMA_BLACK,      MICRONES_VIDEO_LUMA_BLACK,

    MICRONES_VIDEO_LUMA_BRIGHT,      MICRONES_VIDEO_LUMA_BRIGHT,     MICRONES_VIDEO_LUMA_MID_DARK,   MICRONES_VIDEO_LUMA_MID_DARK,
    MICRONES_VIDEO_LUMA_BRIGHT,      MICRONES_VIDEO_LUMA_MID_DARK,   MICRONES_VIDEO_LUMA_MID,        MICRONES_VIDEO_LUMA_MID_BRIGHT,
    MICRONES_VIDEO_LUMA_BRIGHT,      MICRONES_VIDEO_LUMA_BRIGHT,     MICRONES_VIDEO_LUMA_BRIGHT,     MICRONES_VIDEO_LUMA_BRIGHT,
    MICRONES_VIDEO_LUMA_MID_DARK,    MICRONES_VIDEO_LUMA_MID_DARK,   MICRONES_VIDEO_LUMA_MID_DARK,   MICRONES_VIDEO_LUMA_MID_DARK,

    MICRONES_VIDEO_LUMA_WHITE,       MICRONES_VIDEO_LUMA_VERY_BRIGHT, MICRONES_VIDEO_LUMA_BRIGHT,    MICRONES_VIDEO_LUMA_BRIGHT,
    MICRONES_VIDEO_LUMA_WHITE,       MICRONES_VIDEO_LUMA_VERY_BRIGHT, MICRONES_VIDEO_LUMA_VERY_BRIGHT, MICRONES_VIDEO_LUMA_WHITE,
    MICRONES_VIDEO_LUMA_WHITE,       MICRONES_VIDEO_LUMA_WHITE,      MICRONES_VIDEO_LUMA_VERY_BRIGHT, MICRONES_VIDEO_LUMA_WHITE,
    MICRONES_VIDEO_LUMA_BRIGHT,      MICRONES_VIDEO_LUMA_BRIGHT,     MICRONES_VIDEO_LUMA_MID_DARK,   MICRONES_VIDEO_LUMA_MID_DARK,

    MICRONES_VIDEO_LUMA_WHITE,       MICRONES_VIDEO_LUMA_WHITE,      MICRONES_VIDEO_LUMA_WHITE,      MICRONES_VIDEO_LUMA_WHITE,
    MICRONES_VIDEO_LUMA_WHITE,       MICRONES_VIDEO_LUMA_WHITE,      MICRONES_VIDEO_LUMA_WHITE,      MICRONES_VIDEO_LUMA_WHITE,
    MICRONES_VIDEO_LUMA_WHITE,       MICRONES_VIDEO_LUMA_WHITE,      MICRONES_VIDEO_LUMA_VERY_BRIGHT, MICRONES_VIDEO_LUMA_WHITE,
    MICRONES_VIDEO_LUMA_BRIGHT,      MICRONES_VIDEO_LUMA_BRIGHT,     MICRONES_VIDEO_LUMA_MID_DARK,   MICRONES_VIDEO_LUMA_MID_DARK,
};

static void emulator_video_adapter_set_error(PicoEmulatorVideoAdapter *adapter, const char *message) {
    snprintf(adapter->last_error, sizeof(adapter->last_error), "%s", message);
}

static uint64_t emulator_video_adapter_now_us(void *user) {
    (void)user;
    return time_us_64();
}

#if MICRONES_ENABLE_PICO_VIDEO_STATS
static uint8_t emulator_video_adapter_pixel_to_luma(uint8_t nes_pixel) {
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
#if MICRONES_ENABLE_STEP_PROFILING
    uint64_t step_scanline_us_total = 0;
#endif
    ScanlineQueue *queue;
#if MICRONES_ENABLE_PICO_VIDEO_STATS
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

    queue = core1_video_get_queue();
    frame_started_us = time_us_64();

    // Step all 240 visible scanlines and push pixel data to the queue.
    // Core 1 converts each scanline to composite format concurrently.
    // begin_frame / present are handled by core 1.
    for (int line = 0; line < MICRONES_VIDEO_VISIBLE_HEIGHT; ++line) {
        const NesScanline *scanline;
#if MICRONES_ENABLE_STEP_PROFILING
        uint64_t started_us = time_us_64();
#endif
        if (!nes_step_scanline(&adapter->nes)) {
            emulator_video_adapter_set_error(adapter, nes_last_error(&adapter->nes));
            return false;
        }
#if MICRONES_ENABLE_STEP_PROFILING
        step_scanline_us_total += time_us_64() - started_us;
#endif

        scanline = nes_scanline_buffer(&adapter->nes);

#if MICRONES_ENABLE_PICO_VIDEO_STATS
        for (int x = 0; x < MICRONES_VIDEO_VISIBLE_WIDTH; ++x) {
            uint8_t nes_pixel = scanline->pixels[x];
            uint8_t luma = emulator_video_adapter_pixel_to_luma(nes_pixel);

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
            if (luma != MICRONES_VIDEO_LUMA_BLACK) {
                ++visible_nonblack_pixels;
                if (first_visible_x < 0) {
                    first_visible_x = (int16_t)x;
                    first_visible_y = (int16_t)scanline->y;
                }
            }
            if (luma >= MICRONES_VIDEO_LUMA_BRIGHT) {
                ++visible_white_pixels;
            } else if (luma != MICRONES_VIDEO_LUMA_BLACK) {
                ++visible_gray_pixels;
            }
        }
#endif

        scanline_queue_push(queue, scanline->pixels, scanline->y);
        ++adapter->rendered_scanlines;
    }

    ++adapter->rendered_frames;
    adapter->profile_render_frame_us_total += time_us_64() - frame_started_us;
#if MICRONES_ENABLE_STEP_PROFILING
    adapter->profile_step_scanline_us_total += step_scanline_us_total;
#endif

#if MICRONES_ENABLE_PICO_VIDEO_STATS
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
