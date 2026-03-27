#ifndef MICRONES_CPU6502_H
#define MICRONES_CPU6502_H

#include <stdbool.h>
#include <stdint.h>

struct Nes;

enum {
    CPU6502_FLAG_C = 0x01,
    CPU6502_FLAG_Z = 0x02,
    CPU6502_FLAG_I = 0x04,
    CPU6502_FLAG_D = 0x08,
    CPU6502_FLAG_B = 0x10,
    CPU6502_FLAG_U = 0x20,
    CPU6502_FLAG_V = 0x40,
    CPU6502_FLAG_N = 0x80,
};

typedef struct {
    uint8_t a;
    uint8_t x;
    uint8_t y;
    uint8_t sp;
    uint8_t p;
    uint16_t pc;
    uint64_t cycles;
    uint8_t last_opcode;
    bool jammed;
    /* insn_count sits in the 6-byte tail-padding of this struct (offset 20),
     * keeping sizeof(Cpu6502) == 24.  It is incremented on every instruction
     * via a single L32I/ADDI/S32I sequence (offset 20 = L32I imm5) instead of
     * the uint64_t increment at a ~82 KB struct offset in NesExecutionStats. */
    uint32_t insn_count;
} Cpu6502;

typedef struct {
    uint64_t instruction_index;
    uint16_t pc;
    uint8_t opcode;
    uint8_t a;
    uint8_t x;
    uint8_t y;
    uint8_t sp;
    uint8_t p;
    uint64_t cpu_cycles;
} Cpu6502TraceEntry;

void cpu6502_init(Cpu6502 *cpu);
void cpu6502_reset(Cpu6502 *cpu, struct Nes *nes);
bool cpu6502_step(Cpu6502 *cpu, struct Nes *nes);
/* Runs cpu6502_step in a loop until the PPU signals a completed scanline.
 * Lives in the same TU as cpu6502_step so the compiler can inline the step
 * function into the loop body (eliminating per-instruction call overhead).
 * Does NOT flush pending_apu_cycles – that is the caller's responsibility. */
bool cpu6502_run_scanline(Cpu6502 *cpu, struct Nes *nes);

#endif
