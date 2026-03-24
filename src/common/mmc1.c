#include "mmc1.h"

/* Recompute prg_bank_lo and prg_bank_hi from current MMC1 register state.
 *
 * PRG bank mode (control bits 3-2):
 *   0,1: switch 32 KiB at $8000 (bank register selects aligned pair)
 *     2: fix first bank at $8000; switch 16 KiB at $C000
 *     3: fix last  bank at $C000; switch 16 KiB at $8000  (power-on default)
 */
static void mmc1_update_prg_banks(NesCartridge *cart) {
    uint8_t mode = (cart->mmc1_control >> 2) & 0x03u;
    uint8_t bank = cart->mmc1_prg & 0x0Fu;
    uint8_t last = (uint8_t)(cart->prg_banks - 1u);

    switch (mode) {
    case 0:
    case 1:
        /* 32 KiB switch: ignore lowest bit, map consecutive even/odd pair */
        bank &= 0x0Eu;
        cart->prg_bank_lo = cart->prg_rom + (size_t)bank       * 0x4000u;
        cart->prg_bank_hi = cart->prg_rom + (size_t)(bank + 1u) * 0x4000u;
        break;
    case 2:
        /* Fix first bank at $8000, switch selected bank at $C000 */
        cart->prg_bank_lo = cart->prg_rom;
        cart->prg_bank_hi = cart->prg_rom + (size_t)bank * 0x4000u;
        break;
    default: /* mode 3 */
        /* Fix last bank at $C000, switch selected bank at $8000 */
        cart->prg_bank_lo = cart->prg_rom + (size_t)bank * 0x4000u;
        cart->prg_bank_hi = cart->prg_rom + (size_t)last * 0x4000u;
        break;
    }
}

void mmc1_cart_init(NesCartridge *cart) {
    cart->mmc1_shift       = 0;
    cart->mmc1_shift_count = 0;
    /* Power-on: PRG mode 3 (fix last bank at $C000, switch $8000),
     * 8 KiB CHR mode, one-screen-A mirroring */
    cart->mmc1_control = 0x0Cu;
    cart->mmc1_chr0    = 0;
    cart->mmc1_chr1    = 0;
    cart->mmc1_prg     = 0;
    mmc1_update_prg_banks(cart);
}

void mmc1_rebase_banks(NesCartridge *cart, const uint8_t *old_prg_base) {
    size_t lo_off = (size_t)(cart->prg_bank_lo - old_prg_base);
    size_t hi_off = (size_t)(cart->prg_bank_hi - old_prg_base);
    cart->prg_bank_lo = cart->prg_rom + lo_off;
    cart->prg_bank_hi = cart->prg_rom + hi_off;
}

void mmc1_cpu_write(NesCartridge *cart, uint16_t addr, uint8_t value) {
    uint8_t data;

    /* Bit 7 set: reset shift register and force PRG mode 3 */
    if (value & 0x80u) {
        cart->mmc1_shift       = 0;
        cart->mmc1_shift_count = 0;
        cart->mmc1_control    |= 0x0Cu;
        mmc1_update_prg_banks(cart);
        return;
    }

    /* Shift in bit 0 (LSB-first into a 5-bit register) */
    cart->mmc1_shift |= (uint8_t)((value & 0x01u) << cart->mmc1_shift_count);
    cart->mmc1_shift_count++;

    if (cart->mmc1_shift_count < 5u) {
        return;
    }

    /* Fifth write: commit accumulated value to the register selected by
     * address bits 14-13. */
    data                   = cart->mmc1_shift;
    cart->mmc1_shift       = 0;
    cart->mmc1_shift_count = 0;

    switch ((addr >> 13) & 0x03u) {
    case 0: /* $8000-$9FFF: control */
        cart->mmc1_control = data;
        switch (data & 0x03u) {
        case 2: cart->mirror_mode = NES_MIRROR_VERTICAL;   break;
        case 3: cart->mirror_mode = NES_MIRROR_HORIZONTAL; break;
        default: break; /* one-screen modes: unchanged for now */
        }
        mmc1_update_prg_banks(cart);
        break;
    case 1: /* $A000-$BFFF: CHR bank 0 */
        cart->mmc1_chr0 = data;
        break;
    case 2: /* $C000-$DFFF: CHR bank 1 */
        cart->mmc1_chr1 = data;
        break;
    case 3: /* $E000-$FFFF: PRG bank */
        cart->mmc1_prg = data;
        mmc1_update_prg_banks(cart);
        break;
    }
}
