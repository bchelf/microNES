#include "apu.h"

#include <math.h>
#include <string.h>

enum {
    APU_FRAME_STEP_1 = 3729,
    APU_FRAME_STEP_2 = 7457,
    APU_FRAME_STEP_3 = 11186,
    APU_FRAME_STEP_4 = 14915,
    APU_FRAME_STEP_5 = 18641,
};

static const double k_apu_triangle_mix_boost = 1.35;
static const double k_apu_noise_mix_boost = 1.10;

static const uint8_t k_apu_length_table[32] = {
    10, 254, 20,  2, 40,  4, 80,  6,
    160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22,
    192, 24, 72, 26, 16, 28, 32, 30,
};

static const uint8_t k_apu_pulse_duty[4][8] = {
    { 0, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 1, 1, 0, 0, 0, 0, 0 },
    { 0, 1, 1, 1, 1, 0, 0, 0 },
    { 1, 0, 0, 1, 1, 1, 1, 1 },
};

static const uint8_t k_apu_triangle_sequence[32] = {
    15, 14, 13, 12, 11, 10, 9, 8,
    7,  6,  5,  4,  3,  2,  1, 0,
    0,  1,  2,  3,  4,  5,  6, 7,
    8,  9, 10, 11, 12, 13, 14, 15,
};

static const uint16_t k_apu_noise_period_table[16] = {
    4, 8, 16, 32, 64, 96, 128, 160,
    202, 254, 380, 508, 762, 1016, 2034, 4068,
};

enum {
    APU_TEST_TONE_PULSE_PHASE = 0,
    APU_TEST_TONE_TRIANGLE_PHASE = 1,
};

static const uint32_t k_apu_test_tone_step_pulse1 =
    (uint32_t)((440ull << 32) / APU_OUTPUT_SAMPLE_RATE);
static const uint32_t k_apu_test_tone_step_triangle =
    (uint32_t)((220ull << 32) / APU_OUTPUT_SAMPLE_RATE);

static void apu_stats_note(ApuDebugSampleStats *stats, int32_t value) {
    if (stats->sample_count == 0) {
        stats->min_value = value;
        stats->max_value = value;
    } else {
        if (value < stats->min_value) {
            stats->min_value = value;
        }
        if (value > stats->max_value) {
            stats->max_value = value;
        }
    }
    ++stats->sample_count;
    if (value != 0) {
        ++stats->nonzero_sample_count;
    }
    stats->abs_sum += (uint64_t)(value < 0 ? -value : value);
}

static void apu_pcm_push(Apu *apu, int16_t sample) {
    if (apu->pcm_count >= APU_PCM_CAPACITY) {
        ++apu->dropped_samples;
        return;
    }

    apu->pcm[apu->pcm_write_index] = sample;
    apu->pcm_write_index = (apu->pcm_write_index + 1u) % APU_PCM_CAPACITY;
    ++apu->pcm_count;
    ++apu->sample_count;
}

static void apu_capture_pulse_state(const ApuPulseChannel *src, ApuDebugPulseState *dst) {
    dst->enabled = src->enabled;
    dst->length_halt = src->length_halt;
    dst->constant_volume = src->constant_volume;
    dst->sweep_enabled = src->sweep_enabled;
    dst->duty = src->duty;
    dst->duty_step = src->duty_step;
    dst->volume_period = src->volume_period;
    dst->envelope_decay = src->envelope_decay;
    dst->length_counter = src->length_counter;
    dst->timer_period = src->timer_period;
    dst->timer_counter = src->timer_counter;
}

static void apu_capture_triangle_state(const ApuTriangleChannel *src, ApuDebugTriangleState *dst) {
    dst->enabled = src->enabled;
    dst->control_flag = src->control_flag;
    dst->linear_reload_flag = src->linear_reload_flag;
    dst->sequence_step = src->sequence_step;
    dst->linear_reload_value = src->linear_reload_value;
    dst->linear_counter = src->linear_counter;
    dst->length_counter = src->length_counter;
    dst->timer_period = src->timer_period;
    dst->timer_counter = src->timer_counter;
}

