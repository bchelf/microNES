#ifndef MICRONES_VIDEO_HSTX_H
#define MICRONES_VIDEO_HSTX_H

#include "framebuffer.h"

#include <stdbool.h>
#include <stddef.h>
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

/*
 * HDMI audio glue. The data-island scheduler is owned by video_hstx so it
 * can co-exist with the per-line DMA chain; the audio backend pushes int16
 * stereo samples via these entry points.
 */
void video_hstx_print_diag(void);
void video_hstx_hdmi_audio_service(void);

void video_hstx_hdmi_audio_init(uint32_t sample_rate);
size_t video_hstx_hdmi_audio_push(const int16_t *mono_samples,
                                  size_t count);
uint32_t video_hstx_hdmi_audio_underruns(void);
uint32_t video_hstx_hdmi_audio_overruns(void);
uint32_t video_hstx_hdmi_audio_buffer_level(void);

#endif
