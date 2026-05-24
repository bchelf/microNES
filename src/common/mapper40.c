#include "mapper40.h"

/*
 * Mapper 40: pirate FDS-to-cartridge SMB2J board.
 * Source: FCEUX 40.cpp, Mesen2 Mapper40.h (both agree exactly).
 *
 * PRG layout (64 KB = 8 x 8 KB banks):
 *   $6000-$7FFF: fixed bank 6
 *   $8000-$9FFF: fixed bank 4
 *   $A000-$BFFF: fixed bank 5
 *   $C000-$DFFF: switchable (default bank 0, selected by $E000 writes)
 *   $E000-$FFFF: fixed bank 7 (vectors)
 *
 * Registers:
 *   $8000: IRQ disable, acknowledge, counter reset
 *   $A000: IRQ enable (counter starts from 0)
 *   $C000: ignored
 *   $E000: bank select (bits 0-2 -> 8KB bank at $C000)
 *
 * IRQ: 12-bit counter increments each CPU cycle; fires at 4096.
 */

enum {
    MAPPER40_IRQ_PERIOD = 4096,
};

void mapper40_cart_init(NesCartridge *cart) {
    cart->m40_irq_enabled = false;
    cart->m40_irq_counter = 0;
    cart->irq_pending = false;

    cart->m40_prg_6000 = cart->prg_rom + 6u * 0x2000u;

    cart->prg_banks_8k[0] = cart->prg_rom + 4u * 0x2000u;
    cart->prg_banks_8k[1] = cart->prg_rom + 5u * 0x2000u;
    cart->prg_banks_8k[2] = cart->prg_rom + 0u * 0x2000u;
    cart->prg_banks_8k[3] = cart->prg_rom + 7u * 0x2000u;

    cart->prg_bank_lo = cart->prg_banks_8k[0];
    cart->prg_bank_hi = cart->prg_banks_8k[2];
}

void mapper40_cpu_write(NesCartridge *cart, uint16_t addr, uint8_t value) {
    switch (addr & 0xe000u) {
    case 0x8000u:
        cart->m40_irq_enabled = false;
        cart->irq_pending = false;
        cart->m40_irq_counter = 0;
        break;
    case 0xa000u:
        cart->m40_irq_enabled = true;
        cart->m40_irq_counter = 0;
        break;
    case 0xc000u:
        break;
    case 0xe000u: {
        uint8_t bank = value & 0x07u;
        size_t offset = (size_t)bank * 0x2000u;
        if (offset + 0x2000u > cart->prg_rom_size) {
            offset %= cart->prg_rom_size;
        }
        cart->prg_banks_8k[2] = cart->prg_rom + offset;
        cart->prg_bank_hi = cart->prg_banks_8k[2];
        break;
    }
    }
}

void mapper40_tick(NesCartridge *cart, uint32_t cpu_cycles) {
    if (!cart->m40_irq_enabled) {
        return;
    }
    cart->m40_irq_counter += cpu_cycles;
    if (cart->m40_irq_counter >= MAPPER40_IRQ_PERIOD) {
        cart->irq_pending = true;
        cart->m40_irq_enabled = false;
    }
}
