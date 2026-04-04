#include "pico_audio_backend.h"

#include "audio_i2s_max98357.h"

const char *pico_audio_backend_name(void) {
    return "max98357-i2s";
}

uint32_t pico_audio_backend_preferred_sample_rate(void) {
    return 48000u;
}

void pico_audio_backend_init(uint32_t sample_rate) {
    audio_i2s_max98357_init(sample_rate);
}

size_t pico_audio_backend_push_samples(const int16_t *samples, size_t count) {
    return audio_i2s_max98357_push_samples(samples, count);
}

uint32_t pico_audio_backend_underrun_count(void) {
    return audio_i2s_max98357_underrun_count();
}

uint32_t pico_audio_backend_overrun_count(void) {
    return audio_i2s_max98357_overrun_count();
}

uint32_t pico_audio_backend_buffer_level(void) {
    return audio_i2s_max98357_buffer_level();
}
