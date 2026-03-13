#include "apu.h"

#include <string.h>

void apu_init(Apu *apu) {
    memset(apu, 0, sizeof(*apu));
}

void apu_reset(Apu *apu) {
    memset(apu->registers, 0, sizeof(apu->registers));
    apu->cpu_cycles = 0;
}

void apu_step(Apu *apu, uint32_t cpu_cycles) {
    apu->cpu_cycles += cpu_cycles;
}

uint8_t apu_cpu_read(Apu *apu, uint16_t addr) {
    if (addr == 0x4015u) {
        return apu->registers[0x15];
    }
    return 0;
}

void apu_cpu_write(Apu *apu, uint16_t addr, uint8_t value) {
    if (addr >= 0x4000u && addr <= 0x4017u) {
        apu->registers[addr - 0x4000u] = value;
    }
}
