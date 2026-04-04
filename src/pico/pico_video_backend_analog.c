#include "pico_video_backend.h"

#include "video_ntsc.h"

#include <stddef.h>
#include <string.h>

#if defined(MICRONES_PICO_VIDEO_MODE_EMULATOR)
#include "core1_video.h"
#include "pico/multicore.h"
#include "scanline_queue.h"
#endif

const char *pico_video_backend_name(void) {
    return "analog";
}

const char *pico_video_backend_last_error(void) {
    return "";
}

bool pico_video_backend_init(void) {
    video_ntsc_init();
    return true;
}

void pico_video_backend_start_test_pattern(void) {
    video_ntsc_build_test_pattern_frame();
    video_ntsc_start();
}

void pico_video_backend_start_emulator(void) {
#if defined(MICRONES_PICO_VIDEO_MODE_EMULATOR)
    extern const uint8_t k_emulator_video_palette_to_gray[64];

    video_ntsc_precompute_palette(k_emulator_video_palette_to_gray, 64);
    scanline_queue_init(core1_video_get_queue());
    video_ntsc_start();
    multicore_launch_core1(video_ntsc_core1_entry);
#endif
}

void pico_video_backend_get_stats(PicoVideoBackendStats *stats_out) {
    if (stats_out == NULL) {
        return;
    }

    memset(stats_out, 0, sizeof(*stats_out));

#if defined(MICRONES_PICO_VIDEO_MODE_EMULATOR)
    Core1VideoStats core1_stats;
    MicronesVideoNtscPerfStats ntsc_stats;
    ScanlineQueue *queue = core1_video_get_queue();

    core1_video_get_stats(&core1_stats);
    video_ntsc_perf_get(&ntsc_stats);

    stats_out->frames_presented = core1_stats.frames_converted;
    stats_out->scanlines_presented = core1_stats.scanlines_converted;
    stats_out->convert_us_total = core1_stats.convert_us_total;
    stats_out->frame_begin_wait_us_total = core1_stats.frame_begin_wait_us_total;
    stats_out->swap_wait_us_total = ntsc_stats.swap_wait_us_total;
    stats_out->swap_wait_us_max = ntsc_stats.swap_wait_us_max;
    stats_out->queue_stall_count = queue->producer_stall_count;
    stats_out->queue_stall_us_total = queue->producer_stall_us_total;
#else
    MicronesVideoNtscPerfStats ntsc_stats;

    video_ntsc_perf_get(&ntsc_stats);
    stats_out->swap_wait_us_total = ntsc_stats.swap_wait_us_total;
    stats_out->swap_wait_us_max = ntsc_stats.swap_wait_us_max;
#endif
}
