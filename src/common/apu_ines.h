#ifndef MICRONES_APU_INES_H
#define MICRONES_APU_INES_H

/*
 * apu.h — InfoNES-based APU reimplementation
 *
 * This branch replaces the original cycle-accurate APU with the InfoNES
 * DDS (direct digital synthesis) approach from InfoNES_pAPU.cpp.  The
 * external API and compatibility fields are preserved so the rest of the
 * codebase compiles unchanged.
 *
 * Key differences from the original implementation:
 *  - Waveform synthesis uses a 32-bit phase accumulator (skip/index/table)
 *    instead of a hardware-style timer + sequencer.
 *  - Envelope, length counter, and sweep updates happen once per NES video
 *    frame (~60 Hz) in apu_vsync(), matching InfoNES's pAPUVsync() cadence.
 *  - No event queue — register writes take effect immediately.
 *  - Sample rate is still 48000 Hz; the magic constants are scaled from the
 *    InfoNES 44100 Hz values accordingly.
 */

#include "runtime_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    APU_OUTPUT_SAMPLE_RATE = 48000,
    APU_CPU_CLOCK_HZ       = 1789773,
    APU_PCM_CAPACITY       = 1536,
    APU_DEBUG_REGISTER_COUNT = 0x18,
};

typedef enum {
    APU_DEBUG_CHANNEL_PULSE1    = 0,
    APU_DEBUG_CHANNEL_PULSE2    = 1,
    APU_DEBUG_CHANNEL_TRIANGLE  = 2,
    APU_DEBUG_CHANNEL_NOISE     = 3,
    APU_DEBUG_CHANNEL_DMC       = 4,
    APU_DEBUG_CHANNEL_FINAL_MIX = 5,
    APU_DEBUG_CHANNEL_COUNT     = 6,
} ApuDebugChannel;

enum {
    APU_DEBUG_MASK_PULSE1   = 1u << APU_DEBUG_CHANNEL_PULSE1,
    APU_DEBUG_MASK_PULSE2   = 1u << APU_DEBUG_CHANNEL_PULSE2,
    APU_DEBUG_MASK_TRIANGLE = 1u << APU_DEBUG_CHANNEL_TRIANGLE,
    APU_DEBUG_MASK_NOISE    = 1u << APU_DEBUG_CHANNEL_NOISE,
    APU_DEBUG_MASK_DMC      = 1u << APU_DEBUG_CHANNEL_DMC,
    APU_DEBUG_MASK_ALL      = APU_DEBUG_MASK_PULSE1 | APU_DEBUG_MASK_PULSE2 |
                              APU_DEBUG_MASK_TRIANGLE | APU_DEBUG_MASK_NOISE |
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
    int32_t  min_value;
    int32_t  max_value;
    uint64_t abs_sum;
} ApuDebugSampleStats;

typedef struct {
    uint64_t write_count;
    uint8_t  last_value;
} ApuDebugRegisterSummary;

/* Snapshot types for ApuDebugReport — same layout as before. */
typedef struct {
    bool     enabled;
    bool     length_halt;
    bool     constant_volume;
    bool     sweep_enabled;
    uint8_t  duty;
    uint8_t  duty_step;
    uint8_t  volume_period;
    uint8_t  envelope_decay;
    uint8_t  length_counter;
    uint16_t timer_period;
    uint16_t timer_counter;
} ApuDebugPulseState;

typedef struct {
    bool     enabled;
    bool     control_flag;
    bool     linear_reload_flag;
    uint8_t  sequence_step;
    uint8_t  linear_reload_value;
    uint8_t  linear_counter;
    uint8_t  length_counter;
    uint16_t timer_period;
    uint16_t timer_counter;
} ApuDebugTriangleState;

typedef struct {
    bool     enabled;
    bool     length_halt;
    bool     constant_volume;
    bool     mode;
    uint8_t  volume_period;
    uint8_t  envelope_decay;
    uint8_t  length_counter;
    uint8_t  period_index;
    uint16_t timer_period;
    uint16_t timer_counter;
    uint16_t shift_register;
} ApuDebugNoiseState;

typedef struct {
    uint8_t                mix_enable_mask;
    ApuDebugTestTone       test_tone;
    uint64_t               dropped_samples;
    uint64_t               clip_count;
    ApuDebugSampleStats    channel_stats[APU_DEBUG_CHANNEL_COUNT];
    ApuDebugRegisterSummary register_summary[APU_DEBUG_REGISTER_COUNT];
    ApuDebugPulseState     pulse[2];
    ApuDebugTriangleState  triangle;
    ApuDebugNoiseState     noise;
    uint8_t                status;
    bool                   frame_counter_mode_5;
    bool                   frame_irq_inhibit;
} ApuDebugReport;

/* ------------------------------------------------------------------ */
/* Channel state structs (InfoNES DDS model)                           */
/* ------------------------------------------------------------------ */

/*
 * ApuPulseChannel
 *
 * Stores InfoNES-style DDS state for one rectangular wave channel.
 * The fields `enabled` and `length_counter` are also accessed directly
 * by pico/main.c, so their names and types are fixed.
 */
