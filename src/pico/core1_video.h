#ifndef MICRONES_CORE1_VIDEO_H
#define MICRONES_CORE1_VIDEO_H

#include "scanline_queue.h"

#include <stdint.h>

// Statistics collected by core 1's video thread.
typedef struct {
    uint64_t frames_converted;
    uint64_t scanlines_converted;
    uint64_t convert_us_total;        // time in video_ntsc_write_visible_scanline_indexed_luma
    uint64_t frame_begin_wait_us_total; // time in video_ntsc_begin_frame (DMA swap wait)
} Core1VideoStats;

// Returns a pointer to the global ScanlineQueue that core 1 consumes from.
// Core 0 pushes scanlines here via scanline_queue_push().
ScanlineQueue *core1_video_get_queue(void);

// Configure and launch the core 1 video thread. Must be called after
// video_ntsc_init() and video_ntsc_start() have been called from core 0.
// palette_to_luma and palette_size are the same as passed to
// video_ntsc_write_visible_scanline_indexed_luma().
void core1_video_launch(const uint8_t *palette_to_luma, int palette_size);

// Copy current stats (safe to call from core 0 between frames; values are
// written by core 1 and read-only from core 0's perspective here).
void core1_video_get_stats(Core1VideoStats *stats_out);

#endif
