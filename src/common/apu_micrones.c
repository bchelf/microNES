#include "apu_micrones.h"

#include <math.h>
#include <string.h>

/* Frame counter step thresholds in CPU cycles (not APU cycles).
 * Per NESdev, these are the CPU cycle counts from frame counter reset at
 * which quarter-frame and half-frame events fire for NTSC.
 *
 * The frame counter cycle count is now tracked in CPU cycles for precision.
 *
 * NESdev values (CPU cycles from reset):
 *   Step 1: 7457  — quarter frame
 *   Step 2: 14913 — quarter + half frame (length counter clock)
 *   Step 3: 22371 — quarter frame
 *   Step 4: 29829 — quarter + half frame + IRQ (4-step mode wraps here)
 *   Step 5: 37281 — quarter + half frame (5-step mode wraps here)
 *
 * The frame counter fires at the END of the CPU cycle at these counts,
 * i.e., when frame_counter_cycle transitions FROM < N TO >= N.
 */
/* APU frame counter timing, derived from 5-len_timing test source:
 *   With even-cycle $4017 write (delay=4 CPU cycles):
 *     Step 1 (quarter):      fires at cycle 7456 from reset  (7460 from write)
 *     Step 2 (quarter+half): fires at cycle 14912 from reset (14916 from write)
 *     Step 3 (quarter):      fires at cycle 22368 from reset (22372 from write)
 *     Step 4 (quarter+half): fires at cycle 29828 from reset (29832 from write)
 *     Frame period:          29830 cycles (wrap after step 4 + 2 dead cycles)
 *
 *     IRQ fires at cycles 29827, 29828, 29829 (three cycles, latch).
 *
 *   5-step mode:
 *     Step 1: 7456, Step 2: 14912, Step 3: 22368
 *     Step 5 (quarter+half): fires at cycle 37280 from reset (37284 from write)
 *     Frame period: 37282 cycles
 *
 * $4017 write delay: 4 CPU cycles for even-cycle write, 3 for odd.
 * Handled dynamically in apu_cpu_write.
 */
enum {
    APU_FRAME_STEP_1   = 7456,
    APU_FRAME_STEP_2   = 14912,
    APU_FRAME_STEP_3   = 22368,
    APU_FRAME_IRQ_PRE  = 29827,  /* first IRQ cycle (one before step 4) */
    APU_FRAME_STEP_4   = 29828,  /* quarter+half+IRQ (second IRQ cycle) */
    APU_FRAME_IRQ_3    = 29829,  /* third and final IRQ cycle */
    APU_FRAME_PERIOD_4 = 29830,  /* total 4-step frame period; wrap point */
    APU_FRAME_STEP_5   = 37280,  /* quarter+half, 5-step mode */
    APU_FRAME_PERIOD_5 = 37282,  /* total 5-step frame period */
};

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

