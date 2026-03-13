#ifndef SMB2350_NROM_H
#define SMB2350_NROM_H

#include "cart.h"

#include <stdint.h>

uint8_t nrom_cpu_read(const NesCartridge *cartridge, uint16_t addr);
void nrom_cpu_write(NesCartridge *cartridge, uint16_t addr, uint8_t value);
uint8_t nrom_ppu_read(const NesCartridge *cartridge, uint16_t addr);
void nrom_ppu_write(NesCartridge *cartridge, uint16_t addr, uint8_t value);

#endif
