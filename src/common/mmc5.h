#ifndef MICRONES_MMC5_H
#define MICRONES_MMC5_H

#include "cart.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void mmc5_cart_init(NesCartridge *cart);
void mmc5_rebase_banks(NesCartridge *cart, const uint8_t *old_prg_base);
uint8_t mmc5_cpu_read(NesCartridge *cart, uint16_t addr);
void mmc5_cpu_write(NesCartridge *cart, uint16_t addr, uint8_t value);
void mmc5_scanline_tick(NesCartridge *cart, bool rendering_enabled);
uint8_t mmc5_nametable_read(const NesCartridge *cart, const uint8_t *ciram, uint16_t addr);
void mmc5_nametable_write(NesCartridge *cart, uint8_t *ciram, uint16_t addr, uint8_t value);

static inline size_t mmc5_map_chr_addr(const NesCartridge *cart, uint16_t addr, bool sprite) {
    uint16_t masked = addr & 0x1fffu;
    const uint8_t *banks = sprite ? cart->mmc5_chr_sprite_bank : cart->mmc5_chr_bg_bank;
    size_t bank_offset;

    switch (cart->mmc5_chr_mode & 0x03u) {
    case 0:
        bank_offset = (size_t)banks[sprite ? 7 : 3] * 0x2000u;
        return (bank_offset + masked) & cart->chr_mask;
    case 1:
        bank_offset = (size_t)banks[(masked >> 12) | (sprite ? 6 : 2)] * 0x1000u;
        return (bank_offset + (masked & 0x0fffu)) & cart->chr_mask;
    case 2:
        bank_offset = (size_t)banks[(masked >> 11) | (sprite ? 4 : 0)] * 0x0800u;
        return (bank_offset + (masked & 0x07ffu)) & cart->chr_mask;
    default:
        bank_offset = (size_t)banks[sprite ? (masked >> 10) : ((masked >> 10) & 0x03u)] * 0x0400u;
        return (bank_offset + (masked & 0x03ffu)) & cart->chr_mask;
    }
}

#endif
