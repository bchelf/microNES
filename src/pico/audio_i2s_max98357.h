#ifndef MICRONES_AUDIO_I2S_MAX98357_H
#define MICRONES_AUDIO_I2S_MAX98357_H

#include <stddef.h>
#include <stdint.h>

#define MICRONES_MAX98357_BCLK_PIN 10u
#define MICRONES_MAX98357_DIN_PIN  11u
#define MICRONES_MAX98357_LRCLK_PIN 12u

void audio_i2s_max98357_init(uint32_t sample_rate);
size_t audio_i2s_max98357_push_samples(const int16_t *samples, size_t count);
uint32_t audio_i2s_max98357_underrun_count(void);
uint32_t audio_i2s_max98357_overrun_count(void);
uint32_t audio_i2s_max98357_buffer_level(void);

#endif
