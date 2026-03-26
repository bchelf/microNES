#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t pushed_samples;
    uint64_t skipped_samples;
    uint64_t overflow_samples;
} AudioStats;

// Initialise I2S output for audio via MAX98357A on BOARD_AUDIO_*_PIN.
// sample_rate: target sample rate in Hz (typically 48000).
void audio_init(uint32_t sample_rate);

// Push up to n_samples PCM samples (int16_t, mono) into the audio ring
// buffer.  Returns the number actually accepted.
size_t audio_push_samples(const int16_t *samples, size_t n_samples);

// Number of free slots in the audio ring buffer.
size_t audio_free_slots(void);

// Cumulative audio frontend stats since boot.
AudioStats audio_stats_snapshot(void);
