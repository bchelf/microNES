#ifndef MICRONES_NROM_H
#define MICRONES_NROM_H

#include "cart.h"
#include "mmc1.h"
#include "mmc3.h"
#include "mmc5.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

uint8_t nrom_cpu_read(const NesCartridge *cartridge, uint16_t addr);
void nrom_cpu_write(NesCartridge *cartridge, uint16_t addr, uint8_t value);
uint8_t nrom_ppu_read(const NesCartridge *cartridge, uint16_t addr);
void nrom_ppu_write(NesCartridge *cartridge, uint16_t addr, uint8_t value);

// Inlined here so callers (ppu.c) avoid a cross-TU call on the hot render path.
static inline size_t nrom_chr_row_index_sprite(const NesCartridge *cartridge, uint16_t pattern_addr, bool sprite) {
    size_t masked;

    if (cartridge->mapper == 0) {
        masked = pattern_addr & cartridge->chr_mask;
    } else if (cartridge->mapper == 1) {
        masked = mmc1_map_chr_addr(cartridge, pattern_addr);
    } else if (cartridge->mapper == 4) {
        masked = mmc3_map_chr_addr(cartridge, pattern_addr);
    } else if (cartridge->mapper == 5) {
        masked = mmc5_map_chr_addr(cartridge, pattern_addr, sprite);
    } else {
        masked = pattern_addr & cartridge->chr_mask;
    }
    return ((masked >> 4) * 8u) + (masked & 0x07u);
}

static inline size_t nrom_chr_row_index(const NesCartridge *cartridge, uint16_t pattern_addr) {
    return nrom_chr_row_index_sprite(cartridge, pattern_addr, false);
}

static inline const uint8_t *nrom_ppu_row_pixels_sprite(
    const NesCartridge *cartridge,
    uint16_t pattern_addr,
    bool sprite
) {
    if (cartridge->chr_size == 0 || cartridge->chr_row_pixels == NULL) {
        return NULL;
    }
    return &cartridge->chr_row_pixels[nrom_chr_row_index_sprite(cartridge, pattern_addr, sprite) * 8u];
}

static inline const uint8_t *nrom_ppu_row_pixels(const NesCartridge *cartridge, uint16_t pattern_addr) {
    if (cartridge->chr_size == 0 || cartridge->chr_row_pixels == NULL) {
        return NULL;
    }
    return &cartridge->chr_row_pixels[nrom_chr_row_index(cartridge, pattern_addr) * 8u];
}

#endif
