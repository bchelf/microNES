#ifndef SMB2350_NROM_H
#define SMB2350_NROM_H

#include "cart.h"

#include <stddef.h>
#include <stdint.h>

uint8_t nrom_cpu_read(const NesCartridge *cartridge, uint16_t addr);
void nrom_cpu_write(NesCartridge *cartridge, uint16_t addr, uint8_t value);
uint8_t nrom_ppu_read(const NesCartridge *cartridge, uint16_t addr);
void nrom_ppu_write(NesCartridge *cartridge, uint16_t addr, uint8_t value);

// Inlined here so callers (ppu.c) avoid a cross-TU call on the hot render path.
static inline size_t nrom_chr_row_index(const NesCartridge *cartridge, uint16_t pattern_addr) {
    size_t masked = pattern_addr & cartridge->chr_mask;
    return ((masked >> 4) * 8u) + (masked & 0x07u);
}

static inline const uint8_t *nrom_ppu_row_pixels(const NesCartridge *cartridge, uint16_t pattern_addr) {
    if (cartridge->chr_size == 0 || cartridge->chr_row_pixels == NULL) {
        return NULL;
    }
    return &cartridge->chr_row_pixels[nrom_chr_row_index(cartridge, pattern_addr) * 8u];
}

#endif
