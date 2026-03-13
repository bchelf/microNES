#ifndef SMB2350_APU_H
#define SMB2350_APU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    APU_OUTPUT_SAMPLE_RATE = 48000,
    APU_CPU_CLOCK_HZ = 1789773,
    APU_PCM_CAPACITY = 16384,
};

typedef struct {
    bool enabled;
    bool length_halt;
    bool constant_volume;
    bool envelope_start;
    bool sweep_enabled;
    bool sweep_negate;
    bool sweep_reload;
    uint8_t duty;
    uint8_t duty_step;
    uint8_t volume_period;
    uint8_t envelope_divider;
    uint8_t envelope_decay;
    uint8_t sweep_period;
    uint8_t sweep_divider;
    uint8_t sweep_shift;
    uint8_t length_counter;
    uint16_t timer_period;
    uint16_t timer_counter;
    bool sweep_ones_complement;
} ApuPulseChannel;

typedef struct {
    bool enabled;
    bool control_flag;
    bool linear_reload_flag;
    uint8_t sequence_step;
    uint8_t linear_reload_value;
    uint8_t linear_counter;
    uint8_t length_counter;
    uint16_t timer_period;
    uint16_t timer_counter;
} ApuTriangleChannel;

typedef struct {
    bool enabled;
    bool length_halt;
    bool constant_volume;
    bool envelope_start;
    bool mode;
    uint8_t volume_period;
    uint8_t envelope_divider;
    uint8_t envelope_decay;
    uint8_t length_counter;
    uint8_t period_index;
    uint16_t timer_period;
    uint16_t timer_counter;
    uint16_t shift_register;
} ApuNoiseChannel;

typedef struct {
    uint8_t registers[0x18];
    uint64_t cpu_cycles;
    uint64_t sample_count;
    uint64_t dropped_samples;
    uint64_t frame_counter_cycle;
    uint64_t frame_counter_steps;
    uint32_t sample_phase;
    uint32_t pcm_read_index;
    uint32_t pcm_write_index;
    uint32_t pcm_count;
    uint8_t status;
    bool frame_counter_mode_5;
    bool frame_irq_inhibit;
    int16_t pcm[APU_PCM_CAPACITY];
    double dc_prev_input;
    double dc_prev_output;
    ApuPulseChannel pulse[2];
    ApuTriangleChannel triangle;
    ApuNoiseChannel noise;
} Apu;

void apu_init(Apu *apu);
void apu_reset(Apu *apu);
void apu_step(Apu *apu, uint32_t cpu_cycles);
uint8_t apu_cpu_read(Apu *apu, uint16_t addr);
void apu_cpu_write(Apu *apu, uint16_t addr, uint8_t value);

uint32_t apu_output_sample_rate(const Apu *apu);
size_t apu_audio_available_samples(const Apu *apu);
size_t apu_audio_read_samples(Apu *apu, int16_t *dst, size_t max_samples);

#endif
