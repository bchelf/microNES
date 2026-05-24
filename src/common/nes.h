#ifndef MICRONES_NES_H
#define MICRONES_NES_H

#include "apu.h"
#include "axrom.h"
#include "cart.h"
#include "cnrom.h"
#include "colordreams.h"
#include "cpu6502.h"
#include "cpu6502_opcode.h"
#include "framebuffer.h"
#include "gxrom.h"
#include "input.h"
#include "mapper40.h"
#include "mmc1.h"
#include "mmc2.h"
#include "mmc3.h"
#include "nrom.h"
#include "ppu.h"
#include "runtime_config.h"
#include "scanline.h"
#include "uxrom.h"

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
    uint64_t scanline_step_us_total;
    uint64_t apu_step_us_total;
    uint64_t cpu_exec_cycles_total;
    uint64_t ppu_step_cycles_total;
    uint64_t scanline_step_cycles_total;
    uint64_t apu_step_cycles_total;
    uint64_t bus_read_count;
    uint64_t bus_write_count;
    uint64_t scanline_count;
    uint64_t apu_step_count;
} NesStepProfile;

typedef struct Nes {
    Cpu6502 cpu;
    /* pending_apu_cycles is kept immediately after cpu (offset 24) so that the
     * hot-path increment in cpu6502_step uses L32I with a small immediate
     * (offset 24 = L32I imm6) instead of a large-offset L32R+ADD sequence. */
    uint32_t pending_apu_cycles;  /* accumulated CPU cycles not yet flushed to apu_step */
    /* prg_bank_lo/hi are hot caches of cartridge.prg_bank_lo/hi placed at
     * offsets 28/32 so cpu_fetch8, cpu_fetch16, and nes_cpu_bus_read_fast can
     * load them with a single small-immediate L32I instead of L32R+ADD through
     * the ~70 KB cartridge offset.  Sync via nes_sync_prg_cache() on cart load,
     * reset, and MMC1 bank switches. */
    const uint8_t *prg_bank_lo;   /* offset 28: $8000-$BFFF window */
    const uint8_t *prg_bank_hi;   /* offset 32: $C000-$FFFF window */
    /* For NROM (mapper 0) both PRG windows are a contiguous region of prg_rom,
     * so a single masked index into prg_bank_lo replaces the two-branch bank
     * select in cpu_fetch8/16.  0x3FFF = NROM-128 (16 KiB mirror), 0x7FFF =
     * NROM-256 (32 KiB flat).  0 = any other mapper → use two-pointer path. */
    uint32_t prg_fetch_mask;      /* offset 36 */
    /* cpu_ram[] is embedded at a ~74 KB offset in the Nes struct; caching its
     * base address here lets nes_cpu_bus_read/write_fast reach it with a single
     * small-immediate L32I instead of an L32R+ADD through the large offset. */
    uint8_t *cpu_ram_base;        /* offset 40 */
    Ppu ppu;                      /* offset 44 */
    Apu apu;
    NesCartridge cartridge;
    NesController controllers[2];
    uint8_t cpu_ram[2048];
    uint8_t wram[8192];   /* $6000-$7FFF battery-backed PRG-RAM (MMC1) */
    NesExecutionStats stats;
    NesStopInfo stop_info;
    Cpu6502TraceEntry trace[NES_TRACE_CAPACITY];
    uint8_t trace_head;
    uint8_t trace_count;
    micrones_profile_now_us_fn profile_now_us;
    void *profile_now_user;
    NesStepProfile step_profile;
    char last_error[1024];
} Nes;

void nes_init(Nes *nes);
void nes_destroy(Nes *nes);
bool nes_load_cartridge_file(Nes *nes, const char *path);
bool nes_load_cartridge_memory(Nes *nes, const uint8_t *rom_image, size_t rom_image_size);
bool nes_load_cartridge_const_memory(Nes *nes, const uint8_t *rom_image, size_t rom_image_size);
void nes_reset(Nes *nes);
void nes_set_profile_clock(Nes *nes, micrones_profile_now_us_fn now_us, void *user);
void nes_set_controller_state(Nes *nes, unsigned controller_index, NesControllerState state);
void nes_set_render_target(Nes *nes, NesFrameBuffer *fb);
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
    return nes->cartridge.prg_rom[(uint32_t)(addr - 0x8000u) & nes->cartridge.prg_rom_mask];
}

