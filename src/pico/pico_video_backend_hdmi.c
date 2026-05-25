#include "pico_video_backend.h"

#include "video_hstx.h"

#ifdef MICRONES_BOARD_V0_1
#include "board_pinout_v0_1.h"
#define HDMI_HPD_PIN MICRONES_V0_1_PIN_HDMI_HPD
#else
#define HDMI_HPD_PIN 11u
#endif

#include "hardware/gpio.h"
#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>

static bool s_hpd_last;

static bool hdmi_hpd_read(void) {
    return gpio_get(HDMI_HPD_PIN);
}

static void hdmi_hpd_init(void) {
    gpio_init(HDMI_HPD_PIN);
    gpio_set_dir(HDMI_HPD_PIN, GPIO_IN);
    gpio_pull_down(HDMI_HPD_PIN);
    s_hpd_last = false;
}

static void hdmi_hpd_wait(void) {
    while (!hdmi_hpd_read()) {
        sleep_ms(50);
    }
    sleep_ms(100);
    s_hpd_last = true;
}

const char *pico_video_backend_name(void) {
    return "hdmi-hstx";
}

const char *pico_video_backend_last_error(void) {
    return video_hstx_last_error();
}

bool pico_video_backend_init(void) {
    hdmi_hpd_init();
    return video_hstx_init();
}

void pico_video_backend_start_test_pattern(void) {
    video_hstx_draw_test_pattern();
    hdmi_hpd_wait();
    video_hstx_start();
}

void pico_video_backend_start_emulator(void) {
    video_hstx_draw_test_pattern();
    hdmi_hpd_wait();
    video_hstx_start();
}

void pico_video_backend_suspend_for_flash(void) {
    video_hstx_stop();
}

void pico_video_backend_resume_after_flash(void) {
    hdmi_hpd_wait();
    video_hstx_start();
    sleep_ms(200);
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

bool pico_video_backend_hpd_changed(void) {
    bool now = hdmi_hpd_read();
    bool changed = (now != s_hpd_last);
    s_hpd_last = now;
    return changed;
}
