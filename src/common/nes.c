#include "nes.h"

#include "nrom.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void nes_set_error(Nes *nes, const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vsnprintf(nes->last_error, sizeof(nes->last_error), fmt, args);
    va_end(args);
}

static void nes_clear_runtime_state(Nes *nes) {
    memset(&nes->stats, 0, sizeof(nes->stats));
    memset(&nes->stop_info, 0, sizeof(nes->stop_info));
    memset(&nes->step_profile, 0, sizeof(nes->step_profile));
    memset(&nes->ppu.step_profile, 0, sizeof(nes->ppu.step_profile));
#if MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
    memset(nes->trace, 0, sizeof(nes->trace));
    nes->trace_head = 0;
    nes->trace_count = 0;
#else
    nes->trace_head = 0;
    nes->trace_count = 0;
#endif
}

static bool nes_has_cartridge(const Nes *nes) {
    return cart_is_loaded(&nes->cartridge);
}

void nes_init(Nes *nes) {
    memset(nes, 0, sizeof(*nes));
    cpu6502_init(&nes->cpu);
    ppu_init(&nes->ppu);
    apu_init(&nes->apu);
    input_controller_init(&nes->controllers[0]);
    input_controller_init(&nes->controllers[1]);
    nes_clear_runtime_state(nes);
    nes_set_error(nes, "");
}

void nes_destroy(Nes *nes) {
    cart_unload(&nes->cartridge);
}

bool nes_load_cartridge_file(Nes *nes, const char *path) {
    char error[160];

    if (!cart_load_ines_file(&nes->cartridge, path, error, sizeof(error))) {
        nes_set_error(nes, "%s", error);
        return false;
    }

    nes_sync_prg_cache(nes);
    nes_clear_runtime_state(nes);
    nes_set_error(nes, "");
    return true;
}

bool nes_load_cartridge_memory(Nes *nes, const uint8_t *rom_image, size_t rom_image_size) {
    char error[160];

    if (!cart_load_ines_memory(&nes->cartridge, rom_image, rom_image_size, error, sizeof(error))) {
        nes_set_error(nes, "%s", error);
        return false;
    }

    nes_sync_prg_cache(nes);
    nes_clear_runtime_state(nes);
    nes_set_error(nes, "");
    return true;
}

bool nes_load_cartridge_const_memory(Nes *nes, const uint8_t *rom_image, size_t rom_image_size) {
    char error[160];

    if (!cart_load_ines_const_memory(&nes->cartridge, rom_image, rom_image_size, error, sizeof(error))) {
        nes_set_error(nes, "%s", error);
        return false;
    }

    nes_sync_prg_cache(nes);
    nes_clear_runtime_state(nes);
    nes_set_error(nes, "");
    return true;
}

void nes_reset(Nes *nes) {
    memset(nes->cpu_ram, 0, sizeof(nes->cpu_ram));
    memset(nes->wram, 0, sizeof(nes->wram));
    ppu_reset(&nes->ppu);
    apu_reset(&nes->apu);
    input_controller_reset(&nes->controllers[0]);
    input_controller_reset(&nes->controllers[1]);
    nes_sync_prg_cache(nes);
    nes_clear_runtime_state(nes);
    cpu6502_reset(&nes->cpu, nes);
    nes_set_error(nes, "");
}

void nes_set_profile_clock(Nes *nes, micrones_profile_now_us_fn now_us, void *user) {
    nes->profile_now_us = now_us;
    nes->profile_now_user = user;
    nes->ppu.profile_now_us = now_us;
    nes->ppu.profile_now_user = user;
}

void nes_set_controller_state(Nes *nes, unsigned controller_index, NesControllerState state) {
    if (controller_index < 2) {
        input_controller_set_state(&nes->controllers[controller_index], state);
    }
}

void nes_set_render_target(Nes *nes, NesFrameBuffer *fb) {
    ppu_set_render_target(&nes->ppu, fb);
}

void nes_set_sprite0_diag_window(Nes *nes, uint64_t frame_start, uint64_t frame_end) {
    ppu_set_sprite0_diag_window(&nes->ppu, frame_start, frame_end);
}

