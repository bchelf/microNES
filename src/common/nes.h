#ifndef SMB2350_NES_H
#define SMB2350_NES_H

#include "apu.h"
#include "cart.h"
#include "cpu6502.h"
#include "cpu6502_opcode.h"
#include "framebuffer.h"
#include "input.h"
#include "nrom.h"
#include "ppu.h"
#include "runtime_config.h"
#include "scanline.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { NES_TRACE_CAPACITY = 16 };

typedef enum {
    NES_STOP_NONE = 0,
    NES_STOP_NO_CARTRIDGE,
    NES_STOP_STEP_LIMIT,
    NES_STOP_ILLEGAL_OPCODE,
    NES_STOP_UNSUPPORTED_OPCODE,
    NES_STOP_CPU_JAMMED,
} NesStopReason;

typedef struct {
    uint64_t instruction_count;
    uint64_t opcode_counts[256];
    uint32_t unique_opcodes;
    uint64_t nmi_count;
} NesExecutionStats;

typedef struct {
    NesStopReason reason;
    uint16_t pc;
    uint8_t opcode;
    bool opcode_is_official;
    bool opcode_is_supported;
    uint64_t instruction_index;
} NesStopInfo;

typedef struct {
    uint64_t cpu_exec_us_total;
    uint64_t ppu_step_us_total;
    uint64_t bus_read_count;
    uint64_t bus_write_count;
} NesStepProfile;

typedef struct Nes {
    Cpu6502 cpu;
    Ppu ppu;
    Apu apu;
    NesCartridge cartridge;
    NesController controllers[2];
    uint8_t cpu_ram[2048];
    NesExecutionStats stats;
    NesStopInfo stop_info;
    Cpu6502TraceEntry trace[NES_TRACE_CAPACITY];
    uint8_t trace_head;
    uint8_t trace_count;
    smb2350_profile_now_us_fn profile_now_us;
    void *profile_now_user;
    NesStepProfile step_profile;
    char last_error[1024];
} Nes;

void nes_init(Nes *nes);
void nes_destroy(Nes *nes);
bool nes_load_cartridge_file(Nes *nes, const char *path);
bool nes_load_cartridge_memory(Nes *nes, const uint8_t *rom_image, size_t rom_image_size);
void nes_reset(Nes *nes);
void nes_set_profile_clock(Nes *nes, smb2350_profile_now_us_fn now_us, void *user);
void nes_set_controller_state(Nes *nes, unsigned controller_index, NesControllerState state);
void nes_set_sprite0_diag_window(Nes *nes, uint64_t frame_start, uint64_t frame_end);
bool nes_step_instruction(Nes *nes);
bool nes_step_scanline(Nes *nes);
bool nes_step_frame(Nes *nes);
uint32_t nes_audio_sample_rate(const Nes *nes);
size_t nes_audio_available_samples(const Nes *nes);
size_t nes_audio_read_samples(Nes *nes, int16_t *dst, size_t max_samples);
void nes_audio_set_mix_enable_mask(Nes *nes, uint8_t mask);
uint8_t nes_audio_mix_enable_mask(const Nes *nes);
void nes_audio_set_test_tone(Nes *nes, ApuDebugTestTone mode);
ApuDebugTestTone nes_audio_test_tone(const Nes *nes);
void nes_audio_debug_reset_metrics(Nes *nes);
void nes_audio_debug_get_report(const Nes *nes, ApuDebugReport *report);

const Cpu6502 *nes_cpu_state(const Nes *nes);
uint64_t nes_frame_count(const Nes *nes);
int nes_scanline(const Nes *nes);
const NesFrameBuffer *nes_framebuffer(const Nes *nes);
const NesScanline *nes_scanline_buffer(const Nes *nes);
const char *nes_last_error(const Nes *nes);
const NesExecutionStats *nes_execution_stats(const Nes *nes);
const NesStopInfo *nes_stop_info(const Nes *nes);
size_t nes_trace_copy(const Nes *nes, Cpu6502TraceEntry *out_entries, size_t max_entries);
uint64_t nes_state_hash(const Nes *nes);
const char *nes_stop_reason_name(NesStopReason reason);

static inline uint8_t nes_nrom_prg_read_fast(const Nes *nes, uint16_t addr) {
    uint32_t offset;

    if (nes->cartridge.prg_rom_size == 0) {
        return 0xffu;
    }

    offset = (uint32_t)(addr - 0x8000u);
    offset &= (nes->cartridge.prg_rom_size == 0x4000u) ? 0x3fffu : 0x7fffu;
    return nes->cartridge.prg_rom[offset];
}

static inline uint8_t nes_cpu_bus_read_fast(Nes *nes, uint16_t addr) {
#if SMB2350_ENABLE_STEP_PROFILING
    ++nes->step_profile.bus_read_count;
#endif
    if (addr < 0x2000u) {
        return nes->cpu_ram[addr & 0x07ffu];
    }
    if (addr < 0x4000u) {
        return ppu_cpu_read(&nes->ppu, &nes->cartridge, (uint16_t)(0x2000u + (addr & 0x0007u)));
    }
    if (addr == 0x4016u) {
        return input_controller_read(&nes->controllers[0]);
    }
    if (addr == 0x4017u) {
        return input_controller_read(&nes->controllers[1]);
    }
    if (addr >= 0x4000u && addr <= 0x4017u) {
        return apu_cpu_read(&nes->apu, addr);
    }
    if (addr >= 0x8000u) {
        return nes_nrom_prg_read_fast(nes, addr);
    }
    return 0;
}

static inline void nes_cpu_bus_write_fast(Nes *nes, uint16_t addr, uint8_t value) {
#if SMB2350_ENABLE_STEP_PROFILING
    ++nes->step_profile.bus_write_count;
#endif
    if (addr < 0x2000u) {
        nes->cpu_ram[addr & 0x07ffu] = value;
        return;
    }
    if (addr < 0x4000u) {
        ppu_cpu_write(&nes->ppu, &nes->cartridge, (uint16_t)(0x2000u + (addr & 0x0007u)), value);
        return;
    }
    if (addr == 0x4014u) {
        uint16_t base = (uint16_t)value << 8;
        for (uint16_t i = 0; i < 256u; ++i) {
            ppu_oam_write_byte(
                &nes->ppu,
                (uint8_t)(nes->ppu.oam_addr + i),
                nes_cpu_bus_read_fast(nes, (uint16_t)(base + i)),
                true
            );
        }
        return;
    }
    if (addr == 0x4016u) {
        input_controller_write_strobe(&nes->controllers[0], value);
        input_controller_write_strobe(&nes->controllers[1], value);
        return;
    }
    if (addr >= 0x4000u && addr <= 0x4017u) {
        apu_cpu_write(&nes->apu, addr, value);
        return;
    }
    if (addr >= 0x8000u) {
        nrom_cpu_write(&nes->cartridge, addr, value);
    }
}

uint8_t nes_cpu_bus_read(Nes *nes, uint16_t addr);
void nes_cpu_bus_write(Nes *nes, uint16_t addr, uint8_t value);

static inline uint64_t nes_profile_now_us(const Nes *nes) {
#if SMB2350_ENABLE_STEP_PROFILING
    if (nes->profile_now_us != NULL) {
        return nes->profile_now_us(nes->profile_now_user);
    }
#else
    (void)nes;
#endif
    return 0;
}

#endif
