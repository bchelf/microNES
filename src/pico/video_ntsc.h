#ifndef MICRONES_VIDEO_NTSC_H
#define MICRONES_VIDEO_NTSC_H

#include <stdint.h>

// Monochrome composite video on a simple two-resistor DAC:
// - GP0 through 1k to RCA center for sync / blank bias
// - GP1 through 470R to RCA center for added luma
// - RCA shell to GND, with the display assumed to provide 75R termination
#define MICRONES_VIDEO_PIN_BASE 0u
#define MICRONES_VIDEO_PIN_COUNT 2u

enum {
    MICRONES_VIDEO_VISIBLE_WIDTH = 256,
    MICRONES_VIDEO_VISIBLE_HEIGHT = 240,
};

typedef enum {
    MICRONES_VIDEO_LUMA_BLACK = 0,
    MICRONES_VIDEO_LUMA_DARK = 1,
    MICRONES_VIDEO_LUMA_MID_DARK = 2,
    MICRONES_VIDEO_LUMA_MID = 3,
    MICRONES_VIDEO_LUMA_MID_BRIGHT = 4,
    MICRONES_VIDEO_LUMA_BRIGHT = 5,
    MICRONES_VIDEO_LUMA_VERY_BRIGHT = 6,
    MICRONES_VIDEO_LUMA_WHITE = 7,
} micrones_video_luma_t;

typedef struct {
    uint64_t begin_frame_calls;
    uint64_t present_calls;
    uint64_t swap_wait_events;
    uint64_t swap_wait_us_total;
    uint64_t swap_wait_us_max;
} MicronesVideoNtscPerfStats;

void video_ntsc_init(void);
void video_ntsc_start(void);
void video_ntsc_begin_frame(void);
void video_ntsc_write_visible_scanline_mono(int visible_y, const uint8_t *pixels, int pixel_count);
void video_ntsc_write_visible_scanline_luma(int visible_y, const uint8_t *pixels, int pixel_count);
void video_ntsc_write_visible_scanline_indexed_luma(
    int visible_y,
    const uint8_t *pixels,
    int pixel_count,
    const uint8_t *palette_to_luma,
    int palette_size
);
void video_ntsc_present(void);
void video_ntsc_build_test_pattern_frame(void);
void video_ntsc_perf_get(MicronesVideoNtscPerfStats *stats_out);

// Precompute a 64×4 table mapping (nes_palette_index, dither_phase) →
// composite level. Call once after the palette is known (e.g. at launch) to
// enable the fast indexed-luma path that avoids per-sample gray lookups.
void video_ntsc_precompute_palette(const uint8_t *palette_to_luma, int palette_size);

#endif