bool MICRONES_HOT_FUNC(nes_step_instruction)(Nes *nes) {
    bool ok = cpu6502_step(&nes->cpu, nes);
    if (nes->pending_apu_cycles > 0) {
        apu_step(&nes->apu, nes->pending_apu_cycles);
        nes->pending_apu_cycles = 0;
    }
    return ok;
}

bool MICRONES_HOT_FUNC(nes_step_scanline)(Nes *nes) {
    uint64_t scanline_started_us = nes_profile_now_us(nes);
    uint32_t scanline_started_cycles = micrones_profile_now_cycles();

    /* Delegate the tight CPU dispatch loop to cpu6502_run_scanline, which
     * lives in the same TU as cpu6502_step.  The __attribute__((flatten))
     * there lets the compiler inline cpu6502_step into the loop body,
     * eliminating the CALL8+ENTRY+RETW overhead (~8-11 cycles) on every one
     * of the ~27,360 CPU instructions executed per frame. */
    if (!cpu6502_run_scanline(&nes->cpu, nes)) {
        return false;
    }

    /* Flush APU cycles accumulated across all instructions in this scanline.
     * Batching here (240×/frame) instead of per-instruction (~9828×/frame)
     * saves ~1.8ms/frame of apu_step call overhead. */
    if (nes->pending_apu_cycles > 0) {
        uint64_t apu_started_us = nes_profile_now_us(nes);
        uint32_t apu_started_cycles = micrones_profile_now_cycles();
        bool counted_apu_step = false;
        apu_step(&nes->apu, nes->pending_apu_cycles);
        if (apu_started_us != 0) {
            nes->step_profile.apu_step_us_total += nes_profile_now_us(nes) - apu_started_us;
            ++nes->step_profile.apu_step_count;
            counted_apu_step = true;
        }
        if (apu_started_cycles != 0) {
            nes->step_profile.apu_step_cycles_total +=
                (uint32_t)(micrones_profile_now_cycles() - apu_started_cycles);
            if (!counted_apu_step) {
                ++nes->step_profile.apu_step_count;
            }
        }
        nes->pending_apu_cycles = 0;
    }

    if (scanline_started_us != 0) {
        nes->step_profile.scanline_step_us_total += nes_profile_now_us(nes) - scanline_started_us;
        ++nes->step_profile.scanline_count;
    }
    if (scanline_started_cycles != 0) {
        nes->step_profile.scanline_step_cycles_total +=
            (uint32_t)(micrones_profile_now_cycles() - scanline_started_cycles);
        if (scanline_started_us == 0) {
            ++nes->step_profile.scanline_count;
        }
    }

    return true;
}

bool nes_step_frame(Nes *nes) {
    uint64_t target = nes->ppu.frame_count + 1;

    nes->ppu.frame_ready = false;
    while (nes->ppu.frame_count < target) {
        /* Use scanline-granularity stepping so APU is flushed 240×/frame
         * (once per scanline) rather than ~9828×/frame (once per instruction).
         * This saves ~1.8 ms/frame of apu_step call overhead. */
        if (!nes_step_scanline(nes)) {
            return false;
        }
    }
    return true;
}

uint32_t nes_audio_sample_rate(const Nes *nes) {
    return apu_output_sample_rate(&nes->apu);
}

size_t nes_audio_available_samples(const Nes *nes) {
    return apu_audio_available_samples(&nes->apu);
}

size_t nes_audio_read_samples(Nes *nes, int16_t *dst, size_t max_samples) {
    return apu_audio_read_samples(&nes->apu, dst, max_samples);
}

void nes_audio_set_mix_enable_mask(Nes *nes, uint8_t mask) {
    apu_debug_set_mix_enable_mask(&nes->apu, mask);
}

uint8_t nes_audio_mix_enable_mask(const Nes *nes) {
    return apu_debug_mix_enable_mask(&nes->apu);
}

void nes_audio_set_test_tone(Nes *nes, ApuDebugTestTone mode) {
    apu_debug_set_test_tone(&nes->apu, mode);
}

ApuDebugTestTone nes_audio_test_tone(const Nes *nes) {
    return apu_debug_test_tone(&nes->apu);
}

void nes_audio_debug_reset_metrics(Nes *nes) {
    apu_debug_reset_metrics(&nes->apu);
}

