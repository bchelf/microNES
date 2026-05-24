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

static inline size_t mapper40_bank_offset(const NesCartridge *cart, size_t from_end) {
    size_t n8k = cart->prg_rom_size / 0x2000u;
    return ((n8k - 1u - from_end) % n8k) * 0x2000u;
}

void mapper40_cart_init(NesCartridge *cart) {
    cart->m40_irq_enabled = false;
    cart->m40_irq_counter = 0;
    cart->irq_pending = false;

    /* FCEUX ~N = count N banks from the end of PRG ROM.
     * $6000 = ~1 (second-to-last), $8000 = ~3, $A000 = ~2,
     * $C000 = reg (default 0), $E000 = ~0 (last, vectors). */
    cart->m40_prg_6000 = cart->prg_rom + mapper40_bank_offset(cart, 1);

    cart->prg_banks_8k[0] = cart->prg_rom + mapper40_bank_offset(cart, 3);
    cart->prg_banks_8k[1] = cart->prg_rom + mapper40_bank_offset(cart, 2);
    cart->prg_banks_8k[2] = cart->prg_rom;
    cart->prg_banks_8k[3] = cart->prg_rom + mapper40_bank_offset(cart, 0);

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
        size_t n8k = cart->prg_rom_size / 0x2000u;
        size_t bank = (size_t)(value & 0x07u) % n8k;
        cart->prg_banks_8k[2] = cart->prg_rom + bank * 0x2000u;
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
