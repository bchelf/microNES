#include "pico_audio_backend.h"

#include "audio_pwm.h"

const char *pico_audio_backend_name(void) {
    return "pwm-gp9";
}

uint32_t pico_audio_backend_preferred_sample_rate(void) {
    return 44100u;
}

void pico_audio_backend_init(uint32_t sample_rate) {
    audio_pwm_init(sample_rate);
}

size_t pico_audio_backend_push_samples(const int16_t *samples, size_t count) {
    return audio_pwm_push_samples(samples, count);
}

uint32_t pico_audio_backend_underrun_count(void) {
    return audio_pwm_underrun_count();
}

uint32_t pico_audio_backend_overrun_count(void) {
    return 0u;
}

uint32_t pico_audio_backend_buffer_level(void) {
    return audio_pwm_buffer_level();
}
