#include "axrom.h"

static void axrom_update_prg(NesCartridge *cart, uint8_t bank) {
    size_t offset = (size_t)(bank & 0x07u) * 0x8000u;
    if (offset + 0x8000u > cart->prg_rom_size) {
        offset %= cart->prg_rom_size;
    }
    cart->prg_bank_lo = cart->prg_rom + offset;
    cart->prg_bank_hi = cart->prg_rom + offset + 0x4000u;
}

void axrom_cart_init(NesCartridge *cart) {
    cart->mirror_mode = NES_MIRROR_ONE_SCREEN_LOWER;
    axrom_update_prg(cart, 0);
}

void axrom_cpu_write(NesCartridge *cart, uint16_t addr, uint8_t value) {
    (void)addr;
    axrom_update_prg(cart, value);
    cart->mirror_mode = (value & 0x10u)
        ? NES_MIRROR_ONE_SCREEN_UPPER
        : NES_MIRROR_ONE_SCREEN_LOWER;
}
