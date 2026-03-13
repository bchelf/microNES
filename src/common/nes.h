#ifndef SMB2350_NES_H
#define SMB2350_NES_H

#include "apu.h"
#include "cart.h"
#include "cpu6502.h"
#include "cpu6502_opcode.h"
#include "framebuffer.h"
#include "input.h"
#include "ppu.h"
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
    char last_error[1024];
} Nes;

void nes_init(Nes *nes);
void nes_destroy(Nes *nes);
bool nes_load_cartridge_file(Nes *nes, const char *path);
void nes_reset(Nes *nes);
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

uint8_t nes_cpu_bus_read(Nes *nes, uint16_t addr);
void nes_cpu_bus_write(Nes *nes, uint16_t addr, uint8_t value);

#endif