static void apu_capture_noise_state(const ApuNoiseChannel *src, ApuDebugNoiseState *dst) {
    dst->enabled = src->enabled;
    dst->length_halt = src->length_halt;
    dst->constant_volume = src->constant_volume;
    dst->mode = src->mode;
    dst->volume_period = src->volume_period;
    dst->envelope_decay = src->envelope_decay;
    dst->length_counter = src->length_counter;
    dst->period_index = src->period_index;
    dst->timer_period = src->timer_period;
    dst->timer_counter = src->timer_counter;
    dst->shift_register = src->shift_register;
}

static void apu_reset_debug_defaults(Apu *apu) {
    apu->mix_enable_mask = APU_DEBUG_MASK_ALL;
    apu->test_tone_mode = (uint8_t)APU_DEBUG_TEST_TONE_NONE;
    apu->test_tone_phase[APU_TEST_TONE_PULSE_PHASE] = 0;
    apu->test_tone_phase[APU_TEST_TONE_TRIANGLE_PHASE] = 0;
}

static uint16_t apu_pulse_sweep_target(const ApuPulseChannel *pulse) {
    uint16_t change = pulse->timer_period >> pulse->sweep_shift;
    if (pulse->sweep_negate) {
        return (uint16_t)(pulse->timer_period - change - (pulse->sweep_ones_complement ? 1u : 0u));
    }
    return (uint16_t)(pulse->timer_period + change);
}

static bool apu_pulse_sweep_muted(const ApuPulseChannel *pulse) {
    if (pulse->timer_period < 8u) {
        return true;
    }
    if (pulse->sweep_shift == 0u) {
        return false;
    }
    return apu_pulse_sweep_target(pulse) > 0x07ffu;
}

static uint8_t apu_pulse_volume(const ApuPulseChannel *pulse) {
    return pulse->constant_volume ? pulse->volume_period : pulse->envelope_decay;
}

static uint8_t apu_noise_volume(const ApuNoiseChannel *noise) {
    return noise->constant_volume ? noise->volume_period : noise->envelope_decay;
}

static void apu_clock_envelope(ApuPulseChannel *pulse) {
    if (pulse->envelope_start) {
        pulse->envelope_start = false;
        pulse->envelope_decay = 15;
        pulse->envelope_divider = pulse->volume_period;
        return;
    }

    if (pulse->envelope_divider > 0) {
        --pulse->envelope_divider;
        return;
    }

    pulse->envelope_divider = pulse->volume_period;
    if (pulse->envelope_decay > 0) {
        --pulse->envelope_decay;
    } else if (pulse->length_halt) {
        pulse->envelope_decay = 15;
    }
}

static void apu_clock_noise_envelope(ApuNoiseChannel *noise) {
    if (noise->envelope_start) {
        noise->envelope_start = false;
        noise->envelope_decay = 15;
        noise->envelope_divider = noise->volume_period;
        return;
    }

    if (noise->envelope_divider > 0) {
        --noise->envelope_divider;
        return;
    }

    noise->envelope_divider = noise->volume_period;
    if (noise->envelope_decay > 0) {
        --noise->envelope_decay;
    } else if (noise->length_halt) {
        noise->envelope_decay = 15;
    }
}

static void apu_clock_length(ApuPulseChannel *pulse) {
    if (!pulse->enabled) {
        pulse->length_counter = 0;
    } else if (!pulse->length_halt && pulse->length_counter > 0) {
        --pulse->length_counter;
    }
}

static void apu_clock_triangle_length(ApuTriangleChannel *triangle) {
    if (!triangle->enabled) {
        triangle->length_counter = 0;
    } else if (!triangle->control_flag && triangle->length_counter > 0) {
        --triangle->length_counter;
    }
}

static void apu_clock_noise_length(ApuNoiseChannel *noise) {
    if (!noise->enabled) {
        noise->length_counter = 0;
    } else if (!noise->length_halt && noise->length_counter > 0) {
        --noise->length_counter;
    }
}