void nes_audio_debug_get_report(const Nes *nes, ApuDebugReport *report) {
    apu_debug_get_report(&nes->apu, report);
}

const Cpu6502 *nes_cpu_state(const Nes *nes) {
    return &nes->cpu;
}

uint64_t nes_frame_count(const Nes *nes) {
    return nes->ppu.frame_count;
}

int nes_scanline(const Nes *nes) {
    return nes->ppu.scanline;
}

const NesFrameBuffer *nes_framebuffer(const Nes *nes) {
    return ppu_framebuffer(&nes->ppu);
}

const NesScanline *nes_scanline_buffer(const Nes *nes) {
    return ppu_scanline(&nes->ppu);
}

const char *nes_last_error(const Nes *nes) {
    return nes->last_error;
}

const NesExecutionStats *nes_execution_stats(const Nes *nes) {
    return &nes->stats;
}

const NesStopInfo *nes_stop_info(const Nes *nes) {
    return &nes->stop_info;
}

size_t nes_trace_copy(const Nes *nes, Cpu6502TraceEntry *out_entries, size_t max_entries) {
#if !MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
    (void)nes;
    (void)out_entries;
    (void)max_entries;
    return 0;
#else
    size_t count = nes->trace_count;
    size_t start;

    if (count > max_entries) {
        count = max_entries;
    }

    start = (nes->trace_head + NES_TRACE_CAPACITY - count) % NES_TRACE_CAPACITY;
    for (size_t i = 0; i < count; ++i) {
        out_entries[i] = nes->trace[(start + i) % NES_TRACE_CAPACITY];
    }
    return count;
#endif
}

