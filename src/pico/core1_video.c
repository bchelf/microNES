#include "core1_video.h"

#include "scanline_queue.h"
#include "video_ntsc.h"

#include "pico/multicore.h"
#include "pico/time.h"

#include <stddef.h>
#include <string.h>

static ScanlineQueue s_queue;
static const uint8_t *s_palette_to_luma;
static int s_palette_size;
static Core1VideoStats s_stats;

ScanlineQueue *core1_video_get_queue(void) {
    return &s_queue;
}

void core1_video_get_stats(Core1VideoStats *stats_out) {
    if (stats_out == NULL) {
        return;
    }
    *stats_out = s_stats;
}

static void __not_in_flash_func(core1_entry)(void) {
    ScanlineQueueSlot slot;

    // Wait for the initial DMA frame to complete so the build buffer is free.
    video_ntsc_begin_frame();

    while (true) {
        // Pull scanlines from the queue and convert them into the composite
        // build buffer. Frame boundaries are detected by y == 0 (begin_frame
        // was already called above before the first scanline) and
        // y == MICRONES_VIDEO_VISIBLE_HEIGHT-1 (present + begin_frame).
        scanline_queue_pop_blocking(&s_queue, &slot);

        uint64_t convert_start = time_us_64();
        video_ntsc_write_visible_scanline_indexed_luma(
            (int)slot.y,
            slot.pixels,
            MICRONES_VIDEO_VISIBLE_WIDTH,
            s_palette_to_luma,
            s_palette_size
        );
        s_stats.convert_us_total += time_us_64() - convert_start;
        ++s_stats.scanlines_converted;

        if (slot.y == (uint16_t)(MICRONES_VIDEO_VISIBLE_HEIGHT - 1)) {
            // Last visible scanline of this frame: present it and
            // pre-acquire the next build buffer. The begin_frame call
            // may spin briefly waiting for DMA to complete the swap.
            video_ntsc_present();
            ++s_stats.frames_converted;

            uint64_t wait_start = time_us_64();
            video_ntsc_begin_frame();
            s_stats.frame_begin_wait_us_total += time_us_64() - wait_start;
        }
    }
}

void core1_video_launch(const uint8_t *palette_to_luma, int palette_size) {
    scanline_queue_init(&s_queue);
    s_palette_to_luma = palette_to_luma;
    s_palette_size = palette_size;
    memset(&s_stats, 0, sizeof(s_stats));
    // Build the precomputed pixel→level table before core 1 starts consuming.
    video_ntsc_precompute_palette(palette_to_luma, palette_size);
    multicore_launch_core1(core1_entry);
}