static void apu_clock_triangle_linear(ApuTriangleChannel *triangle) {
    if (triangle->linear_reload_flag) {
        triangle->linear_counter = triangle->linear_reload_value;
    } else if (triangle->linear_counter > 0) {
        --triangle->linear_counter;
    }

    if (!triangle->control_flag) {
        triangle->linear_reload_flag = false;
    }
}

static void apu_clock_sweep(ApuPulseChannel *pulse) {
    bool do_update = (pulse->sweep_divider == 0) &&
                     pulse->sweep_enabled &&
                     pulse->sweep_shift > 0 &&
                     !apu_pulse_sweep_muted(pulse);

    if (do_update) {
        pulse->timer_period = apu_pulse_sweep_target(pulse);
    }

    if (pulse->sweep_divider == 0 || pulse->sweep_reload) {
        /* Spec: divider period is P+1 half-frames, so reload to P. */
        pulse->sweep_divider = pulse->sweep_period;
        pulse->sweep_reload = false;
    } else {
        --pulse->sweep_divider;
    }
}

static void apu_quarter_frame(Apu *apu) {
    apu_clock_envelope(&apu->pulse[0]);
    apu_clock_envelope(&apu->pulse[1]);
    apu_clock_noise_envelope(&apu->noise);
    apu_clock_triangle_linear(&apu->triangle);
}

static void apu_half_frame(Apu *apu) {
    apu_clock_length(&apu->pulse[0]);
    apu_clock_length(&apu->pulse[1]);
    apu_clock_triangle_length(&apu->triangle);
    apu_clock_noise_length(&apu->noise);
    apu_clock_sweep(&apu->pulse[0]);
    apu_clock_sweep(&apu->pulse[1]);
}

static void apu_clock_frame_counter(Apu *apu) {
    ++apu->frame_counter_cycle;

    if (!apu->frame_counter_mode_5) {
        switch (apu->frame_counter_cycle) {
        case APU_FRAME_STEP_1:
        case APU_FRAME_STEP_3:
            apu_quarter_frame(apu);
            break;
        case APU_FRAME_STEP_2:
            apu_quarter_frame(apu);
            apu_half_frame(apu);
            break;
        case APU_FRAME_STEP_4:
            apu_quarter_frame(apu);
            apu_half_frame(apu);
            apu->frame_counter_cycle = 0;
            ++apu->frame_counter_steps;
            break;
        default:
            break;
        }
        return;
    }

    switch (apu->frame_counter_cycle) {
    case APU_FRAME_STEP_1:
    case APU_FRAME_STEP_3:
        apu_quarter_frame(apu);
        break;
    case APU_FRAME_STEP_2:
        apu_quarter_frame(apu);
        apu_half_frame(apu);
        break;
    case APU_FRAME_STEP_5:
        apu_quarter_frame(apu);
        apu_half_frame(apu);
        apu->frame_counter_cycle = 0;
        ++apu->frame_counter_steps;
        break;
    default:
        break;
    }
}

static void apu_clock_pulse_timer(ApuPulseChannel *pulse) {
    if (pulse->timer_counter == 0) {
        pulse->timer_counter = pulse->timer_period;
        pulse->duty_step = (uint8_t)((pulse->duty_step + 1u) & 0x07u);
    } else {
        --pulse->timer_counter;
    }
}

static void apu_clock_triangle_timer(ApuTriangleChannel *triangle) {
    if (triangle->timer_counter == 0) {
        triangle->timer_counter = triangle->timer_period;
        if (triangle->enabled &&
            triangle->length_counter > 0 &&
            triangle->linear_counter > 0 &&
            triangle->timer_period >= 2u) {
            triangle->sequence_step = (uint8_t)((triangle->sequence_step + 1u) & 0x1fu);
        }
    } else {
        --triangle->timer_counter;
    }
}

static void apu_clock_noise_timer(ApuNoiseChannel *noise) {
    uint16_t feedback;
    uint8_t tap_bit;

    if (noise->timer_counter == 0) {
        noise->timer_counter = noise->timer_period;
        tap_bit = noise->mode ? 6u : 1u;
        feedback = (uint16_t)((noise->shift_register & 0x0001u) ^
                              ((noise->shift_register >> tap_bit) & 0x0001u));
        noise->shift_register >>= 1;
        noise->shift_register |= (uint16_t)(feedback << 14);
    } else {
        --noise->timer_counter;
    }
}

