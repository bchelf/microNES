#ifndef MICRONES_PICO_AUDIO_BACKEND_H
#define MICRONES_PICO_AUDIO_BACKEND_H

#include <stddef.h>
#include <stdint.h>

const char *pico_audio_backend_name(void);
uint32_t pico_audio_backend_preferred_sample_rate(void);
void pico_audio_backend_init(uint32_t sample_rate);
size_t pico_audio_backend_push_samples(const int16_t *samples, size_t count);
uint32_t pico_audio_backend_underrun_count(void);
uint32_t pico_audio_backend_overrun_count(void);
uint32_t pico_audio_backend_buffer_level(void);

#endif