uint64_t nes_state_hash(const Nes *nes) {
#if !MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
    (void)nes;
    return 0;
#else
    uint64_t hash = 1469598103934665603ull;

#define HASH_BYTE(v) do { hash ^= (uint8_t)(v); hash *= 1099511628211ull; } while (0)
#define HASH_U64(v) \
    do { \
        uint64_t _value = (uint64_t)(v); \
        for (unsigned _i = 0; _i < 8; ++_i) { \
            HASH_BYTE((_value >> (_i * 8)) & 0xffu); \
        } \
    } while (0)

    HASH_U64(nes->stats.instruction_count);
    HASH_U64(nes->cpu.cycles);
    HASH_BYTE(nes->cpu.a);
    HASH_BYTE(nes->cpu.x);
    HASH_BYTE(nes->cpu.y);
    HASH_BYTE(nes->cpu.sp);
    HASH_BYTE(nes->cpu.p);
    HASH_BYTE(nes->cpu.last_opcode);
    HASH_BYTE(nes->cpu.pc & 0xffu);
    HASH_BYTE(nes->cpu.pc >> 8);
    HASH_U64(nes->ppu.frame_count);
    HASH_U64(nes->ppu.completed_frame_count);
    HASH_U64(nes->ppu.completed_frame_ready ? 1u : 0u);
    HASH_U64(nes->ppu.last_completed_frame_hash);
    HASH_U64(nes->ppu.last_completed_nonzero_pixels);
    HASH_U64(nes->ppu.first_nonblank_frame_index);
    HASH_U64(nes->ppu.first_nonblank_frame_hash);
    HASH_U64((uint64_t)(uint32_t)nes->ppu.scanline);
    HASH_U64(nes->ppu.vram_addr);
    HASH_U64(nes->ppu.temp_addr);
    HASH_U64(nes->ppu.fine_x);
    HASH_U64(nes->ppu.write_toggle ? 1u : 0u);
    HASH_U64(nes->ppu.render_vram_addr);
    HASH_U64(nes->ppu.render_fine_x);
    HASH_U64(nes->ppu.render_scroll_x);
    HASH_U64(nes->ppu.render_scroll_y);
    HASH_U64(nes->ppu.render_base_nametable);
    HASH_U64(nes->stats.nmi_count);
    HASH_U64(nes->apu.cpu_cycles);
    HASH_U64(nes->apu.sample_count);
    HASH_U64(nes->apu.dropped_samples);
    HASH_U64(nes->apu.frame_counter_cycle);
    HASH_U64(nes->apu.frame_counter_steps);
    HASH_U64(nes->apu.sample_phase);
    HASH_U64(nes->apu.pcm_count);
    HASH_U64((uint64_t)(uint32_t)nes->apu.pulse[0].length_counter);
    HASH_U64((uint64_t)(uint32_t)nes->apu.pulse[0].timer_period);
    HASH_U64((uint64_t)(uint32_t)nes->apu.pulse[0].duty_step);
    HASH_U64((uint64_t)(uint32_t)nes->apu.pulse[0].envelope_decay);
    HASH_U64((uint64_t)(uint32_t)nes->apu.pulse[1].length_counter);
    HASH_U64((uint64_t)(uint32_t)nes->apu.pulse[1].timer_period);
    HASH_U64((uint64_t)(uint32_t)nes->apu.pulse[1].duty_step);
    HASH_U64((uint64_t)(uint32_t)nes->apu.pulse[1].envelope_decay);
    HASH_U64((uint64_t)(uint32_t)nes->apu.triangle.length_counter);
    HASH_U64((uint64_t)(uint32_t)nes->apu.triangle.timer_period);
    HASH_U64((uint64_t)(uint32_t)nes->apu.triangle.sequence_step);
    HASH_U64((uint64_t)(uint32_t)nes->apu.triangle.linear_counter);
    HASH_U64((uint64_t)(uint32_t)nes->apu.noise.length_counter);
    HASH_U64((uint64_t)(uint32_t)nes->apu.noise.timer_period);
    HASH_U64((uint64_t)(uint32_t)nes->apu.noise.shift_register);
    HASH_U64((uint64_t)(uint32_t)nes->apu.noise.envelope_decay);
    HASH_U64(nes->ppu.sprite0_hit_count);
    HASH_U64(nes->ppu.sprite0_opaque_pixel_count);
    HASH_U64(nes->ppu.sprite0_background_overlap_count);
    HASH_U64(nes->ppu.sprite0_hit_ever ? 1u : 0u);
    HASH_U64(nes->ppu.first_sprite0_hit_frame);
    HASH_U64((uint64_t)(uint32_t)(nes->ppu.first_sprite0_hit_scanline + 1));
    HASH_U64((uint64_t)(uint32_t)(nes->ppu.first_sprite0_hit_x + 1));
    HASH_U64(nes->ppu.first_sprite0_opaque_frame);
    HASH_U64((uint64_t)(uint32_t)(nes->ppu.first_sprite0_opaque_scanline + 1));
    HASH_U64((uint64_t)(uint32_t)(nes->ppu.first_sprite0_opaque_x + 1));
    HASH_U64(nes->ppu.sprite_composited_pixel_count);
    HASH_U64(nes->ppu.frames_with_sprite_pixels);
    HASH_U64(nes->ppu.last_completed_sprite_pixels);
    HASH_U64(nes->ppu.first_frame_with_sprite_pixels);
    HASH_U64(nes->ppu.max_scanline_sprite_count);
    HASH_U64(nes->stats.unique_opcodes);
    HASH_U64(nes->stop_info.reason);
    HASH_U64(nes->stop_info.pc);
    HASH_U64(nes->stop_info.opcode);
    for (size_t i = 0; i < 256; ++i) {
        HASH_U64(nes->stats.opcode_counts[i]);
    }

#undef HASH_U64
#undef HASH_BYTE

    return hash;
#endif
}

const char *nes_stop_reason_name(NesStopReason reason) {
    switch (reason) {
    case NES_STOP_NONE:
        return "none";
    case NES_STOP_NO_CARTRIDGE:
        return "no-cartridge";
    case NES_STOP_STEP_LIMIT:
        return "step-limit";
    case NES_STOP_ILLEGAL_OPCODE:
        return "illegal-opcode";
    case NES_STOP_UNSUPPORTED_OPCODE:
        return "unsupported-opcode";
    case NES_STOP_CPU_JAMMED:
        return "cpu-jammed";
    default:
        return "unknown";
    }
}

uint8_t nes_cpu_bus_read(Nes *nes, uint16_t addr) {
    return nes_cpu_bus_read_fast(nes, addr);
}

void nes_cpu_bus_write(Nes *nes, uint16_t addr, uint8_t value) {
    nes_cpu_bus_write_fast(nes, addr, value);
}