static uint8_t apu_pulse_output(const ApuPulseChannel *pulse) {
    if (!pulse->enabled ||
        pulse->length_counter == 0 ||
        apu_pulse_sweep_muted(pulse) ||
        pulse->timer_period < 8u ||
        k_apu_pulse_duty[pulse->duty][pulse->duty_step] == 0) {
        return 0;
    }

    return apu_pulse_volume(pulse);
}

static uint8_t apu_triangle_output(const ApuTriangleChannel *triangle) {
    if (!triangle->enabled ||
        triangle->length_counter == 0 ||
        triangle->linear_counter == 0 ||
        triangle->timer_period < 2u) {
        return 0;
    }

    return k_apu_triangle_sequence[triangle->sequence_step];
}

static uint8_t apu_noise_output(const ApuNoiseChannel *noise) {
    if (!noise->enabled ||
        noise->length_counter == 0 ||
        (noise->shift_register & 0x0001u) != 0) {
        return 0;
    }

    return apu_noise_volume(noise);
}

static int16_t apu_mix_sample(Apu *apu) {
    int32_t pulse1_raw = (int32_t)apu_pulse_output(&apu->pulse[0]);
    int32_t pulse2_raw = (int32_t)apu_pulse_output(&apu->pulse[1]);
    int32_t triangle_raw = (int32_t)apu_triangle_output(&apu->triangle);
    int32_t noise_raw = (int32_t)apu_noise_output(&apu->noise);
    int32_t dmc_raw = 0;
    double pulse1;
    double pulse2;
    double triangle;
    double noise;
    double pulse_sum;
    double pulse_out = 0.0;
    double tnd_out = 0.0;
    double mixed;
    int sample;

    if (apu->test_tone_mode == APU_DEBUG_TEST_TONE_PULSE1) {
        apu->test_tone_phase[APU_TEST_TONE_PULSE_PHASE] += k_apu_test_tone_step_pulse1;
        pulse1_raw = (apu->test_tone_phase[APU_TEST_TONE_PULSE_PHASE] & 0x80000000u) ? 12 : 0;
        pulse2_raw = 0;
        triangle_raw = 0;
        noise_raw = 0;
    } else if (apu->test_tone_mode == APU_DEBUG_TEST_TONE_TRIANGLE) {
        uint32_t phase;
        uint32_t step;
        int32_t tri;

        apu->test_tone_phase[APU_TEST_TONE_TRIANGLE_PHASE] += k_apu_test_tone_step_triangle;
        phase = apu->test_tone_phase[APU_TEST_TONE_TRIANGLE_PHASE];
        step = phase >> 27;
        tri = (step < 16u) ? (int32_t)step : (int32_t)(31u - step);
        pulse1_raw = 0;
        pulse2_raw = 0;
        triangle_raw = tri;
        noise_raw = 0;
    }

    apu_stats_note(&apu->channel_stats[APU_DEBUG_CHANNEL_PULSE1], pulse1_raw);
    apu_stats_note(&apu->channel_stats[APU_DEBUG_CHANNEL_PULSE2], pulse2_raw);
    apu_stats_note(&apu->channel_stats[APU_DEBUG_CHANNEL_TRIANGLE], triangle_raw);
    apu_stats_note(&apu->channel_stats[APU_DEBUG_CHANNEL_NOISE], noise_raw);
    apu_stats_note(&apu->channel_stats[APU_DEBUG_CHANNEL_DMC], dmc_raw);

    pulse1 = (apu->mix_enable_mask & APU_DEBUG_MASK_PULSE1) ? (double)pulse1_raw : 0.0;
    pulse2 = (apu->mix_enable_mask & APU_DEBUG_MASK_PULSE2) ? (double)pulse2_raw : 0.0;
    triangle = (apu->mix_enable_mask & APU_DEBUG_MASK_TRIANGLE) ?
        (double)triangle_raw * k_apu_triangle_mix_boost : 0.0;
    noise = (apu->mix_enable_mask & APU_DEBUG_MASK_NOISE) ?
        (double)noise_raw * k_apu_noise_mix_boost : 0.0;
    pulse_sum = pulse1 + pulse2;

    if (pulse_sum > 0.0) {
        pulse_out = 95.88 / ((8128.0 / pulse_sum) + 100.0);
    }
    if ((triangle + noise) > 0.0) {
        tnd_out = 159.79 / ((1.0 / ((triangle / 8227.0) + (noise / 12241.0))) + 100.0);
    }

    mixed = pulse_out + tnd_out;

    /* No DC filter: the derivative-form filter (y = x - x_prev + a*y_prev)
     * produces a full-amplitude negative spike whenever the mixer output drops
     * (e.g. length counter silencing a channel mid-waveform). That spike decays
     * over ~26ms at this sample rate, audible as a thump at effect end. The NES
     * mixer output is already 0 at true silence so there is no DC to remove. */
    sample = (int)lrint(mixed * 32767.0 * 0.85);
    if (sample < -32768) {
        sample = -32768;
        ++apu->clip_count;
    } else if (sample > 32767) {
        sample = 32767;
        ++apu->clip_count;
    }
    apu_stats_note(&apu->channel_stats[APU_DEBUG_CHANNEL_FINAL_MIX], sample);
    return (int16_t)sample;
}

