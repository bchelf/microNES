/*
 * pico_video_backend_hdmi.c — adapts the HSTX/DVI driver to the
 * PicoVideoBackend interface used by main.c and the emulator adapter.
 *
 * This is intentionally thin: video_hstx owns the heavy lifting (line
 * buffers, DMA chain, line-fill ISR).  The adapter only exposes the
 * standard hooks main.c calls and forwards stats.
 */

#include "pico_video_backend.h"

#include "video_hstx.h"

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
}

void pico_video_backend_start_emulator(void) {
    /* No core1 launch needed: the HSTX path is fully DMA-driven from an
     * IRQ.  The emulator runs on core0 unimpeded. */
}

void pico_video_backend_get_stats(PicoVideoBackendStats *stats_out) {
    if (stats_out == NULL) {
        return;
    }
    memset(stats_out, 0, sizeof(*stats_out));

    VideoHstxStats hstx_stats;
    video_hstx_get_stats(&hstx_stats);
    stats_out->frames_presented = hstx_stats.frames_presented;
    stats_out->scanlines_presented = hstx_stats.lines_filled;
    stats_out->convert_us_total = hstx_stats.fill_us_total;
    stats_out->swap_wait_us_max = hstx_stats.fill_us_max_per_line;
}
