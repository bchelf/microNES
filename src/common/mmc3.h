#ifndef MICRONES_MMC3_H
#define MICRONES_MMC3_H

#include "cart.h"

#include <stddef.h>
#include <stdint.h>

void mmc3_cart_init(NesCartridge *cart);
void mmc3_rebase_banks(NesCartridge *cart, const uint8_t *old_prg_base);
void mmc3_cpu_write(NesCartridge *cart, uint16_t addr, uint8_t value);
void mmc3_scanline_tick(NesCartridge *cart);

static inline size_t mmc3_map_chr_addr(const NesCartridge *cart, uint16_t addr) {
    uint16_t masked = addr & 0x1fffu;
    uint8_t slot;
    size_t bank_offset;

    if ((cart->mmc3_bank_select & 0x80u) == 0) {
        if (masked < 0x0800u) {
            bank_offset = (size_t)(cart->mmc3_bank_data[0] & 0xfeu) * 0x0400u;
            return (bank_offset + masked) & cart->chr_mask;
        }
        if (masked < 0x1000u) {
            bank_offset = (size_t)(cart->mmc3_bank_data[1] & 0xfeu) * 0x0400u;
            return (bank_offset + (masked - 0x0800u)) & cart->chr_mask;
        }
        slot = (uint8_t)(2u + ((masked - 0x1000u) >> 10));
        bank_offset = (size_t)cart->mmc3_bank_data[slot] * 0x0400u;
        return (bank_offset + (masked & 0x03ffu)) & cart->chr_mask;
    }

    if (masked < 0x1000u) {
        slot = (uint8_t)(2u + (masked >> 10));
        bank_offset = (size_t)cart->mmc3_bank_data[slot] * 0x0400u;
        return (bank_offset + (masked & 0x03ffu)) & cart->chr_mask;
    }
    if (masked < 0x1800u) {
        bank_offset = (size_t)(cart->mmc3_bank_data[0] & 0xfeu) * 0x0400u;
        return (bank_offset + (masked - 0x1000u)) & cart->chr_mask;
    }
    bank_offset = (size_t)(cart->mmc3_bank_data[1] & 0xfeu) * 0x0400u;
    return (bank_offset + (masked - 0x1800u)) & cart->chr_mask;
}

#endif
