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
    MicronesVideoNtscPerfStats ntsc_stats;
    ScanlineQueue *queue = core1_video_get_queue();

    video_ntsc_perf_get(&ntsc_stats);

    stats_out->frames_presented = ntsc_stats.frames_rendered;
    stats_out->scanlines_presented = ntsc_stats.active_lines_rendered;
    stats_out->convert_us_total = ntsc_stats.render_us_total;
    stats_out->frame_begin_wait_us_total = 0;
    stats_out->swap_wait_us_total = ntsc_stats.swap_wait_us_total;
    stats_out->swap_wait_us_max = ntsc_stats.swap_wait_us_max;
    stats_out->queue_stall_count = queue->producer_stall_count;
    stats_out->queue_stall_us_total = queue->producer_stall_us_total;
    stats_out->video_frames_rendered = ntsc_stats.frames_rendered;
    stats_out->video_irq0_count = ntsc_stats.irq0_count;
    stats_out->video_irq1_count = ntsc_stats.irq1_count;
    stats_out->video_lines_rendered = ntsc_stats.lines_rendered;
    stats_out->video_active_lines_rendered = ntsc_stats.active_lines_rendered;
    stats_out->video_blank_lines_rendered = ntsc_stats.blank_lines_rendered;
    stats_out->video_vsync_lines_rendered = ntsc_stats.vsync_lines_rendered;
    stats_out->video_render_us_total = ntsc_stats.render_us_total;
    stats_out->video_render_us_max = ntsc_stats.render_us_max;
    stats_out->video_render_over_50us_count = ntsc_stats.render_over_50us_count;
    stats_out->video_render_over_63us_count = ntsc_stats.render_over_63us_count;
    stats_out->video_render_over_100us_count = ntsc_stats.render_over_100us_count;
    stats_out->video_render_us_max_line = ntsc_stats.render_us_max_line;
    stats_out->video_render_us_max_active_y = ntsc_stats.render_us_max_active_y;
    stats_out->video_render_us_max_kind = ntsc_stats.render_us_max_kind;
    stats_out->video_display_line = ntsc_stats.display_line;
    stats_out->video_render_line = ntsc_stats.render_line;
    stats_out->video_idle_buf = ntsc_stats.idle_buf;
    stats_out->queue_level = queue->head - queue->tail;
    stats_out->queue_level_max = ntsc_stats.queue_level_max;
    stats_out->queue_consumer_wait_count = queue->consumer_wait_count;
    stats_out->queue_consumer_wait_us_total = queue->consumer_wait_us_total;
    stats_out->queue_consumer_wait_us_max = queue->consumer_wait_us_max;
    stats_out->scanline_y_mismatch_count = ntsc_stats.scanline_y_mismatch_count;
    for (int i = 0; i < 4; ++i) {
        stats_out->probe_y[i] = ntsc_stats.probe_y[i];
        stats_out->probe_slot_y[i] = ntsc_stats.probe_slot_y[i];
        stats_out->probe_hash[i] = ntsc_stats.probe_hash[i];
        stats_out->probe_nonblack[i] = ntsc_stats.probe_nonblack[i];
        stats_out->probe_min[i] = ntsc_stats.probe_min[i];
        stats_out->probe_max[i] = ntsc_stats.probe_max[i];
    }
#else
    MicronesVideoNtscPerfStats ntsc_stats;

    video_ntsc_perf_get(&ntsc_stats);
    stats_out->swap_wait_us_total = ntsc_stats.swap_wait_us_total;
    stats_out->swap_wait_us_max = ntsc_stats.swap_wait_us_max;
    stats_out->video_irq0_count = ntsc_stats.irq0_count;
#endif
}
