#include "uxrom.h"

void uxrom_cart_init(NesCartridge *cart) {
    cart->prg_bank_lo = cart->prg_rom;
    cart->prg_bank_hi = cart->prg_rom + (cart->prg_rom_size - 0x4000u);
}

void uxrom_cpu_write(NesCartridge *cart, uint16_t addr, uint8_t value) {
    (void)addr;
    uint8_t bank = (uint8_t)(value & 0x0fu);
    size_t offset = (size_t)bank * 0x4000u;
    if (offset + 0x4000u > cart->prg_rom_size) {
        offset %= cart->prg_rom_size;
    }
    cart->prg_bank_lo = cart->prg_rom + offset;
}
