#include "pico_video_backend.h"

#include "video_tft_ili9341.h"

#include <string.h>

const char *pico_video_backend_name(void) {
    return "tft_ili9341";
}

const char *pico_video_backend_last_error(void) {
    return video_tft_ili9341_last_error();
}

bool pico_video_backend_init(void) {
    return video_tft_ili9341_init();
}

void pico_video_backend_start_test_pattern(void) {
    video_tft_ili9341_draw_test_pattern();
}

void pico_video_backend_start_emulator(void) {
}

void pico_video_backend_get_stats(PicoVideoBackendStats *stats_out) {
    PicoTftIli9341Stats tft_stats;

    if (stats_out == NULL) {
        return;
    }

    memset(stats_out, 0, sizeof(*stats_out));
    video_tft_ili9341_get_stats(&tft_stats);
    stats_out->frames_presented = tft_stats.frames_presented;
    stats_out->convert_us_total = tft_stats.present_us_total;
    stats_out->swap_wait_us_total = tft_stats.present_us_total;
    stats_out->swap_wait_us_max = tft_stats.present_us_max;
}