/* Sync the prg_bank_lo/hi hot cache from cartridge after any event that may
 * change the bank pointers (cart load, reset, MMC1 write). */
static inline void nes_sync_prg_cache(Nes *nes) {
    nes->prg_bank_lo = nes->cartridge.prg_bank_lo;
    nes->prg_bank_hi = nes->cartridge.prg_bank_hi;
    /* NROM (mapper 0): both windows are contiguous in prg_rom, so a single
     * masked index suffices.  All other mappers may have non-adjacent banks. */
    nes->prg_fetch_mask = (nes->cartridge.mapper == 0)
        ? (uint32_t)(nes->cartridge.prg_rom_size - 1u) & 0x7FFFu
        : 0u;
}

static inline uint8_t nes_cpu_bus_read_fast(Nes *nes, uint16_t addr) {
#if MICRONES_ENABLE_STEP_PROFILING
    ++nes->step_profile.bus_read_count;
#endif
    if (addr >= 0x8000u) {
        uint32_t off = (uint32_t)(addr - 0x8000u);
        uint32_t mask = nes->prg_fetch_mask;
        if (__builtin_expect(mask != 0u, 1)) {
            return nes->prg_bank_lo[off & mask];
        }
        if (__builtin_expect(nes->cartridge.mapper == 4 || nes->cartridge.mapper == 9
                            || nes->cartridge.mapper == 40, 0)) {
            return nes->cartridge.prg_banks_8k[off >> 13][off & 0x1fffu];
        }
        if (off < 0x4000u) {
            return nes->prg_bank_lo[off];
        }
        return nes->prg_bank_hi[off - 0x4000u];
    }
    if (addr < 0x2000u) {
        return nes->cpu_ram_base[addr & 0x07ffu];
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
    if (addr >= 0x6000u && addr < 0x8000u) {
        if (__builtin_expect(nes->cartridge.mapper == 40, 0)) {
            return nes->cartridge.m40_prg_6000[addr - 0x6000u];
        }
        return nes->wram[addr - 0x6000u];
    }
    return 0;
}

static inline void nes_cpu_bus_write_fast(Nes *nes, uint16_t addr, uint8_t value) {
#if MICRONES_ENABLE_STEP_PROFILING
    ++nes->step_profile.bus_write_count;
#endif
    if (addr < 0x2000u) {
        nes->cpu_ram_base[addr & 0x07ffu] = value;
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
    if (addr >= 0x6000u && addr < 0x8000u) {
        nes->wram[addr - 0x6000u] = value;
        return;
    }
    if (addr >= 0x8000u) {
        switch (nes->cartridge.mapper) {
        case 1:
            mmc1_cpu_write(&nes->cartridge, addr, value);
            nes_sync_prg_cache(nes);
            break;
        case 2:
            uxrom_cpu_write(&nes->cartridge, addr, value);
            nes_sync_prg_cache(nes);
            break;
        case 3:
            cnrom_cpu_write(&nes->cartridge, addr, value);
            break;
        case 4:
            mmc3_cpu_write(&nes->cartridge, addr, value);
            nes_sync_prg_cache(nes);
            break;
        case 7:
            axrom_cpu_write(&nes->cartridge, addr, value);
            nes_sync_prg_cache(nes);
            break;
        case 9:
            mmc2_cpu_write(&nes->cartridge, addr, value);
            nes_sync_prg_cache(nes);
            break;
        case 40:
            mapper40_cpu_write(&nes->cartridge, addr, value);
            break;
        case 11:
            colordreams_cpu_write(&nes->cartridge, addr, value);
            nes_sync_prg_cache(nes);
            break;
        case 66:
            gxrom_cpu_write(&nes->cartridge, addr, value);
            nes_sync_prg_cache(nes);
            break;
        }
        return;
    }
}

uint8_t nes_cpu_bus_read(Nes *nes, uint16_t addr);
void nes_cpu_bus_write(Nes *nes, uint16_t addr, uint8_t value);

static inline uint64_t nes_profile_now_us(const Nes *nes) {
#if MICRONES_ENABLE_STEP_PROFILING
    if (nes->profile_now_us != NULL) {
        return nes->profile_now_us(nes->profile_now_user);
    }
#else
    (void)nes;
#endif
    return 0;
}

#endif