static void apu_clock_sample_output(Apu *apu) {
    apu->sample_phase += APU_OUTPUT_SAMPLE_RATE;
    while (apu->sample_phase >= APU_CPU_CLOCK_HZ) {
        apu->sample_phase -= APU_CPU_CLOCK_HZ;
        apu_pcm_push(apu, apu_mix_sample(apu));
    }
}

void apu_init(Apu *apu) {
    memset(apu, 0, sizeof(*apu));
    apu->pulse[0].sweep_ones_complement = true;
    apu->noise.shift_register = 1;
    apu_reset_debug_defaults(apu);
}

void apu_reset(Apu *apu) {
    memset(apu, 0, sizeof(*apu));
    apu->pulse[0].sweep_ones_complement = true;
    apu->noise.shift_register = 1;
    apu_reset_debug_defaults(apu);
}

void apu_step(Apu *apu, uint32_t cpu_cycles) {
    for (uint32_t i = 0; i < cpu_cycles; ++i) {
        ++apu->cpu_cycles;
        apu_clock_triangle_timer(&apu->triangle);
        /* Pulse timers, noise, and frame counter are all clocked at APU rate
         * (every other CPU cycle). Frame counter thresholds are in APU cycles. */
        if ((apu->cpu_cycles & 1u) == 0u) {
            apu_clock_frame_counter(apu);
            apu_clock_pulse_timer(&apu->pulse[0]);
            apu_clock_pulse_timer(&apu->pulse[1]);
            apu_clock_noise_timer(&apu->noise);
        }
        apu_clock_sample_output(apu);
    }
}

uint8_t apu_cpu_read(Apu *apu, uint16_t addr) {
    uint8_t status;

    if (addr != 0x4015u) {
        return 0;
    }

    status = 0;
    if (apu->pulse[0].length_counter > 0) {
        status |= 0x01u;
    }
    if (apu->pulse[1].length_counter > 0) {
        status |= 0x02u;
    }
    if (apu->triangle.length_counter > 0) {
        status |= 0x04u;
    }
    if (apu->noise.length_counter > 0) {
        status |= 0x08u;
    }
    apu->status = status;
    return status;
}

