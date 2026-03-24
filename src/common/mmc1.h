#ifndef MICRONES_MMC1_H
#define MICRONES_MMC1_H

#include "cart.h"

#include <stdint.h>

/*
 * Mapper 1 / MMC1 (SxROM) support.
 *
 * Used by: The Legend of Zelda (PRG0), among others.
 *   PRG: up to 16 x 16 KiB banks (256 KiB total)
 *   CHR: up to 32 x 4 KiB banks (128 KiB), or CHR RAM if chr_banks == 0
 *
 * Banking is controlled by a 5-bit serial shift register. Each write to
 * $8000-$FFFF shifts in bit 0; after 5 writes the value is committed to
 * one of four internal registers selected by address bits 14-13.
 * Writing with bit 7 set resets the shift register and forces PRG mode 3.
 *
 * Not touching: CPU, PPU, APU, renderer.
 * PPU CHR reads/writes are handled by nrom_ppu_read/nrom_ppu_write because
 * Zelda uses 8 KiB CHR RAM with no effective CHR banking.
 */

/* Set initial MMC1 register state and prg_bank_lo/prg_bank_hi pointers.
 * Call this from cart.c after cart_parse_ines_image for mapper-1 ROMs. */
void mmc1_cart_init(NesCartridge *cart);

/* Rebase prg_bank_lo/prg_bank_hi after prg_rom is moved to a new allocation
 * (used by cart_load_ines_const_memory when copying PRG to DRAM). */
void mmc1_rebase_banks(NesCartridge *cart, const uint8_t *old_prg_base);

/* Called from nes_cpu_bus_write_fast for every write to $8000-$FFFF. */
void mmc1_cpu_write(NesCartridge *cart, uint16_t addr, uint8_t value);

#endif
