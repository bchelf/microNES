#ifndef MICRONES_NROM_H
#define MICRONES_NROM_H

#include "cart.h"
#include "mmc1.h"

#include <stddef.h>
#include <stdint.h>

uint8_t nrom_cpu_read(const NesCartridge *cartridge, uint16_t addr);
void nrom_cpu_write(NesCartridge *cartridge, uint16_t addr, uint8_t value);
uint8_t nrom_ppu_read(const NesCartridge *cartridge, uint16_t addr);
void nrom_ppu_write(NesCartridge *cartridge, uint16_t addr, uint8_t value);

// Inlined here so callers (ppu.c) avoid a cross-TU call on the hot render path.
static inline size_t nrom_chr_row_index(const NesCartridge *cartridge, uint16_t pattern_addr) {
    size_t masked;

    if (cartridge->mapper == 1) {
        masked = mmc1_map_chr_addr(cartridge, pattern_addr);
    } else {
        masked = pattern_addr & cartridge->chr_mask;
    }
    return ((masked >> 4) * 8u) + (masked & 0x07u);
}

static inline const uint8_t *nrom_ppu_row_pixels(const NesCartridge *cartridge, uint16_t pattern_addr) {
    size_t mapped;
    size_t low_addr;
    uint8_t low;
    uint8_t high;
    uint8_t *scratch;

    if (cartridge->chr_size == 0) {
        return NULL;
    }
    if (cartridge->chr_row_pixels != NULL) {
        return &cartridge->chr_row_pixels[nrom_chr_row_index(cartridge, pattern_addr) * 8u];
    }

    if (cartridge->mapper == 1) {
        mapped = mmc1_map_chr_addr(cartridge, pattern_addr);
    } else {
        mapped = pattern_addr & cartridge->chr_mask;
    }

    low_addr = (mapped & (size_t)~0x0fu) + (mapped & 0x07u);
    low = cartridge->chr_data[low_addr & cartridge->chr_mask];
    high = cartridge->chr_data[(low_addr + 8u) & cartridge->chr_mask];
    scratch = ((NesCartridge *)cartridge)->chr_row_scratch;
    for (int x = 0; x < 8; ++x) {
        uint8_t bit = (uint8_t)(7 - x);
        scratch[x] = (uint8_t)((((high >> bit) & 0x01u) << 1) | ((low >> bit) & 0x01u));
    }
    return scratch;
}

#endif
