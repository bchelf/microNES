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
    uint8_t val;
    if (cartridge->chr_size == 0) {
        return 0;
    }
    val = cartridge->chr_data[mapper_chr_addr(cartridge, addr)];
    if (cartridge->mapper == 9) {
        mmc2_latch_update((NesCartridge *)cartridge, addr);
    }
    return val;
}

void nrom_ppu_write(NesCartridge *cartridge, uint16_t addr, uint8_t value) {
    size_t masked;
    size_t low_addr;

    if (!cartridge->chr_is_ram || cartridge->chr_size == 0) {
        return;
    }

    masked = mapper_chr_addr(cartridge, addr);
    cartridge->chr_data[masked] = value;

    if (cartridge->chr_row_pixels == NULL) {
        return;
    }

    low_addr = (masked & (size_t)~0x0fu) + (masked & 0x07u);
    {
        size_t row_index = ((low_addr >> 4) * 8u) + (low_addr & 0x07u);
        uint8_t *dst = &cartridge->chr_row_pixels[row_index * 8u];
        uint8_t low = cartridge->chr_data[low_addr % cartridge->chr_size];
        uint8_t high = cartridge->chr_data[(low_addr + 8u) % cartridge->chr_size];

        for (int x = 0; x < 8; ++x) {
            uint8_t bit = (uint8_t)(7 - x);
            dst[x] = (uint8_t)((((high >> bit) & 0x01u) << 1) | ((low >> bit) & 0x01u));
        }
    }
}
