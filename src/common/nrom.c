#include "nrom.h"

uint8_t nrom_cpu_read(const NesCartridge *cartridge, uint16_t addr) {
    uint32_t offset = (uint32_t)(addr - 0x8000u);

    if (cartridge->prg_rom_size == 0) {
        return 0xffu;
    }

    if (cartridge->prg_rom_size == 0x4000u) {
        offset &= 0x3fffu;
    } else {
        offset &= 0x7fffu;
    }

    return cartridge->prg_rom[offset];
}

void nrom_cpu_write(NesCartridge *cartridge, uint16_t addr, uint8_t value) {
    (void)cartridge;
    (void)addr;
    (void)value;
}

uint8_t nrom_ppu_read(const NesCartridge *cartridge, uint16_t addr) {
    if (cartridge->chr_size == 0) {
        return 0;
    }
    return cartridge->chr_data[addr % cartridge->chr_size];
}

void nrom_ppu_write(NesCartridge *cartridge, uint16_t addr, uint8_t value) {
    if (!cartridge->chr_is_ram || cartridge->chr_size == 0) {
        return;
    }
    cartridge->chr_data[addr % cartridge->chr_size] = value;
}