typedef struct {
    /* Accessed externally by pico/main.c */
    bool     enabled;
    uint8_t  length_counter;    /* active time left, in vsync frames */

    /* Raw NES registers ($4000–$4003 or $4004–$4007) */
    uint8_t  ra, rb, rc, rd;

    /* DDS phase accumulator */
    uint8_t  wave_idx;          /* index into pulse_waves[4] */
    uint32_t skip;              /* phase increment per output sample */
    uint32_t index;             /* current phase (29-bit, masked &0x1fffffff) */

    /* Envelope */
    int32_t  env_phase;
    uint8_t  envelope_decay;    /* current decayed volume (0–15); nes.c hashes this */

    /* Frequency sweep */
    int32_t  sweep_phase;

    /* 11-bit raw timer period — nes.c hashes this */
    uint16_t timer_period;

    /* DDS sequencer position (0–31) — nes.c hashes this */
    uint8_t  duty_step;

    /* Pulse 1 uses ones-complement negate in hardware; pulse 2 uses twos. */
    bool     ones_complement;
} ApuPulseChannel;

/*
 * ApuTriangleChannel
 */
typedef struct {
    bool     enabled;
    uint8_t  ra, rc, rd;
    uint8_t  length_counter;    /* active time left, in vsync frames */
    uint32_t llc;               /* linear length counter (scaled units) */
    bool     reload_flag;
    uint32_t skip;
    uint32_t index;
    /* nes.c hashes these — kept in sync */
    uint16_t timer_period;
    uint8_t  sequence_step;     /* index >> 24 */
    uint8_t  linear_counter;    /* llc >> 6, clamped 0–127 */
} ApuTriangleChannel;

/*
 * ApuNoiseChannel
 */
typedef struct {
    bool     enabled;
    uint8_t  ra, rc, rd;
    uint8_t  length_counter;    /* active time left, in vsync frames */
    uint32_t sr;                /* LFSR shift register (full 32-bit) */
    uint32_t skip;
    uint32_t index;
    uint8_t  envelope_decay;   /* current decayed volume (0–15); nes.c hashes this */
    int32_t  env_phase;
    /* nes.c hashes these — kept in sync */
    uint16_t timer_period;      /* k_noise_freq[rc & 0xf] */
    uint16_t shift_register;    /* sr & 0x7fff */
} ApuNoiseChannel;

/*
 * ApuDmcChannel  (minimal — DMC output is stubbed to zero in this branch)
 */
typedef struct {
    uint8_t  reg[4];
    uint8_t  enable, looping, cur_byte, dpcm_value;
    int      freq, phaseacc;
    uint16_t address, cache_addr;
    int      dma_length, cache_dma_length;
} ApuDmcChannel;

/* ------------------------------------------------------------------ */
/* Top-level APU state                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    /* ---- Compatibility fields (accessed directly by pico/main.c) ---- */
    uint8_t  registers[0x18];
    uint64_t register_write_count[APU_DEBUG_REGISTER_COUNT];
    uint8_t  mix_enable_mask;
    uint8_t  test_tone_mode;
    uint64_t cpu_cycles;            /* total CPU cycles ever processed */
    uint64_t sample_count;
    uint64_t dropped_samples;
    uint64_t clip_count;
    uint64_t frame_counter_steps;   /* NES video frames elapsed */
    uint32_t frame_counter_cycle;   /* cycles within current frame (= cycles_since_vsync); nes.c hashes this */
    uint32_t pcm_count;
    uint8_t  status;                /* last $4015 read result */
    bool     frame_counter_mode_5;  /* unused in InfoNES model; kept for compat */
    bool     frame_irq_inhibit;

    /* ---- InfoNES channel state ---- */
    ApuPulseChannel    pulse[2];
    ApuTriangleChannel triangle;
    ApuNoiseChannel    noise;
    ApuDmcChannel      dmc;

    /* Latched $4015 channel-enable bits (ctrl/ctrl_new in InfoNES) */
    uint8_t  ctrl;
    uint8_t  ctrl_new;

    /* ---- Timing ---- */
    uint32_t sample_phase;          /* fractional sample accumulator */
    uint32_t cycles_since_vsync;    /* CPU cycles since last vsync call */

    /* ---- PCM ring buffer ---- */
    uint32_t pcm_read_index;
    uint32_t pcm_write_index;
    int16_t  pcm[APU_PCM_CAPACITY];

    /* ---- Debug / metrics ---- */
    ApuDebugSampleStats channel_stats[APU_DEBUG_CHANNEL_COUNT];
    double   dc_level_tracker;
    uint32_t test_tone_phase[2];
} Apu;

/* ------------------------------------------------------------------ */
/* Public API (identical signatures to original apu.h)                 */
/* ------------------------------------------------------------------ */

void apu_init(Apu *apu);
void apu_reset(Apu *apu);
#if MICRONES_ENABLE_APU_EMULATION
void apu_step(Apu *apu, uint32_t cpu_cycles);
#else
static inline void apu_step(Apu *apu, uint32_t cpu_cycles) { (void)apu; (void)cpu_cycles; }
#endif
uint8_t apu_cpu_read(Apu *apu, uint16_t addr);
void    apu_cpu_write(Apu *apu, uint16_t addr, uint8_t value);

uint32_t apu_output_sample_rate(const Apu *apu);
size_t   apu_audio_available_samples(const Apu *apu);
size_t   apu_audio_read_samples(Apu *apu, int16_t *dst, size_t max_samples);

void             apu_debug_set_mix_enable_mask(Apu *apu, uint8_t mask);
uint8_t          apu_debug_mix_enable_mask(const Apu *apu);
void             apu_debug_set_test_tone(Apu *apu, ApuDebugTestTone mode);
ApuDebugTestTone apu_debug_test_tone(const Apu *apu);
void             apu_debug_reset_metrics(Apu *apu);
void             apu_debug_get_report(const Apu *apu, ApuDebugReport *report);
const char      *apu_debug_channel_name(ApuDebugChannel channel);
const char      *apu_debug_test_tone_name(ApuDebugTestTone mode);

#endif /* MICRONES_APU_INES_H */
