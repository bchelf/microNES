#include "cnrom.h"

void cnrom_cart_init(NesCartridge *cart) {
    cart->cnrom_chr_bank = 0;
    cart->prg_bank_lo = cart->prg_rom;
    cart->prg_bank_hi = (cart->prg_rom_size == 0x4000u)
        ? cart->prg_rom
        : cart->prg_rom + 0x4000u;
}

void cnrom_cpu_write(NesCartridge *cart, uint16_t addr, uint8_t value) {
    (void)addr;
    cart->cnrom_chr_bank = (uint8_t)(value & 0x03u);
}
