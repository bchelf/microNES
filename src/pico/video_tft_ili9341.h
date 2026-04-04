#ifndef MICRONES_VIDEO_TFT_ILI9341_H
#define MICRONES_VIDEO_TFT_ILI9341_H

#include "framebuffer.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t frames_presented;
    uint64_t bytes_sent;
    uint64_t present_us_total;
    uint64_t present_us_max;
    /* Profiling breakdown within present_frame: */
    uint64_t present_convert_us_total; /* time in palette→RGB565 conversion */
    uint64_t present_diff_us_total;    /* time in dirty-region diff */
    uint64_t present_spi_us_total;     /* time blocked in spi_write_blocking */
    uint64_t spans_sent_total;         /* total span count across all frames */
    uint64_t full_frames_sent;         /* frames sent without diff (first frame) */
} PicoTftIli9341Stats;

bool video_tft_ili9341_init(void);
const char *video_tft_ili9341_last_error(void);
void video_tft_ili9341_set_interlace(bool enabled);
void video_tft_ili9341_draw_test_pattern(void);
void video_tft_ili9341_present_frame(const NesFrameBuffer *frame);
void video_tft_ili9341_get_stats(PicoTftIli9341Stats *stats_out);

#endif
