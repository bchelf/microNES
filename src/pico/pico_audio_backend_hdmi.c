#include "pico_audio_backend.h"
#include "video_hstx.h"

const char *pico_audio_backend_name(void) {
    return "hdmi-hstx";
}

uint32_t pico_audio_backend_preferred_sample_rate(void) {
    return 48000u;
}

void pico_audio_backend_init(uint32_t sample_rate) {
    video_hstx_hdmi_audio_init(sample_rate);
}

size_t pico_audio_backend_push_samples(const int16_t *samples, size_t count) {
    return video_hstx_hdmi_audio_push(samples, count);
}

uint32_t pico_audio_backend_underrun_count(void) {
    return video_hstx_hdmi_audio_underruns();
}

uint32_t pico_audio_backend_overrun_count(void) {
    return video_hstx_hdmi_audio_overruns();
}

uint32_t pico_audio_backend_buffer_level(void) {
    return video_hstx_hdmi_audio_buffer_level();
}
