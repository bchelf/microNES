#ifndef SMB2350_VIDEO_NTSC_H
#define SMB2350_VIDEO_NTSC_H

#include <stdint.h>

// Monochrome composite video on a simple two-resistor DAC:
// - GP0 through 1k to RCA center for sync / blank bias
// - GP1 through 470R to RCA center for added luma
// - RCA shell to GND, with the display assumed to provide 75R termination
#define SMB2350_VIDEO_PIN_BASE 0u
#define SMB2350_VIDEO_PIN_COUNT 2u

enum {
    SMB2350_VIDEO_VISIBLE_WIDTH = 256,
    SMB2350_VIDEO_VISIBLE_HEIGHT = 240,
};

typedef enum {
    SMB2350_VIDEO_LUMA_BLACK = 0,
    SMB2350_VIDEO_LUMA_DARK = 1,
    SMB2350_VIDEO_LUMA_MID_DARK = 2,
    SMB2350_VIDEO_LUMA_MID = 3,
    SMB2350_VIDEO_LUMA_MID_BRIGHT = 4,
    SMB2350_VIDEO_LUMA_BRIGHT = 5,
    SMB2350_VIDEO_LUMA_VERY_BRIGHT = 6,
    SMB2350_VIDEO_LUMA_WHITE = 7,
} smb2350_video_luma_t;

typedef struct {
    uint64_t begin_frame_calls;
    uint64_t present_calls;
    uint64_t swap_wait_events;
    uint64_t swap_wait_us_total;
    uint64_t swap_wait_us_max;
} Smb2350VideoNtscPerfStats;

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
void video_ntsc_perf_get(Smb2350VideoNtscPerfStats *stats_out);

// Precompute a 64×4 table mapping (nes_palette_index, dither_phase) →
// composite level. Call once after the palette is known (e.g. at launch) to
// enable the fast indexed-luma path that avoids per-sample gray lookups.
void video_ntsc_precompute_palette(const uint8_t *palette_to_luma, int palette_size);

#endif