void apu_cpu_write(Apu *apu, uint16_t addr, uint8_t value) {
    if (addr >= 0x4000u && addr <= 0x4017u) {
        apu->registers[addr - 0x4000u] = value;
        ++apu->register_write_count[addr - 0x4000u];
    }

    switch (addr) {
    case 0x4000u:
    case 0x4004u: {
        ApuPulseChannel *pulse = &apu->pulse[(addr - 0x4000u) / 4u];
        pulse->duty = (uint8_t)((value >> 6) & 0x03u);
        pulse->length_halt = (value & 0x20u) != 0;
        pulse->constant_volume = (value & 0x10u) != 0;
        pulse->volume_period = value & 0x0fu;
        break;
    }
    case 0x4001u:
    case 0x4005u: {
        ApuPulseChannel *pulse = &apu->pulse[(addr - 0x4001u) / 4u];
        pulse->sweep_enabled = (value & 0x80u) != 0;
        pulse->sweep_period = (uint8_t)((value >> 4) & 0x07u);
        pulse->sweep_negate = (value & 0x08u) != 0;
        pulse->sweep_shift = value & 0x07u;
        pulse->sweep_reload = true;
        break;
    }
    case 0x4002u:
    case 0x4006u: {
        ApuPulseChannel *pulse = &apu->pulse[(addr - 0x4002u) / 4u];
        pulse->timer_period = (uint16_t)((pulse->timer_period & 0x0700u) | value);
        /* Do not reset timer_counter here; only $4003/$4007 restarts the timer. */
        break;
    }
    case 0x4003u:
    case 0x4007u: {
        ApuPulseChannel *pulse = &apu->pulse[(addr - 0x4003u) / 4u];
        pulse->timer_period = (uint16_t)((pulse->timer_period & 0x00ffu) | ((uint16_t)(value & 0x07u) << 8));
        if (pulse->enabled) {
            pulse->length_counter = k_apu_length_table[(value >> 3) & 0x1fu];
        }
        pulse->timer_counter = pulse->timer_period;
        pulse->duty_step = 0;
        pulse->envelope_start = true;
        break;
    }
    case 0x4008u:
        apu->triangle.control_flag = (value & 0x80u) != 0;
        apu->triangle.linear_reload_value = value & 0x7fu;
        break;
    case 0x400au:
        apu->triangle.timer_period = (uint16_t)((apu->triangle.timer_period & 0x0700u) | value);
        /* Do not reset timer_counter here; only $400B restarts the timer. */
        break;
    case 0x400bu:
        apu->triangle.timer_period = (uint16_t)((apu->triangle.timer_period & 0x00ffu) | ((uint16_t)(value & 0x07u) << 8));
        if (apu->triangle.enabled) {
            apu->triangle.length_counter = k_apu_length_table[(value >> 3) & 0x1fu];
        }
        apu->triangle.timer_counter = apu->triangle.timer_period;
        apu->triangle.linear_reload_flag = true;
        break;
    case 0x400cu:
        apu->noise.length_halt = (value & 0x20u) != 0;
        apu->noise.constant_volume = (value & 0x10u) != 0;
        apu->noise.volume_period = value & 0x0fu;
        break;
    case 0x400eu:
        apu->noise.mode = (value & 0x80u) != 0;
        apu->noise.period_index = value & 0x0fu;
        apu->noise.timer_period = k_apu_noise_period_table[apu->noise.period_index];
        apu->noise.timer_counter = apu->noise.timer_period;
        break;
    case 0x400fu:
        if (apu->noise.enabled) {
            apu->noise.length_counter = k_apu_length_table[(value >> 3) & 0x1fu];
        }
        apu->noise.timer_counter = apu->noise.timer_period;
        apu->noise.envelope_start = true;
        break;
    case 0x4015u:
        apu->pulse[0].enabled = (value & 0x01u) != 0;
        apu->pulse[1].enabled = (value & 0x02u) != 0;
        apu->triangle.enabled = (value & 0x04u) != 0;
        apu->noise.enabled = (value & 0x08u) != 0;
        if (!apu->pulse[0].enabled) {
            apu->pulse[0].length_counter = 0;
        }
        if (!apu->pulse[1].enabled) {
            apu->pulse[1].length_counter = 0;
        }
        if (!apu->triangle.enabled) {
            apu->triangle.length_counter = 0;
        }
        if (!apu->noise.enabled) {
            apu->noise.length_counter = 0;
        }
        apu->status = value & 0x0fu;
        break;
    case 0x4017u:
        apu->frame_counter_mode_5 = (value & 0x80u) != 0;
        apu->frame_irq_inhibit = (value & 0x40u) != 0;
        apu->frame_counter_cycle = 0;
        if (apu->frame_counter_mode_5) {
            apu_quarter_frame(apu);
            apu_half_frame(apu);
        }
        break;
    default:
        break;
    }
}

