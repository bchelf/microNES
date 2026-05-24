#include "mmc2.h"

void mmc2_cart_init(NesCartridge *cart) {
    cart->mmc2_chr0_fd = 0;
    cart->mmc2_chr0_fe = 0;
    cart->mmc2_chr1_fd = 0;
    cart->mmc2_chr1_fe = 0;
    cart->mmc2_latch0 = true;
    cart->mmc2_latch1 = true;

    size_t n8k = cart->prg_rom_size / 0x2000u;
    cart->prg_banks_8k[0] = cart->prg_rom;
    cart->prg_banks_8k[1] = cart->prg_rom + (n8k - 3u) * 0x2000u;
    cart->prg_banks_8k[2] = cart->prg_rom + (n8k - 2u) * 0x2000u;
    cart->prg_banks_8k[3] = cart->prg_rom + (n8k - 1u) * 0x2000u;
    cart->prg_bank_lo = cart->prg_banks_8k[0];
    cart->prg_bank_hi = cart->prg_banks_8k[2];
}

void mmc2_cpu_write(NesCartridge *cart, uint16_t addr, uint8_t value) {
    switch (addr & 0xf000u) {
    case 0xa000u: {
        uint8_t bank = value & 0x0fu;
        size_t offset = (size_t)bank * 0x2000u;
        if (offset + 0x2000u > cart->prg_rom_size) {
            offset %= cart->prg_rom_size;
        }
        cart->prg_banks_8k[0] = cart->prg_rom + offset;
        cart->prg_bank_lo = cart->prg_banks_8k[0];
        break;
    }
    case 0xb000u:
        cart->mmc2_chr0_fd = value & 0x1fu;
        break;
    case 0xc000u:
        cart->mmc2_chr0_fe = value & 0x1fu;
        break;
    case 0xd000u:
        cart->mmc2_chr1_fd = value & 0x1fu;
        break;
    case 0xe000u:
        cart->mmc2_chr1_fe = value & 0x1fu;
        break;
    case 0xf000u:
        cart->mirror_mode = (value & 0x01u)
            ? NES_MIRROR_HORIZONTAL
            : NES_MIRROR_VERTICAL;
        break;
    }
}

void mmc2_latch_update(NesCartridge *cart, uint16_t pattern_addr) {
    uint16_t tile = (pattern_addr >> 4) & 0xffu;
    if (pattern_addr < 0x1000u) {
        if (tile == 0xfdu) cart->mmc2_latch0 = true;
        else if (tile == 0xfeu) cart->mmc2_latch0 = false;
    } else {
        if (tile == 0xfdu) cart->mmc2_latch1 = true;
        else if (tile == 0xfeu) cart->mmc2_latch1 = false;
    }
}
