#include "colordreams.h"

void colordreams_cart_init(NesCartridge *cart) {
    cart->cnrom_chr_bank = 0;
    cart->prg_bank_lo = cart->prg_rom;
    cart->prg_bank_hi = cart->prg_rom + 0x4000u;
}

void colordreams_cpu_write(NesCartridge *cart, uint16_t addr, uint8_t value) {
    (void)addr;
    uint8_t prg_bank = value & 0x03u;
    cart->cnrom_chr_bank = (uint8_t)((value >> 4) & 0x0fu);

    size_t prg_offset = (size_t)prg_bank * 0x8000u;
    if (prg_offset + 0x8000u > cart->prg_rom_size) {
        prg_offset %= cart->prg_rom_size;
    }
    cart->prg_bank_lo = cart->prg_rom + prg_offset;
    cart->prg_bank_hi = cart->prg_rom + prg_offset + 0x4000u;
}
