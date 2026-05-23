#ifndef MICRONES_VIDEO_HSTX_H
#define MICRONES_VIDEO_HSTX_H

#include "framebuffer.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t frames_presented;
    uint64_t present_us_total;
    uint64_t present_us_max;
    uint32_t scanline;
    bool started;
} VideoHstxStats;

bool video_hstx_init(void);
const char *video_hstx_last_error(void);
void video_hstx_start(void);
void video_hstx_stop(void);
void video_hstx_draw_test_pattern(void);
void video_hstx_present_frame(const NesFrameBuffer *frame);
void video_hstx_get_stats(VideoHstxStats *stats_out);

#endif
