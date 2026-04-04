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
} PicoVideoBackendStats;

const char *pico_video_backend_name(void);
const char *pico_video_backend_last_error(void);
bool pico_video_backend_init(void);
void pico_video_backend_start_test_pattern(void);
void pico_video_backend_start_emulator(void);
void pico_video_backend_get_stats(PicoVideoBackendStats *stats_out);

#endif
