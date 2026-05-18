#ifndef MICRONES_PICO_VIDEO_BACKEND_H
#define MICRONES_PICO_VIDEO_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t frames_presented;
    uint64_t scanlines_presented;
    uint64_t convert_us_total;
    uint64_t frame_begin_wait_us_total;
    uint64_t swap_wait_us_total;
    uint64_t swap_wait_us_max;
    uint32_t queue_stall_count;
    uint64_t queue_stall_us_total;
    uint64_t video_frames_rendered;
    uint64_t video_irq0_count;
    uint64_t video_irq1_count;
    uint64_t video_lines_rendered;
    uint64_t video_active_lines_rendered;
    uint64_t video_blank_lines_rendered;
    uint64_t video_vsync_lines_rendered;
    uint64_t video_render_us_total;
    uint32_t video_render_us_max;
    uint32_t video_render_over_50us_count;
    uint32_t video_render_over_63us_count;
    uint32_t video_render_over_100us_count;
    uint32_t video_render_us_max_line;
    uint32_t video_render_us_max_active_y;
    uint32_t video_render_us_max_kind;
    uint32_t video_display_line;
    uint32_t video_render_line;
    uint32_t video_idle_buf;
    uint32_t queue_level;
    uint32_t queue_level_max;
    uint32_t queue_consumer_wait_count;
    uint64_t queue_consumer_wait_us_total;
    uint32_t queue_consumer_wait_us_max;
    uint32_t scanline_y_mismatch_count;
    uint32_t probe_y[4];
    uint32_t probe_slot_y[4];
    uint32_t probe_hash[4];
    uint32_t probe_nonblack[4];
    uint8_t probe_min[4];
    uint8_t probe_max[4];
} PicoVideoBackendStats;

const char *pico_video_backend_name(void);
const char *pico_video_backend_last_error(void);
bool pico_video_backend_init(void);
void pico_video_backend_start_test_pattern(void);
void pico_video_backend_start_emulator(void);
void pico_video_backend_get_stats(PicoVideoBackendStats *stats_out);

#endif
