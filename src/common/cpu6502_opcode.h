#ifndef MICRONES_CPU6502_OPCODE_H
#define MICRONES_CPU6502_OPCODE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *mnemonic;
    const char *addressing_mode;
    bool official;
    bool supported;
} Cpu6502OpcodeInfo;

const Cpu6502OpcodeInfo *cpu6502_opcode_info(uint8_t opcode);

#endif