uint32_t apu_output_sample_rate(const Apu *apu) {
    (void)apu;
    return APU_OUTPUT_SAMPLE_RATE;
}

size_t apu_audio_available_samples(const Apu *apu) {
    return apu->pcm_count;
}

size_t apu_audio_read_samples(Apu *apu, int16_t *dst, size_t max_samples) {
    size_t count = apu->pcm_count;

    if (count > max_samples) {
        count = max_samples;
    }

    for (size_t i = 0; i < count; ++i) {
        dst[i] = apu->pcm[apu->pcm_read_index];
        apu->pcm_read_index = (apu->pcm_read_index + 1u) % APU_PCM_CAPACITY;
    }
    apu->pcm_count -= (uint32_t)count;
    return count;
}

void apu_debug_set_mix_enable_mask(Apu *apu, uint8_t mask) {
    apu->mix_enable_mask = (uint8_t)(mask & APU_DEBUG_MASK_ALL);
}

uint8_t apu_debug_mix_enable_mask(const Apu *apu) {
    return apu->mix_enable_mask;
}

void apu_debug_set_test_tone(Apu *apu, ApuDebugTestTone mode) {
    apu->test_tone_mode = (uint8_t)mode;
    apu->test_tone_phase[APU_TEST_TONE_PULSE_PHASE] = 0;
    apu->test_tone_phase[APU_TEST_TONE_TRIANGLE_PHASE] = 0;
}

ApuDebugTestTone apu_debug_test_tone(const Apu *apu) {
    return (ApuDebugTestTone)apu->test_tone_mode;
}

void apu_debug_reset_metrics(Apu *apu) {
    memset(apu->channel_stats, 0, sizeof(apu->channel_stats));
    memset(apu->register_write_count, 0, sizeof(apu->register_write_count));
    apu->dropped_samples = 0;
    apu->clip_count = 0;
}

void apu_debug_get_report(const Apu *apu, ApuDebugReport *report) {
    memset(report, 0, sizeof(*report));
    report->mix_enable_mask = apu->mix_enable_mask;
    report->test_tone = (ApuDebugTestTone)apu->test_tone_mode;
    report->dropped_samples = apu->dropped_samples;
    report->clip_count = apu->clip_count;
    memcpy(report->channel_stats, apu->channel_stats, sizeof(report->channel_stats));
    for (size_t i = 0; i < APU_DEBUG_REGISTER_COUNT; ++i) {
        report->register_summary[i].write_count = apu->register_write_count[i];
        report->register_summary[i].last_value = apu->registers[i];
    }
    apu_capture_pulse_state(&apu->pulse[0], &report->pulse[0]);
    apu_capture_pulse_state(&apu->pulse[1], &report->pulse[1]);
    apu_capture_triangle_state(&apu->triangle, &report->triangle);
    apu_capture_noise_state(&apu->noise, &report->noise);
    report->status = apu->status;
    report->frame_counter_mode_5 = apu->frame_counter_mode_5;
    report->frame_irq_inhibit = apu->frame_irq_inhibit;
}

const char *apu_debug_channel_name(ApuDebugChannel channel) {
    switch (channel) {
    case APU_DEBUG_CHANNEL_PULSE1:
        return "pulse1";
    case APU_DEBUG_CHANNEL_PULSE2:
        return "pulse2";
    case APU_DEBUG_CHANNEL_TRIANGLE:
        return "triangle";
    case APU_DEBUG_CHANNEL_NOISE:
        return "noise";
    case APU_DEBUG_CHANNEL_DMC:
        return "dmc";
    case APU_DEBUG_CHANNEL_FINAL_MIX:
        return "mix";
    default:
        return "unknown";
    }
}

const char *apu_debug_test_tone_name(ApuDebugTestTone mode) {
    switch (mode) {
    case APU_DEBUG_TEST_TONE_PULSE1:
        return "pulse1";
    case APU_DEBUG_TEST_TONE_TRIANGLE:
        return "triangle";
    default:
        return "off";
    }
}
