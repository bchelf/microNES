#include "mapper40.h"

/*
 * Mapper 40: pirate FDS-to-cartridge SMB2J board.
 *
 * PRG layout (128 KB / 8 x 16 KB):
 *   $6000-$7FFF: switchable 8 KB bank (from the 8 KB sub-banks)
 *   $8000-$9FFF: fixed to PRG bank 6 (8 KB at prg_rom + 0xC000)
 *   $A000-$BFFF: fixed to PRG bank 7 (8 KB at prg_rom + 0xE000)
 *   $C000-$DFFF: fixed to PRG bank ~4 typically
 *   $E000-$FFFF: fixed to PRG bank 9 / last (vectors)
 *
 * Simplified: fixed 32K at $8000-$FFFF = banks 4-7 of PRG.
 * Switchable 8K at $6000-$7FFF selected by writes to $8000-$FFFF.
 *
 * CHR: 8 KB, not banked.
 *
 * IRQ: cycle-count timer. Writing $A000-$BFFF disables and resets.
 *      Writing $C000-$DFFF enables. Fires after 4096 M2 cycles.
 */

enum {
    MAPPER40_IRQ_PERIOD = 4096,
};

void mapper40_cart_init(NesCartridge *cart) {
    cart->m40_irq_enabled = false;
    cart->m40_irq_counter = 0;
    cart->irq_pending = false;

    /* $6000-$7FFF: switchable, default to bank 6 (offset 0xC000) */
    cart->m40_prg_6000 = cart->prg_rom + 6u * 0x2000u;

    /* $8000-$FFFF: fixed. Banks 4,5,0,7 mapped to $8000-$FFFF. */
    cart->prg_banks_8k[0] = cart->prg_rom + 4u * 0x2000u;
    cart->prg_banks_8k[1] = cart->prg_rom + 5u * 0x2000u;
    cart->prg_banks_8k[2] = cart->prg_rom + 0u * 0x2000u;
    size_t last_8k = cart->prg_rom_size - 0x2000u;
    cart->prg_banks_8k[3] = cart->prg_rom + last_8k;

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
        cart->m40_prg_6000 = cart->prg_rom + offset;
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
