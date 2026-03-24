/*
 * apu.c — InfoNES-based APU reimplementation
 *
 * Waveform synthesis uses InfoNES's DDS (phase accumulator) approach.
 * Envelope / length / sweep updates fire once per NES video frame (~60 Hz).
 * Register writes take effect immediately (no event queue).
 * Output: 48 000 Hz mono int16 PCM via the existing ring-buffer API.
 */

#include "apu_ines.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

/*
 * Phase-accumulator magic for 48 000 Hz.
 * Derived from the InfoNES 44 100 Hz value 0x289d9c00 scaled by 44100/48000.
 */
#define APU_PULSE_MAGIC    626196480u
#define APU_TRIANGLE_MAGIC 626196480u
#define APU_NOISE_MAGIC    626196480u

/* DMC cycle rate: 1 789 773 / 48 000 * 65 536 */
#define APU_CYCLE_RATE     2443637u

/* CPU cycles per NTSC NES frame: 1 789 773 / 60.0988 */
#define APU_CYCLES_PER_FRAME 29781u

/* ------------------------------------------------------------------ */
/* Wave tables (verbatim from InfoNES_pAPU.cpp)                        */
/* ------------------------------------------------------------------ */

static const uint8_t pulse_25[32] = {
    0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
static const uint8_t pulse_50[32] = {
    0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,
    0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
static const uint8_t pulse_75[32] = {
    0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,
    0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,
    0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
static const uint8_t pulse_87[32] = {
    0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,
    0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,
    0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,
    0x11,0x11,0x11,0x11,0x00,0x00,0x00,0x00,
};
static const uint8_t triangle_50[32] = {
    0x00,0x10,0x20,0x30,0x40,0x50,0x60,0x70,
    0x80,0x90,0xa0,0xb0,0xc0,0xd0,0xe0,0xf0,
    0xff,0xef,0xdf,0xcf,0xbf,0xaf,0x9f,0x8f,
    0x7f,0x6f,0x5f,0x4f,0x3f,0x2f,0x1f,0x0f,
};
static const uint8_t *const pulse_waves[4] = {
    pulse_87, pulse_75, pulse_50, pulse_25,
};

/* ------------------------------------------------------------------ */
/* Lookup tables                                                       */
/* ------------------------------------------------------------------ */

/* NES length counter values converted to 60 Hz vsync frames. */
static const uint8_t k_atl[32] = {
     5, 127, 10,  1, 19,  2, 40,  3, 80,  4, 30,  5,  7,  6, 13,  7,
     6,   8, 12,  9, 24, 10, 48, 11, 96, 12, 36, 13,  8, 14, 16, 15,
};

/* Sweep muting: max period before a positive sweep is silenced. */
static const uint16_t k_freq_limit[8] = {
    0x3FF, 0x555, 0x666, 0x71C, 0x787, 0x7C1, 0x7E0, 0x7F0,
};

/* Noise LFSR clock period in CPU cycles (indexed by $400E[3:0]). */
static const uint32_t k_noise_freq[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068,
};

/* DMC byte-fetch period in CPU cycles (indexed by $4010[3:0]). */
static const uint32_t k_dpcm_cycles[16] = {
    428,380,340,320,286,254,226,214,190,160,142,128,106,85,72,54,
};

/* ------------------------------------------------------------------ */
/* Skip (phase increment) helpers                                      */
/* ------------------------------------------------------------------ */

static void update_pulse_skip(ApuPulseChannel *p)
{
    /* skip = PulseMagic / (freq/2).  freq==0 or freq==1 → silence. */
    if (p->timer_period > 1) {
        p->skip = APU_PULSE_MAGIC / (p->timer_period / 2);
    } else {
        p->skip = 0;
    }
}

static void update_triangle_skip(ApuTriangleChannel *t)
{
    uint32_t freq = (((uint32_t)(t->rd & 0x07)) << 8) | t->rc;
    t->timer_period = (uint16_t)freq;
    if (freq) {
        t->skip = APU_TRIANGLE_MAGIC / freq;
    } else {
        t->skip = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Per-frame update (InfoNES pAPUVsync equivalent, ~60 Hz)            */
/* ------------------------------------------------------------------ */

static void apu_vsync(Apu *apu)
{
    /* --- Pulse 1 --- */
    {
        ApuPulseChannel *p = &apu->pulse[0];

        /* Length counter (InfoNES decrements unconditionally for pulse) */
        if (p->length_counter) { p->length_counter--; }

        /* Envelope decay */
        uint8_t env_delay = p->ra & 0x0f;
        if (env_delay) {
            p->env_phase -= 4;
            while (p->env_phase < 0) {
                p->env_phase += env_delay;
                if ((p->ra >> 5) & 1) {                 /* hold/loop */
                    p->envelope_decay = (p->envelope_decay - 1) & 0x0f;
                } else if (p->envelope_decay > 0) {
                    p->envelope_decay--;
                }
            }
        }

        /* Frequency sweep */
        uint8_t sweep_on     = (p->rb >> 7) & 1;
        uint8_t sweep_shifts =  p->rb & 0x07;
        if (sweep_on && sweep_shifts) {
            p->sweep_phase -= 2;
            while (p->sweep_phase < 0) {
                uint8_t sweep_delay = (p->rb >> 4) & 0x07;
                if (!sweep_delay) break;
                p->sweep_phase += sweep_delay;
                if ((p->rb >> 3) & 1) {             /* negate (ramp up pitch) */
                    if (p->ones_complement) {
                        p->timer_period += ~(p->timer_period >> sweep_shifts);
                    } else {
                        p->timer_period -= (p->timer_period >> sweep_shifts);
                    }
                } else {                             /* positive (ramp down pitch) */
                    p->timer_period += (p->timer_period >> sweep_shifts);
                }
                update_pulse_skip(p);
            }
        }
    }

    /* --- Pulse 2 --- */
    {
        ApuPulseChannel *p = &apu->pulse[1];

        if (p->length_counter) { p->length_counter--; }

        uint8_t env_delay = p->ra & 0x0f;
        if (env_delay) {
            p->env_phase -= 4;
            while (p->env_phase < 0) {
                p->env_phase += env_delay;
                if ((p->ra >> 5) & 1) {
                    p->envelope_decay = (p->envelope_decay - 1) & 0x0f;
                } else if (p->envelope_decay > 0) {
                    p->envelope_decay--;
                }
            }
        }

        uint8_t sweep_on     = (p->rb >> 7) & 1;
        uint8_t sweep_shifts =  p->rb & 0x07;
        if (sweep_on && sweep_shifts) {
            p->sweep_phase -= 2;
            while (p->sweep_phase < 0) {
                uint8_t sweep_delay = (p->rb >> 4) & 0x07;
                if (!sweep_delay) break;
                p->sweep_phase += sweep_delay;
                if ((p->rb >> 3) & 1) {
                    p->timer_period -= (p->timer_period >> sweep_shifts);   /* twos complement */
                } else {
                    p->timer_period += (p->timer_period >> sweep_shifts);
                }
                update_pulse_skip(p);
            }
        }
    }

    /* --- Triangle --- */
    {
        ApuTriangleChannel *t = &apu->triangle;
        uint8_t holdnote = (t->ra >> 7) & 1;

        if (t->reload_flag) {
            /* llc in scaled units: (reload_val) * 64 → decrement 256/vsync → 240 Hz effective */
            t->llc = (uint32_t)(t->ra & 0x7f) * 64u;
        } else if (t->llc > 0) {
            if (t->llc >= 256u) { t->llc -= 256u; } else { t->llc = 0; }
        }

        if (!holdnote) { t->reload_flag = false; }

        if (t->length_counter > 0 && !holdnote) { t->length_counter--; }
    }

    /* --- Noise --- */
    {
        ApuNoiseChannel *n = &apu->noise;

        if (n->length_counter > 0 && !((n->ra >> 5) & 1)) { n->length_counter--; }

        uint8_t env_delay = n->ra & 0x0f;
        if (env_delay) {
            n->env_phase -= 4;
            while (n->env_phase < 0) {
                n->env_phase += env_delay;
                if ((n->ra >> 5) & 1) {
                    n->envelope_decay = (n->envelope_decay - 1) & 0x0f;
                } else if (n->envelope_decay > 0) {
                    n->envelope_decay--;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Per-sample render (InfoNES DDS)                                     */
/* ------------------------------------------------------------------ */

static int16_t MICRONES_HOT_FUNC(apu_render_sample)(Apu *apu)
{
    uint8_t ctrl = apu->ctrl;
    uint8_t mask = apu->mix_enable_mask;

    /* --- Pulse 1 --- */
    int p1_nes = 0;
    if (mask & APU_DEBUG_MASK_PULSE1) {
        ApuPulseChannel *p = &apu->pulse[0];
        uint8_t hold      = (p->ra >> 5) & 1;
        uint8_t inc_dec   = (p->rb >> 3) & 1;
        uint8_t shifts    = p->rb & 7;
        int     gate      = (ctrl & 0x01) &&
                            (p->length_counter > 0 || hold) &&
                            (p->timer_period >= 8) &&
                            !(!inc_dec && shifts && p->timer_period > k_freq_limit[shifts]);
        if (gate) {
            p->index += p->skip;
            p->index &= 0x1fffffffu;
            p->duty_step = (uint8_t)(p->index >> 24);
            uint8_t vol = ((p->ra >> 4) & 1) ? (p->ra & 0x0f) : p->envelope_decay;
            p1_nes = pulse_waves[p->wave_idx][p->duty_step] * vol / 17;
        }
    }

    /* --- Pulse 2 --- */
    int p2_nes = 0;
    if (mask & APU_DEBUG_MASK_PULSE2) {
        ApuPulseChannel *p = &apu->pulse[1];
        uint8_t hold    = (p->ra >> 5) & 1;
        uint8_t inc_dec = (p->rb >> 3) & 1;
        uint8_t shifts  = p->rb & 7;
        int     gate    = (ctrl & 0x02) &&
                          (p->length_counter > 0 || hold) &&
                          (p->timer_period >= 8) &&
                          !(!inc_dec && shifts && p->timer_period > k_freq_limit[shifts]);
        if (gate) {
            p->index += p->skip;
            p->index &= 0x1fffffffu;
            p->duty_step = (uint8_t)(p->index >> 24);
            uint8_t vol = ((p->ra >> 4) & 1) ? (p->ra & 0x0f) : p->envelope_decay;
            p2_nes = pulse_waves[p->wave_idx][p->duty_step] * vol / 17;
        }
    }

    /* --- Triangle --- */
    int tri_nes = 0;
    if (mask & APU_DEBUG_MASK_TRIANGLE) {
        ApuTriangleChannel *t = &apu->triangle;
        int gate = (ctrl & 0x04) &&
                   (t->length_counter > 0) &&
                   (t->llc > 0) &&
                   (t->timer_period >= 8);
        if (gate) {
            t->index += t->skip;
            t->index &= 0x1fffffffu;
            t->sequence_step = (uint8_t)(t->index >> 24);
            tri_nes = triangle_50[t->sequence_step] / 17;
        }
        t->linear_counter = (uint8_t)(t->llc > (127u * 64u) ? 127u : t->llc / 64u);
    }

    /* --- Noise --- */
    int noise_nes = 0;
    if (mask & APU_DEBUG_MASK_NOISE) {
        ApuNoiseChannel *n = &apu->noise;
        if ((ctrl & 0x08) && n->length_counter > 0) {
            n->index += n->skip;
            if (n->index > 0x00ffffffu) {
                int shift = ((n->rc >> 7) & 1) ? 6 : 1;
                uint32_t f = (n->sr ^ (n->sr >> shift)) & 1u;
                n->sr = (n->sr >> 1) | (f << 14);
                n->index &= 0x00ffffffu;
                n->shift_register = (uint16_t)(n->sr & 0x7fffu);
            }
            if (!(n->sr & 1u)) {
                noise_nes = ((n->ra >> 4) & 1) ? (n->ra & 0x0f) : n->envelope_decay;
            }
        }
    }

    /* --- NES nonlinear mix --- */
    float pulse_out = 0.0f, tnd_out = 0.0f;
    float ps = (float)(p1_nes + p2_nes);
    if (ps > 0.0f) {
        pulse_out = 95.88f / (8128.0f / ps + 100.0f);
    }
    float tnd = (float)tri_nes / 8227.0f + (float)noise_nes / 12241.0f;
    if (tnd > 0.0f) {
        tnd_out = 159.79f / (1.0f / tnd + 100.0f);
    }
    float mixed = pulse_out + tnd_out;

    /* DC removal */
    apu->dc_level_tracker += ((double)mixed - apu->dc_level_tracker) * 0.0001;
    mixed -= (float)apu->dc_level_tracker;

    int sample = (int)(mixed * 32767.0f);
    if (sample < -32768) { sample = -32768; ++apu->clip_count; }
    else if (sample > 32767) { sample = 32767; ++apu->clip_count; }
    return (int16_t)sample;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

static void apu_common_init(Apu *apu)
{
    memset(apu, 0, sizeof(*apu));
    apu->pulse[0].ones_complement = true;
    apu->pulse[0].wave_idx = 2;   /* pulse_50 */
    apu->pulse[1].wave_idx = 2;
    apu->noise.sr = 1u;
    apu->mix_enable_mask = APU_DEBUG_MASK_ALL;
}

void apu_init(Apu *apu)  { apu_common_init(apu); }
void apu_reset(Apu *apu) { apu_common_init(apu); }

#if MICRONES_ENABLE_APU_EMULATION
void MICRONES_HOT_FUNC(apu_step)(Apu *apu, uint32_t cpu_cycles)
{
    if (!cpu_cycles) return;

    apu->cpu_cycles        += cpu_cycles;
    apu->cycles_since_vsync += cpu_cycles;

    /* Vsync: fire once per NES frame (~29 781 CPU cycles). */
    while (apu->cycles_since_vsync >= APU_CYCLES_PER_FRAME) {
        apu_vsync(apu);
        apu->cycles_since_vsync -= APU_CYCLES_PER_FRAME;
        ++apu->frame_counter_steps;
    }
    /* Keep compat field in sync (nes.c hashes this). */
    apu->frame_counter_cycle = apu->cycles_since_vsync;

    /* Emit output samples at 48 000 Hz. */
    apu->sample_phase += APU_OUTPUT_SAMPLE_RATE * cpu_cycles;
    while (apu->sample_phase >= APU_CPU_CLOCK_HZ) {
        apu->sample_phase -= APU_CPU_CLOCK_HZ;
        int16_t s = apu_render_sample(apu);
        if (apu->pcm_count < APU_PCM_CAPACITY) {
            apu->pcm[apu->pcm_write_index] = s;
            apu->pcm_write_index = (apu->pcm_write_index + 1u) % APU_PCM_CAPACITY;
            ++apu->pcm_count;
            ++apu->sample_count;
        } else {
            ++apu->dropped_samples;
        }
    }
}
#endif /* MICRONES_ENABLE_APU_EMULATION */

uint8_t apu_cpu_read(Apu *apu, uint16_t addr)
{
    if (addr != 0x4015u) return 0;
    uint8_t s = 0;
    if (apu->pulse[0].length_counter > 0) s |= 0x01u;
    if (apu->pulse[1].length_counter > 0) s |= 0x02u;
    if (apu->triangle.length_counter > 0) s |= 0x04u;
    if (apu->noise.length_counter    > 0) s |= 0x08u;
    if (apu->dmc.dma_length          > 0) s |= 0x10u;
    apu->status = s;
    return s;
}

void apu_cpu_write(Apu *apu, uint16_t addr, uint8_t value)
{
    if (addr >= 0x4000u && addr <= 0x4017u) {
        apu->registers[addr - 0x4000u] = value;
        ++apu->register_write_count[addr - 0x4000u];
    }

    switch (addr) {

    /* --- Pulse 1 --- */
    case 0x4000u:
        apu->pulse[0].ra = value;
        apu->pulse[0].wave_idx = (value >> 6) & 3u;
        break;
    case 0x4001u:
        apu->pulse[0].rb = value;
        break;
    case 0x4002u:
        apu->pulse[0].rc = value;
        apu->pulse[0].timer_period = (uint16_t)(((apu->pulse[0].rd & 0x07u) << 8) | value);
        update_pulse_skip(&apu->pulse[0]);
        break;
    case 0x4003u:
        apu->pulse[0].rd = value;
        apu->pulse[0].timer_period = (uint16_t)(((uint32_t)(value & 0x07u) << 8) | apu->pulse[0].rc);
        if (apu->pulse[0].enabled) {
            apu->pulse[0].length_counter = k_atl[(value >> 3) & 0x1fu];
        }
        apu->pulse[0].envelope_decay = 15;
        update_pulse_skip(&apu->pulse[0]);
        break;

    /* --- Pulse 2 --- */
    case 0x4004u:
        apu->pulse[1].ra = value;
        apu->pulse[1].wave_idx = (value >> 6) & 3u;
        break;
    case 0x4005u:
        apu->pulse[1].rb = value;
        break;
    case 0x4006u:
        apu->pulse[1].rc = value;
        apu->pulse[1].timer_period = (uint16_t)(((apu->pulse[1].rd & 0x07u) << 8) | value);
        update_pulse_skip(&apu->pulse[1]);
        break;
    case 0x4007u:
        apu->pulse[1].rd = value;
        apu->pulse[1].timer_period = (uint16_t)(((uint32_t)(value & 0x07u) << 8) | apu->pulse[1].rc);
        if (apu->pulse[1].enabled) {
            apu->pulse[1].length_counter = k_atl[(value >> 3) & 0x1fu];
        }
        apu->pulse[1].envelope_decay = 15;
        update_pulse_skip(&apu->pulse[1]);
        break;

    /* --- Triangle --- */
    case 0x4008u:
        apu->triangle.ra = value;
        break;
    case 0x400au:
        apu->triangle.rc = value;
        update_triangle_skip(&apu->triangle);
        break;
    case 0x400bu:
        apu->triangle.rd = value;
        update_triangle_skip(&apu->triangle);
        if (apu->triangle.enabled) {
            apu->triangle.length_counter = k_atl[(value >> 3) & 0x1fu];
        }
        apu->triangle.reload_flag = true;
        break;

    /* --- Noise --- */
    case 0x400cu:
        apu->noise.ra = value;
        break;
    case 0x400eu:
        apu->noise.rc = value;
        {
            uint32_t nf = k_noise_freq[value & 0x0fu];
            apu->noise.timer_period = (uint16_t)nf;
            apu->noise.skip = nf ? (APU_NOISE_MAGIC / nf) : 0u;
        }
        break;
    case 0x400fu:
        apu->noise.rd = value;
        if (apu->noise.enabled) {
            apu->noise.length_counter = k_atl[(value >> 3) & 0x1fu];
        }
        apu->noise.envelope_decay = 15;
        break;

    /* --- DMC --- */
    case 0x4010u:
        apu->dmc.reg[0] = value;
        apu->dmc.freq    = (int)(k_dpcm_cycles[value & 0x0fu] << 16);
        apu->dmc.looping = value & 0x40u;
        break;
    case 0x4011u:
        apu->dmc.reg[1]       = value;
        apu->dmc.dpcm_value   = (value & 0x7fu) >> 1;
        break;
    case 0x4012u:
        apu->dmc.reg[2]       = value;
        apu->dmc.cache_addr   = (uint16_t)(0xC000u + ((uint16_t)value << 6));
        break;
    case 0x4013u:
        apu->dmc.reg[3]            = value;
        apu->dmc.cache_dma_length  = (int)(((value << 4) + 1) << 3);
        break;

    /* --- Channel enable --- */
    case 0x4015u:
        apu->ctrl = value;
        apu->pulse[0].enabled = (value & 0x01u) != 0;
        if (!apu->pulse[0].enabled) apu->pulse[0].length_counter = 0;

        apu->pulse[1].enabled = (value & 0x02u) != 0;
        if (!apu->pulse[1].enabled) apu->pulse[1].length_counter = 0;

        apu->triangle.enabled = (value & 0x04u) != 0;
        if (!apu->triangle.enabled) {
            apu->triangle.length_counter = 0;
            apu->triangle.llc            = 0;
        }

        apu->noise.enabled = (value & 0x08u) != 0;
        if (!apu->noise.enabled) apu->noise.length_counter = 0;

        apu->dmc.enable = (value & 0x10u) != 0;
        if (!apu->dmc.enable) {
            apu->dmc.dma_length = 0;
        } else if (!apu->dmc.dma_length) {
            apu->dmc.address    = apu->dmc.cache_addr;
            apu->dmc.dma_length = apu->dmc.cache_dma_length;
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Audio output                                                        */
/* ------------------------------------------------------------------ */

uint32_t apu_output_sample_rate(const Apu *apu)
{
    (void)apu;
    return APU_OUTPUT_SAMPLE_RATE;
}

size_t apu_audio_available_samples(const Apu *apu)
{
    return (size_t)apu->pcm_count;
}

size_t apu_audio_read_samples(Apu *apu, int16_t *dst, size_t max_samples)
{
    size_t n = (size_t)apu->pcm_count;
    if (n > max_samples) n = max_samples;
    for (size_t i = 0; i < n; ++i) {
        dst[i] = apu->pcm[apu->pcm_read_index];
        apu->pcm_read_index = (apu->pcm_read_index + 1u) % APU_PCM_CAPACITY;
    }
    apu->pcm_count -= (uint32_t)n;
    return n;
}

/* ------------------------------------------------------------------ */
/* Debug API                                                           */
/* ------------------------------------------------------------------ */

void apu_debug_set_mix_enable_mask(Apu *apu, uint8_t mask)
{
    apu->mix_enable_mask = mask;
}

uint8_t apu_debug_mix_enable_mask(const Apu *apu)
{
    return apu->mix_enable_mask;
}

void apu_debug_set_test_tone(Apu *apu, ApuDebugTestTone mode)
{
    apu->test_tone_mode = (uint8_t)mode;
}

ApuDebugTestTone apu_debug_test_tone(const Apu *apu)
{
    return (ApuDebugTestTone)apu->test_tone_mode;
}

void apu_debug_reset_metrics(Apu *apu)
{
    memset(apu->channel_stats, 0, sizeof(apu->channel_stats));
    apu->dropped_samples = 0;
    apu->clip_count      = 0;
    apu->sample_count    = 0;
}

void apu_debug_get_report(const Apu *apu, ApuDebugReport *report)
{
    memset(report, 0, sizeof(*report));
    report->mix_enable_mask = apu->mix_enable_mask;
    report->test_tone       = (ApuDebugTestTone)apu->test_tone_mode;
    report->dropped_samples = apu->dropped_samples;
    report->clip_count      = apu->clip_count;
    memcpy(report->channel_stats, apu->channel_stats, sizeof(apu->channel_stats));

    for (int i = 0; i < APU_DEBUG_REGISTER_COUNT; ++i) {
        report->register_summary[i].write_count = apu->register_write_count[i];
        report->register_summary[i].last_value  = apu->registers[i];
    }

    /* Pulse snapshots */
    for (int i = 0; i < 2; ++i) {
        const ApuPulseChannel *p = &apu->pulse[i];
        ApuDebugPulseState    *d = &report->pulse[i];
        d->enabled        = p->enabled;
        d->length_halt    = (p->ra >> 5) & 1;
        d->constant_volume= (p->ra >> 4) & 1;
        d->sweep_enabled  = (p->rb >> 7) & 1;
        d->duty           = (p->ra >> 6) & 3;
        d->duty_step      = (uint8_t)(p->index >> 24);
        d->volume_period  = p->ra & 0x0f;
        d->envelope_decay = p->envelope_decay;
        d->length_counter = p->length_counter;
        d->timer_period   = (uint16_t)p->timer_period;
        d->timer_counter  = 0;   /* not tracked in DDS model */
    }

    /* Triangle snapshot */
    {
        const ApuTriangleChannel *t = &apu->triangle;
        ApuDebugTriangleState    *d = &report->triangle;
        d->enabled            = t->enabled;
        d->control_flag       = (t->ra >> 7) & 1;
        d->linear_reload_flag = t->reload_flag;
        d->sequence_step      = (uint8_t)(t->index >> 24);
        d->linear_reload_value= t->ra & 0x7f;
        d->linear_counter     = (uint8_t)(t->llc >> 6);  /* convert back to 0–127 */
        d->length_counter     = t->length_counter;
        d->timer_period       = (uint16_t)((((uint32_t)(t->rd & 7)) << 8) | t->rc);
        d->timer_counter      = 0;
    }

    /* Noise snapshot */
    {
        const ApuNoiseChannel *n = &apu->noise;
        ApuDebugNoiseState    *d = &report->noise;
        d->enabled        = n->enabled;
        d->length_halt    = (n->ra >> 5) & 1;
        d->constant_volume= (n->ra >> 4) & 1;
        d->mode           = (n->rc >> 7) & 1;
        d->volume_period  = n->ra & 0x0f;
        d->envelope_decay = n->envelope_decay;
        d->length_counter = n->length_counter;
        d->period_index   = n->rc & 0x0f;
        d->timer_period   = (uint16_t)k_noise_freq[n->rc & 0x0f];
        d->timer_counter  = 0;
        d->shift_register = (uint16_t)(n->sr & 0x7fffu);
    }

    report->status           = apu->status;
    report->frame_counter_mode_5 = false;
    report->frame_irq_inhibit    = false;
}

const char *apu_debug_channel_name(ApuDebugChannel channel)
{
    static const char *names[APU_DEBUG_CHANNEL_COUNT] = {
        "pulse1", "pulse2", "triangle", "noise", "dmc", "mix",
    };
    if ((unsigned)channel < APU_DEBUG_CHANNEL_COUNT) return names[channel];
    return "?";
}

const char *apu_debug_test_tone_name(ApuDebugTestTone mode)
{
    switch (mode) {
    case APU_DEBUG_TEST_TONE_NONE:     return "none";
    case APU_DEBUG_TEST_TONE_PULSE1:   return "pulse1";
    case APU_DEBUG_TEST_TONE_TRIANGLE: return "triangle";
    default:                           return "?";
    }
}
