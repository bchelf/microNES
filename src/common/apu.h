#ifndef SMB2350_APU_H
#define SMB2350_APU_H

#include "runtime_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    APU_OUTPUT_SAMPLE_RATE = 48000,
    APU_CPU_CLOCK_HZ = 1789773,
    APU_PCM_CAPACITY = 16384,
    APU_DEBUG_REGISTER_COUNT = 0x18,
};

typedef enum {
    APU_DEBUG_CHANNEL_PULSE1 = 0,
    APU_DEBUG_CHANNEL_PULSE2 = 1,
    APU_DEBUG_CHANNEL_TRIANGLE = 2,
    APU_DEBUG_CHANNEL_NOISE = 3,
    APU_DEBUG_CHANNEL_DMC = 4,
    APU_DEBUG_CHANNEL_FINAL_MIX = 5,
    APU_DEBUG_CHANNEL_COUNT = 6,
} ApuDebugChannel;

enum {
    APU_DEBUG_MASK_PULSE1 = 1u << APU_DEBUG_CHANNEL_PULSE1,
    APU_DEBUG_MASK_PULSE2 = 1u << APU_DEBUG_CHANNEL_PULSE2,
    APU_DEBUG_MASK_TRIANGLE = 1u << APU_DEBUG_CHANNEL_TRIANGLE,
    APU_DEBUG_MASK_NOISE = 1u << APU_DEBUG_CHANNEL_NOISE,
    APU_DEBUG_MASK_DMC = 1u << APU_DEBUG_CHANNEL_DMC,
    APU_DEBUG_MASK_ALL = APU_DEBUG_MASK_PULSE1 |
                         APU_DEBUG_MASK_PULSE2 |
                         APU_DEBUG_MASK_TRIANGLE |
                         APU_DEBUG_MASK_NOISE |
                         APU_DEBUG_MASK_DMC,
};

typedef enum {
    APU_DEBUG_TEST_TONE_NONE = 0,
    APU_DEBUG_TEST_TONE_PULSE1,
    APU_DEBUG_TEST_TONE_TRIANGLE,
} ApuDebugTestTone;

typedef struct {
    uint64_t sample_count;
    uint64_t nonzero_sample_count;
    int32_t min_value;
    int32_t max_value;
    uint64_t abs_sum;
} ApuDebugSampleStats;

typedef struct {
    uint64_t write_count;
    uint8_t last_value;
} ApuDebugRegisterSummary;

typedef struct {
    bool enabled;
    bool length_halt;
    bool constant_volume;
    bool sweep_enabled;
    uint8_t duty;
    uint8_t duty_step;
    uint8_t volume_period;
    uint8_t envelope_decay;
    uint8_t length_counter;
    uint16_t timer_period;
    uint16_t timer_counter;
} ApuDebugPulseState;

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
} ApuDebugTriangleState;

typedef struct {
    bool enabled;
    bool length_halt;
    bool constant_volume;
    bool mode;
    uint8_t volume_period;
    uint8_t envelope_decay;
    uint8_t length_counter;
    uint8_t period_index;
    uint16_t timer_period;
    uint16_t timer_counter;
    uint16_t shift_register;
} ApuDebugNoiseState;

typedef struct {
    uint8_t mix_enable_mask;
    ApuDebugTestTone test_tone;
    uint64_t dropped_samples;
    uint64_t clip_count;
    ApuDebugSampleStats channel_stats[APU_DEBUG_CHANNEL_COUNT];
    ApuDebugRegisterSummary register_summary[APU_DEBUG_REGISTER_COUNT];
    ApuDebugPulseState pulse[2];
    ApuDebugTriangleState triangle;
    ApuDebugNoiseState noise;
    uint8_t status;
    bool frame_counter_mode_5;
    bool frame_irq_inhibit;
} ApuDebugReport;

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
    uint64_t register_write_count[APU_DEBUG_REGISTER_COUNT];
    uint8_t mix_enable_mask;
    uint8_t test_tone_mode;
    uint64_t cpu_cycles;
    uint64_t sample_count;
    uint64_t dropped_samples;
    uint64_t clip_count;
    uint64_t frame_counter_cycle;
    uint64_t frame_counter_steps;
    uint32_t sample_phase;
    uint32_t pcm_read_index;
    uint32_t pcm_write_index;
    uint32_t pcm_count;
    uint32_t test_tone_phase[2];
    uint8_t status;
    bool frame_counter_mode_5;
    bool frame_irq_inhibit;
    int16_t pcm[APU_PCM_CAPACITY];
    double dc_prev_input;
    double dc_prev_output;
    ApuDebugSampleStats channel_stats[APU_DEBUG_CHANNEL_COUNT];
    ApuPulseChannel pulse[2];
    ApuTriangleChannel triangle;
    ApuNoiseChannel noise;
} Apu;

void apu_init(Apu *apu);
void apu_reset(Apu *apu);
#if SMB2350_ENABLE_APU_EMULATION
void apu_step(Apu *apu, uint32_t cpu_cycles);
#else
static inline void apu_step(Apu *apu, uint32_t cpu_cycles) { (void)apu; (void)cpu_cycles; }
#endif
uint8_t apu_cpu_read(Apu *apu, uint16_t addr);
void apu_cpu_write(Apu *apu, uint16_t addr, uint8_t value);

uint32_t apu_output_sample_rate(const Apu *apu);
size_t apu_audio_available_samples(const Apu *apu);
size_t apu_audio_read_samples(Apu *apu, int16_t *dst, size_t max_samples);

void apu_debug_set_mix_enable_mask(Apu *apu, uint8_t mask);
uint8_t apu_debug_mix_enable_mask(const Apu *apu);
void apu_debug_set_test_tone(Apu *apu, ApuDebugTestTone mode);
ApuDebugTestTone apu_debug_test_tone(const Apu *apu);
void apu_debug_reset_metrics(Apu *apu);
void apu_debug_get_report(const Apu *apu, ApuDebugReport *report);
const char *apu_debug_channel_name(ApuDebugChannel channel);
const char *apu_debug_test_tone_name(ApuDebugTestTone mode);

#endif
