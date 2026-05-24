#ifndef MICRONES_MMC2_H
#define MICRONES_MMC2_H

#include "cart.h"

#include <stddef.h>
#include <stdint.h>

void mmc2_cart_init(NesCartridge *cart);
void mmc2_cpu_write(NesCartridge *cart, uint16_t addr, uint8_t value);
void mmc2_latch_update(NesCartridge *cart, uint16_t pattern_addr);

static inline size_t mmc2_map_chr_addr(const NesCartridge *cart, uint16_t addr) {
    uint16_t masked = addr & 0x1fffu;
    uint8_t bank;

    if (masked < 0x1000u) {
        bank = cart->mmc2_latch0 ? cart->mmc2_chr0_fd : cart->mmc2_chr0_fe;
        return ((size_t)bank * 0x1000u + masked) & cart->chr_mask;
    }
    bank = cart->mmc2_latch1 ? cart->mmc2_chr1_fd : cart->mmc2_chr1_fe;
    return ((size_t)bank * 0x1000u + (masked - 0x1000u)) & cart->chr_mask;
}

#endif
