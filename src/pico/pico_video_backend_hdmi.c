#include "pico_video_backend.h"

#include "video_hstx.h"

#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>

const char *pico_video_backend_name(void) {
    return "hdmi-hstx";
}

const char *pico_video_backend_last_error(void) {
    return video_hstx_last_error();
}

bool pico_video_backend_init(void) {
    return video_hstx_init();
}

void pico_video_backend_start_test_pattern(void) {
    video_hstx_draw_test_pattern();
    video_hstx_start();
}

void pico_video_backend_start_emulator(void) {
    video_hstx_draw_test_pattern();
    video_hstx_start();
}

void pico_video_backend_suspend_for_flash(void) {
    video_hstx_stop();
}

void pico_video_backend_resume_after_flash(void) {
    video_hstx_start();
    /* Give the HDMI sink time to lock onto the restored TMDS signal.
     * Without this delay the monitor may fail to reacquire sync. */
    sleep_ms(100);
}

void pico_video_backend_get_stats(PicoVideoBackendStats *stats_out) {
    VideoHstxStats hstx_stats;

    if (stats_out == NULL) {
        return;
    }

    memset(stats_out, 0, sizeof(*stats_out));
    video_hstx_get_stats(&hstx_stats);
    stats_out->frames_presented = hstx_stats.frames_presented;
    stats_out->convert_us_total = hstx_stats.present_us_total;
    stats_out->swap_wait_us_total = hstx_stats.present_us_total;
    stats_out->swap_wait_us_max = hstx_stats.present_us_max;
    stats_out->video_scanline = hstx_stats.scanline;
    stats_out->video_started = hstx_stats.started;
}