/* DMC rate table (CPU cycles per DMC timer tick) — NTSC */
static const uint16_t k_apu_dmc_rate_table[16] = {
    428, 380, 340, 320, 286, 254, 226, 214,
    190, 160, 142, 128, 106,  84,  72,  54,
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
#if MICRONES_ENABLE_APU_DEBUG_METRICS
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
#else
    (void)stats;
    (void)value;
#endif
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
        /* When reloading from silence (counter was 0), reset the sequencer to
         * a zero-value position so the waveform begins at the bottom of its
         * cycle.  This prevents the click caused by the output jumping from
         * silence to an arbitrary mid-waveform amplitude at note-on.  The
         * real 2A03 does not reset the sequencer here, but without the NES
         * hardware's analog output smoothing the discontinuity is clearly
         * audible in digital emulation. */
        if (triangle->linear_counter == 0) {
            triangle->sequence_step = 15; /* sequence value 0 — zero crossing */
        }
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

/* The three per-cycle clock_*_timer functions below are kept only for unit
 * test reference. They are NOT called from the normal emulation path; timers
 * are advanced via the batched apu_timer_advance() calls in apu_step(). */
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
        triangle->timer_period < 8u) {
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

static int16_t __attribute__((noinline)) MICRONES_HOT_FUNC(apu_mix_sample)(Apu *apu) {
    int32_t pulse1_raw = (int32_t)apu_pulse_output(&apu->pulse[0]);
    int32_t pulse2_raw = (int32_t)apu_pulse_output(&apu->pulse[1]);
    int32_t triangle_raw = (int32_t)apu_triangle_output(&apu->triangle);
    int32_t noise_raw = (int32_t)apu_noise_output(&apu->noise);
    int32_t dmc_raw = 0;
    float pulse1;
    float pulse2;
    float triangle;
    float noise;
    float pulse_sum;
    float pulse_out = 0.0f;
    float tnd_out = 0.0f;
    float mixed;
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

    pulse1 = (apu->mix_enable_mask & APU_DEBUG_MASK_PULSE1) ? (float)pulse1_raw : 0.0f;
    pulse2 = (apu->mix_enable_mask & APU_DEBUG_MASK_PULSE2) ? (float)pulse2_raw : 0.0f;
    triangle = (apu->mix_enable_mask & APU_DEBUG_MASK_TRIANGLE) ? (float)triangle_raw : 0.0f;
    noise = (apu->mix_enable_mask & APU_DEBUG_MASK_NOISE) ? (float)noise_raw : 0.0f;
    pulse_sum = pulse1 + pulse2;

    if (pulse_sum > 0.0f) {
        pulse_out = 95.88f / ((8128.0f / pulse_sum) + 100.0f);
    }
    if ((triangle + noise) > 0.0f) {
        tnd_out = 159.79f / ((1.0f / ((triangle / 8227.0f) + (noise / 12241.0f))) + 100.0f);
    }

    mixed = pulse_out + tnd_out;

    /* Leaky DC-level tracker. Chases the mean of `mixed` at ~0.76 Hz.
     * Subtracting it removes DC offset without creating step discontinuities
     * at note-on/off transitions — equivalent to the analog capacitor coupling
     * on the real NES output circuit. R = 0.9999 per sample at 48 kHz.
     * A faster constant (e.g. matching the NES RC filter at ~16 Hz) decays
     * the baseline to zero between notes, making the amplitude jump at
     * note-on larger and the click more audible — so keep it slow. */
    apu->dc_level_tracker += ((double)mixed - apu->dc_level_tracker) * 0.0001;
    mixed = mixed - (float)apu->dc_level_tracker;

    sample = (int)lrintf(mixed * 32767.0f);
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

/* Advance a channel timer by n_cycles. Returns the number of times it fired.
 * Timers count down from period to 0, fire, then reset to period.
 * Period length in cycles = timer_period + 1. */
static uint32_t MICRONES_HOT_FUNC(apu_timer_advance)(uint16_t *counter,
                                                     uint16_t  period,
                                                     uint32_t  n_cycles) {
    if (*counter >= n_cycles) {
        *counter -= (uint16_t)n_cycles;
        return 0;
    }
    uint32_t remaining = n_cycles - (uint32_t)*counter - 1u;
    uint32_t period1   = (uint32_t)period + 1u;
    uint32_t fires     = 1u + remaining / period1;
    *counter = (uint16_t)(period - remaining % period1);
    return fires;
}

static void apu_dmc_compute_start_addr(ApuDmcChannel *dmc) {
    dmc->dmc_current_addr = (uint16_t)(0xC000u + ((uint16_t)dmc->dmc_sample_addr_reg << 6));
}

static void apu_dmc_compute_bytes_remaining(ApuDmcChannel *dmc) {
    dmc->dmc_bytes_remaining = (uint16_t)(((uint16_t)dmc->dmc_sample_length_reg << 4) + 1u);
}

static void apu_dmc_start(ApuDmcChannel *dmc) {
    apu_dmc_compute_start_addr(dmc);
    apu_dmc_compute_bytes_remaining(dmc);
    dmc->dmc_timer_period = k_apu_dmc_rate_table[dmc->dmc_rate_index];
    /* Reset the timer counter to 0 so the first fire occurs on the very
     * next CPU cycle.  On real hardware the DMC timer runs continuously,
     * but since we don't tick it while dmc_active=false the counter is
     * stale; resetting to 0 gives the closest approximation of the
     * "timer about to fire" state that sync_dmc_fast leaves behind. */
    dmc->dmc_timer_counter = 0;
    /* Treat the first byte as pre-loaded into the shift register: consume
     * one byte from bytes_remaining now so the loop only loads SUBSEQUENT
     * bytes.  The first timer fire will output the first bit. */
    if (dmc->dmc_bytes_remaining > 0) {
        --dmc->dmc_bytes_remaining;
        dmc->dmc_bits_remaining = 8;
        dmc->dmc_active = true;
    } else {
        dmc->dmc_bits_remaining = 0;
        dmc->dmc_active = false;
    }
}

void apu_init(Apu *apu) {
    memset(apu, 0, sizeof(*apu));
    apu->pulse[0].sweep_ones_complement = true;
    apu->noise.shift_register = 1;
    apu->dmc.dmc_sample_buffer_empty = true;
    apu->dmc.dmc_bits_remaining = 0;
    apu_reset_debug_defaults(apu);
}

void apu_reset(Apu *apu) {
    /* Preserve the DC-level tracker across reset so the first post-reset
     * sample isn't subtracted against 0 (which would cause an audible pop). */
    double dc = apu->dc_level_tracker;
    memset(apu, 0, sizeof(*apu));
    apu->dc_level_tracker = dc;
    apu->pulse[0].sweep_ones_complement = true;
    apu->noise.shift_register = 1;
    apu->dmc.dmc_sample_buffer_empty = true;
    apu->dmc.dmc_bits_remaining = 0;
    apu_reset_debug_defaults(apu);
}

#if MICRONES_ENABLE_APU_EMULATION
void MICRONES_HOT_FUNC(apu_step)(Apu *apu, uint32_t cpu_cycles) {
    if (cpu_cycles == 0) return;

    /* Update cumulative CPU cycle counter first. */
    uint32_t old_cpu  = (uint32_t)apu->cpu_cycles;
    apu->cpu_cycles  += cpu_cycles;

    /* --- $4017 write delay ---
     * The frame counter reset takes effect after the delay from the write.
     * fc_reset_countdown is the absolute cpu_cycles value at which the reset
     * fires.  We check AFTER updating cpu_cycles so that the write instruction's
     * own cycles are consumed before the counter starts, ensuring the counter
     * begins at 0 relative to the first instruction AFTER the write. */
    bool fc_reset_fired = false;
    if (apu->fc_reset_pending && apu->cpu_cycles >= apu->fc_reset_countdown) {
        uint8_t v = apu->fc_reset_value;
        apu->fc_reset_pending = false;
        /* Apply mode + inhibit changes and reset the cycle counter */
        apu->frame_counter_mode_5 = (v & 0x80u) != 0;
        apu->frame_irq_inhibit    = (v & 0x40u) != 0;
        if (apu->frame_irq_inhibit) {
            apu->frame_irq_flag = false;
        }
        apu->frame_counter_cycle = 0;
        fc_reset_fired = true;
        /* The immediate half-frame clock for mode 5 was already fired at
         * write time in apu_cpu_write, so we don't clock again here. */
    }

    /* Compute APU-rate cycles (half CPU rate, fired when cpu_cycles is even).
     * apu_cycles = number of even values in (old, old+cpu_cycles]. */
    uint32_t apu_cycles = (old_cpu + cpu_cycles) / 2u - old_cpu / 2u;

    if (apu_cycles > 0) {
        /* --- Frame counter: now tracked in CPU cycles for precision ---
         * NESdev specifies the step thresholds in CPU cycles from reset.
         * Processed before the triangle timer so that any length/linear counter
         * updates take effect before the sequencer is advanced this batch.
         * Skip if a reset just fired this batch (the write instruction's own cycles
         * should not count toward the new frame period). */
        if (!apu->fc_reset_pending && !fc_reset_fired) {
            uint32_t old_fc = (uint32_t)apu->frame_counter_cycle;
            uint32_t new_fc = old_fc + cpu_cycles;
            /* Each batch is 1-8 CPU cycles; thresholds are 7000+ apart, so at
             * most one threshold crossing per call. Check from high to low so
             * the reset case is handled first. */
            if (!apu->frame_counter_mode_5) {
                /* 4-step mode.
                 * The step events fire at their thresholds; the frame counter
                 * wraps at APU_FRAME_PERIOD_4 (29830), which is 2 cycles after
                 * step 4 (29828).  Checks are ordered high-to-low to handle at
                 * most one crossing per batch (batches are ~1-8 cycles). */
                /* In 4-step mode, IRQ fires at three consecutive cycles:
                 *   APU_FRAME_IRQ_PRE (29827), APU_FRAME_STEP_4 (29828),
                 *   and APU_FRAME_PERIOD_4-1 (29829, i.e. the wrap cycle).
                 * We approximate this by setting the flag at the wrap as well. */
                if (new_fc >= APU_FRAME_PERIOD_4) {
                    /* Wrap: counter has passed end of frame.
                     * Step 4 events fire if not already done this batch. */
                    if (old_fc < APU_FRAME_STEP_4) {
                        apu_quarter_frame(apu); apu_half_frame(apu);
                        ++apu->frame_counter_steps;
                        if (!apu->frame_irq_inhibit) {
                            apu->frame_irq_flag = true;
                        }
                    } else if (old_fc < APU_FRAME_IRQ_3) {
                        /* Third IRQ fires if not already done */
                        if (!apu->frame_irq_inhibit) {
                            apu->frame_irq_flag = true;
                        }
                    }
                    new_fc -= APU_FRAME_PERIOD_4;
                } else if (old_fc < APU_FRAME_IRQ_3 && new_fc >= APU_FRAME_IRQ_3) {
                    /* Third consecutive IRQ cycle */
                    if (!apu->frame_irq_inhibit) {
                        apu->frame_irq_flag = true;
                    }
                } else if (old_fc < APU_FRAME_STEP_4 && new_fc >= APU_FRAME_STEP_4) {
                    apu_quarter_frame(apu); apu_half_frame(apu);
                    if (!apu->frame_irq_inhibit) {
                        apu->frame_irq_flag = true;
                    }
                    ++apu->frame_counter_steps;
                } else if (old_fc < APU_FRAME_IRQ_PRE && new_fc >= APU_FRAME_IRQ_PRE) {
                    if (!apu->frame_irq_inhibit) {
                        apu->frame_irq_flag = true;
                    }
                } else if (old_fc < APU_FRAME_STEP_3 && new_fc >= APU_FRAME_STEP_3) {
                    apu_quarter_frame(apu);
                } else if (old_fc < APU_FRAME_STEP_2 && new_fc >= APU_FRAME_STEP_2) {
                    apu_quarter_frame(apu); apu_half_frame(apu);
                } else if (old_fc < APU_FRAME_STEP_1 && new_fc >= APU_FRAME_STEP_1) {
                    apu_quarter_frame(apu);
                }
            } else {
                /* 5-step mode: step 5 fires and the frame wraps at APU_FRAME_PERIOD_5. */
                if (new_fc >= APU_FRAME_PERIOD_5) {
                    if (old_fc < APU_FRAME_STEP_5) {
                        apu_quarter_frame(apu); apu_half_frame(apu);
                        ++apu->frame_counter_steps;
                    }
                    new_fc -= APU_FRAME_PERIOD_5;
                } else if (old_fc < APU_FRAME_STEP_5 && new_fc >= APU_FRAME_STEP_5) {
                    apu_quarter_frame(apu); apu_half_frame(apu);
                    ++apu->frame_counter_steps;
                } else if (old_fc < APU_FRAME_STEP_3 && new_fc >= APU_FRAME_STEP_3) {
                    apu_quarter_frame(apu);
                } else if (old_fc < APU_FRAME_STEP_2 && new_fc >= APU_FRAME_STEP_2) {
                    apu_quarter_frame(apu); apu_half_frame(apu);
                } else if (old_fc < APU_FRAME_STEP_1 && new_fc >= APU_FRAME_STEP_1) {
                    apu_quarter_frame(apu);
                }
            }
            apu->frame_counter_cycle = new_fc;
        }

        /* --- Pulse timers: clocked at APU rate --- */
        {
            ApuPulseChannel *p = &apu->pulse[0];
            uint32_t fires = apu_timer_advance(&p->timer_counter, p->timer_period, apu_cycles);
            p->duty_step = (uint8_t)((p->duty_step + fires) & 0x07u);
        }
        {
            ApuPulseChannel *p = &apu->pulse[1];
            uint32_t fires = apu_timer_advance(&p->timer_counter, p->timer_period, apu_cycles);
            p->duty_step = (uint8_t)((p->duty_step + fires) & 0x07u);
        }

        /* --- Noise timer: clocked at APU rate --- */
        {
            ApuNoiseChannel *n = &apu->noise;
            uint32_t fires = apu_timer_advance(&n->timer_counter, n->timer_period, apu_cycles);
            uint8_t tap_bit = n->mode ? 6u : 1u;
            for (uint32_t i = 0; i < fires; ++i) {
                uint16_t feedback = (uint16_t)((n->shift_register & 1u) ^
                                               ((n->shift_register >> tap_bit) & 1u));
                n->shift_register = (n->shift_register >> 1) | (uint16_t)(feedback << 14u);
            }
        }

    }

    /* --- DMC timer: clocked at CPU rate ---
     * The DMC timer period is in CPU cycles; advance using cpu_cycles directly
     * (not apu_cycles), so this block is outside the apu_cycles > 0 guard. */
    {
        ApuDmcChannel *d = &apu->dmc;
        if (d->dmc_active && d->dmc_timer_period > 0) {
            uint32_t dmc_fires = apu_timer_advance(&d->dmc_timer_counter,
                                                   (uint16_t)(d->dmc_timer_period - 1u),
                                                   cpu_cycles);
            /* Each fire = one output bit clocked from the shift register.
             * dmc_bits_remaining starts at 8 (first byte pre-loaded) and
             * counts down to 0; on reaching 0 we load the next byte.
             * We don't do real memory DMA here (no bus access in apu_step),
             * so we just advance bytes_remaining/addr to track sample end. */
            for (uint32_t f = 0; f < dmc_fires; ++f) {
                /* Clock one bit: decrement the shift-register bit count */
                if (d->dmc_bits_remaining > 0) {
                    --d->dmc_bits_remaining;
                }
                /* If the shift register is now empty, load the next byte */
                if (d->dmc_bits_remaining == 0) {
                    if (d->dmc_bytes_remaining > 0) {
                        /* Load next byte from sample buffer (DMA fetch simulated) */
                        --d->dmc_bytes_remaining;
                        d->dmc_bits_remaining = 8;
                        /* current_addr advances; wrap at $FFFF back to $8000 */
                        if (d->dmc_current_addr == 0xFFFFu) {
                            d->dmc_current_addr = 0x8000u;
                        } else {
                            ++d->dmc_current_addr;
                        }
                    } else {
                        /* No more bytes: sample ended */
                        if (d->dmc_loop) {
                            apu_dmc_start(d);
                        } else {
                            d->dmc_active = false;
                            if (d->dmc_irq_enabled) {
                                d->dmc_irq_flag = true;
                            }
                        }
                        break;
                    }
                }
            }
        }
    }

    /* --- Triangle timer: clocked every CPU cycle --- */
    /* Placed after the frame counter so length/linear counter changes this
     * batch are visible before we advance the sequencer. On real hardware the
     * sequencer is also gated by timer_period < 2 only at the output stage,
     * not the timer itself, so that check is omitted here. */
    {
        ApuTriangleChannel *tri = &apu->triangle;
        uint32_t fires = apu_timer_advance(&tri->timer_counter, tri->timer_period, cpu_cycles);
        if (fires > 0 && tri->enabled && tri->length_counter > 0 &&
                tri->linear_counter > 0) {
            tri->sequence_step = (uint8_t)((tri->sequence_step + fires) & 0x1fu);
        }
    }

#if MICRONES_ENABLE_APU_PCM_OUTPUT
    /* --- Sample output: batched for the whole cpu_cycles block --- */
    apu->sample_phase += APU_OUTPUT_SAMPLE_RATE * cpu_cycles;
    while (apu->sample_phase >= APU_CPU_CLOCK_HZ) {
        apu->sample_phase -= APU_CPU_CLOCK_HZ;
        apu_pcm_push(apu, apu_mix_sample(apu));
    }
#endif
}
#endif

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
    if (apu->dmc.dmc_active) {
        status |= 0x10u;  /* bit 4: DMC active */
    }
    if (apu->frame_irq_flag) {
        status |= 0x40u;  /* bit 6: frame counter IRQ */
    }
    if (apu->dmc.dmc_irq_flag) {
        status |= 0x80u;  /* bit 7: DMC IRQ */
    }
    /* Reading $4015 clears the frame counter IRQ flag */
    apu->frame_irq_flag = false;
    apu->status = status;
    return status;
}

bool apu_has_irq(const Apu *apu) {
    return apu->frame_irq_flag || apu->dmc.dmc_irq_flag;
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
        /* Do not reset timer_counter or duty_step: the 2A03 duty sequencer is
         * free-running and the timer reloads naturally at the next expiry. */
        pulse->envelope_start = true;
        break;
    }
    case 0x4008u:
        apu->triangle.control_flag = (value & 0x80u) != 0;
        apu->triangle.linear_reload_value = value & 0x7fu;
        break;
    case 0x400au:
        apu->triangle.timer_period = (uint16_t)((apu->triangle.timer_period & 0x0700u) | value);
        /* Do not reset timer_counter: the triangle timer runs continuously. */
        break;
    case 0x400bu:
        apu->triangle.timer_period = (uint16_t)((apu->triangle.timer_period & 0x00ffu) | ((uint16_t)(value & 0x07u) << 8));
        if (apu->triangle.enabled) {
            apu->triangle.length_counter = k_apu_length_table[(value >> 3) & 0x1fu];
        }
         /* Clamp timer_counter to new period. Without this, a note change to a
         * shorter period leaves a stale large timer_counter. The first scanline
         * batch (n_cycles≈114) then causes apu_timer_advance to return a burst
         * of fires (e.g. fires=11) that skips sequence_step past positions 15/16
         * (value=0), producing an audible mid-wave click on every note change.
         * Clamping here matches real NES behavior where the timer never holds a
         * count larger than its current period. */
        if (apu->triangle.timer_counter > apu->triangle.timer_period) {
          apu->triangle.timer_counter = apu->triangle.timer_period;
        }
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
        /* Do not reset timer_counter: the noise LFSR timer runs continuously. */
        break;
    case 0x400fu:
        if (apu->noise.enabled) {
            apu->noise.length_counter = k_apu_length_table[(value >> 3) & 0x1fu];
        }
        apu->noise.timer_counter = apu->noise.timer_period;
        apu->noise.envelope_start = true;
        break;
    case 0x4010u:
        apu->dmc.dmc_irq_enabled = (value & 0x80u) != 0;
        apu->dmc.dmc_loop        = (value & 0x40u) != 0;
        apu->dmc.dmc_rate_index  = value & 0x0fu;
        apu->dmc.dmc_timer_period = k_apu_dmc_rate_table[apu->dmc.dmc_rate_index];
        if (!apu->dmc.dmc_irq_enabled) {
            apu->dmc.dmc_irq_flag = false;
        }
        break;
    case 0x4011u:
        apu->dmc.dmc_direct_load  = value & 0x7fu;
        apu->dmc.dmc_output_level = value & 0x7fu;
        break;
    case 0x4012u:
        apu->dmc.dmc_sample_addr_reg = value;
        break;
    case 0x4013u:
        apu->dmc.dmc_sample_length_reg = value;
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
            apu->triangle.linear_counter = 0;
        }
        if (!apu->noise.enabled) {
            apu->noise.length_counter = 0;
        }
        /* Writing $4015 clears the DMC IRQ flag */
        apu->dmc.dmc_irq_flag = false;
        /* DMC channel: bit 4 starts/stops */
        if (value & 0x10u) {
            /* Start or restart DMC: if already active, don't reset */
            if (!apu->dmc.dmc_active) {
                apu_dmc_start(&apu->dmc);
            }
        } else {
            apu->dmc.dmc_active = false;
        }
        apu->status = value & 0x0fu;
        break;
    case 0x4017u:
        /* The frame counter RESET (cycle counter to 0) is delayed by 3 or 4
         * CPU cycles after the write (3 if odd cycle, 4 if even cycle).
         * We store the absolute cpu_cycles target; apu_step() fires when reached.
         *
         * For mode 5 (bit 7 set), the immediate half-frame/quarter-frame
         * clock happens NOW (at write time), before the reset delay. */
        /* Delay is 3 CPU cycles if written on an odd CPU cycle, 4 if even.
         * cpu_cycles is updated by apu_step BEFORE the reset check, so at
         * write time it reflects cycles completed up to but not including
         * the current instruction batch.  Parity of cpu_cycles determines
         * even/odd write cycle. */
        {
            uint64_t delay = ((apu->cpu_cycles & 1u) == 0u) ? 3u : 2u;
            apu->fc_reset_countdown = apu->cpu_cycles + delay;
        }
        apu->fc_reset_pending   = true;
        apu->fc_reset_value     = value;
        /* If inhibit bit is set, clear the IRQ flag immediately */
        if (value & 0x40u) {
            apu->frame_irq_flag = false;
        }
        /* For mode 5 (5-step), immediately clock envelope+length counters */
        if (value & 0x80u) {
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
