#ifndef MICRONES_NROM_H
#define MICRONES_NROM_H

#include "cart.h"
#include "mmc1.h"
#include "mmc2.h"
#include "mmc3.h"

#include <stddef.h>
#include <stdint.h>

uint8_t nrom_cpu_read(const NesCartridge *cartridge, uint16_t addr);
void nrom_cpu_write(NesCartridge *cartridge, uint16_t addr, uint8_t value);
uint8_t nrom_ppu_read(const NesCartridge *cartridge, uint16_t addr);
void nrom_ppu_write(NesCartridge *cartridge, uint16_t addr, uint8_t value);

static inline size_t mapper_chr_addr(const NesCartridge *cartridge, uint16_t pattern_addr) {
    switch (cartridge->mapper) {
    case 1:
        return mmc1_map_chr_addr(cartridge, pattern_addr);
    case 3: case 11: case 66: {
        size_t base = (size_t)cartridge->cnrom_chr_bank * 0x2000u;
        return (base + (pattern_addr & 0x1fffu)) & cartridge->chr_mask;
    }
    case 4:
        return mmc3_map_chr_addr(cartridge, pattern_addr);
    case 9:
        return mmc2_map_chr_addr(cartridge, pattern_addr);
    default:
        return pattern_addr & cartridge->chr_mask;
    }
}

static inline size_t nrom_chr_row_index(const NesCartridge *cartridge, uint16_t pattern_addr) {
    size_t masked = mapper_chr_addr(cartridge, pattern_addr);
    return ((masked >> 4) * 8u) + (masked & 0x07u);
}

static inline const uint8_t *nrom_ppu_row_pixels(const NesCartridge *cartridge, uint16_t pattern_addr) {
    size_t mapped;
    size_t low_addr;
    uint8_t low;
    uint8_t high;

    if (cartridge->chr_size == 0) {
        return NULL;
    }
    if (cartridge->chr_row_pixels != NULL) {
        return &cartridge->chr_row_pixels[nrom_chr_row_index(cartridge, pattern_addr) * 8u];
    }

    mapped = mapper_chr_addr(cartridge, pattern_addr);

    low_addr = (mapped & (size_t)~0x0fu) + (mapped & 0x07u);
    low = cartridge->chr_data[low_addr & cartridge->chr_mask];
    high = cartridge->chr_data[(low_addr + 8u) & cartridge->chr_mask];
    {
        uint8_t *scratch = ((NesCartridge *)cartridge)->chr_row_scratch;
        for (int x = 0; x < 8; ++x) {
            uint8_t bit = (uint8_t)(7 - x);
            scratch[x] = (uint8_t)((((high >> bit) & 0x01u) << 1) | ((low >> bit) & 0x01u));
        }
        return scratch;
    }
}

#endif
