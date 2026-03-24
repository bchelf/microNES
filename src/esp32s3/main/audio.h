#pragma once

#include <stddef.h>
#include <stdint.h>

// Initialise LEDC PWM output for audio on BOARD_AUDIO_PIN.
// sample_rate: target sample rate in Hz (typically 48000).
void audio_init(uint32_t sample_rate);

// Push up to n_samples PCM samples (int16_t, mono) into the audio ring
// buffer.  Returns the number actually accepted.
size_t audio_push_samples(const int16_t *samples, size_t n_samples);

// Number of free slots in the audio ring buffer.
size_t audio_free_slots(void);
