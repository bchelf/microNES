#ifndef SMB2350_CPU6502_H
#define SMB2350_CPU6502_H

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
} Cpu6502;

void cpu6502_init(Cpu6502 *cpu);
void cpu6502_reset(Cpu6502 *cpu, struct Nes *nes);
bool cpu6502_step(Cpu6502 *cpu, struct Nes *nes);

#endif
