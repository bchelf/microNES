#ifndef SMB2350_APU_H
#define SMB2350_APU_H

#include <stdint.h>

typedef struct {
    uint8_t registers[0x18];
    uint64_t cpu_cycles;
} Apu;

void apu_init(Apu *apu);
void apu_reset(Apu *apu);
void apu_step(Apu *apu, uint32_t cpu_cycles);
uint8_t apu_cpu_read(Apu *apu, uint16_t addr);
void apu_cpu_write(Apu *apu, uint16_t addr, uint8_t value);

#endif
