#ifndef MICRONES_VIDEO_TFT_H
#define MICRONES_VIDEO_TFT_H

#include "framebuffer.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t frames_presented;
    uint64_t bytes_sent;
    uint64_t present_us_total;
    uint64_t present_us_max;
    uint64_t present_convert_us_total;
    uint64_t present_diff_us_total;
    uint64_t present_bus_us_total;
    uint64_t spans_sent_total;
    uint64_t full_frames_sent;
} PicoTftStats;

bool video_tft_init(void);
const char *video_tft_last_error(void);
const char *video_tft_backend_name(void);
const char *video_tft_controller_name(void);
void video_tft_set_interlace(bool enabled);
void video_tft_draw_test_pattern(void);
void video_tft_present_frame(const NesFrameBuffer *frame);
void video_tft_get_stats(PicoTftStats *stats_out);

#endif
