#include "mmc3.h"

static void mmc3_update_prg_banks(NesCartridge *cart) {
    size_t bank_count_8k = cart->prg_rom_size / 0x2000u;
    if (bank_count_8k == 0) return;
    uint8_t last = (uint8_t)((bank_count_8k - 1u) & 0xffu);
    uint8_t second_last = (uint8_t)((bank_count_8k - 2u) & 0xffu);
    uint8_t bank6 = (uint8_t)(cart->mmc3_bank_data[6] % bank_count_8k);
    uint8_t bank7 = (uint8_t)(cart->mmc3_bank_data[7] % bank_count_8k);

    if ((cart->mmc3_bank_select & 0x40u) == 0) {
        cart->prg_banks_8k[0] = cart->prg_rom + (size_t)bank6 * 0x2000u;
        cart->prg_banks_8k[1] = cart->prg_rom + (size_t)bank7 * 0x2000u;
        cart->prg_banks_8k[2] = cart->prg_rom + (size_t)second_last * 0x2000u;
        cart->prg_banks_8k[3] = cart->prg_rom + (size_t)last * 0x2000u;
    } else {
        cart->prg_banks_8k[0] = cart->prg_rom + (size_t)second_last * 0x2000u;
        cart->prg_banks_8k[1] = cart->prg_rom + (size_t)bank7 * 0x2000u;
        cart->prg_banks_8k[2] = cart->prg_rom + (size_t)bank6 * 0x2000u;
        cart->prg_banks_8k[3] = cart->prg_rom + (size_t)last * 0x2000u;
    }
    cart->prg_bank_lo = cart->prg_banks_8k[0];
    cart->prg_bank_hi = cart->prg_banks_8k[2];
}

void mmc3_cart_init(NesCartridge *cart) {
    cart->mmc3_bank_select = 0;
    cart->mmc3_irq_latch = 0;
    cart->mmc3_irq_counter = 0;
    cart->mmc3_irq_reload = false;
    cart->mmc3_irq_enabled = false;
    cart->irq_pending = false;

    cart->mmc3_bank_data[0] = 0;
    cart->mmc3_bank_data[1] = 2;
    cart->mmc3_bank_data[2] = 4;
    cart->mmc3_bank_data[3] = 5;
    cart->mmc3_bank_data[4] = 6;
    cart->mmc3_bank_data[5] = 7;
    cart->mmc3_bank_data[6] = 0;
    cart->mmc3_bank_data[7] = 1;
    mmc3_update_prg_banks(cart);
}

void mmc3_rebase_banks(NesCartridge *cart, const uint8_t *old_prg_base) {
    for (int i = 0; i < 4; ++i) {
        size_t off = (size_t)(cart->prg_banks_8k[i] - old_prg_base);
        cart->prg_banks_8k[i] = cart->prg_rom + off;
    }
    cart->prg_bank_lo = cart->prg_banks_8k[0];
    cart->prg_bank_hi = cart->prg_banks_8k[2];
}

void mmc3_cpu_write(NesCartridge *cart, uint16_t addr, uint8_t value) {
    switch (addr & 0xe001u) {
    case 0x8000u:
        cart->mmc3_bank_select = value;
        mmc3_update_prg_banks(cart);
        break;
    case 0x8001u:
        cart->mmc3_bank_data[cart->mmc3_bank_select & 0x07u] = value;
        mmc3_update_prg_banks(cart);
        break;
    case 0xa000u:
        cart->mirror_mode = (value & 0x01u) ? NES_MIRROR_HORIZONTAL : NES_MIRROR_VERTICAL;
        break;
    case 0xa001u:
        break;
    case 0xc000u:
        cart->mmc3_irq_latch = value;
        break;
    case 0xc001u:
        cart->mmc3_irq_counter = 0;
        cart->mmc3_irq_reload = true;
        break;
    case 0xe000u:
        cart->mmc3_irq_enabled = false;
        cart->irq_pending = false;
        break;
    case 0xe001u:
        cart->mmc3_irq_enabled = true;
        break;
    }
}

void mmc3_scanline_tick(NesCartridge *cart) {
    if (cart->mmc3_irq_counter == 0 || cart->mmc3_irq_reload) {
        cart->mmc3_irq_counter = cart->mmc3_irq_latch;
        cart->mmc3_irq_reload = false;
    } else {
        --cart->mmc3_irq_counter;
    }

    if (cart->mmc3_irq_counter == 0 && cart->mmc3_irq_enabled) {
        cart->irq_pending = true;
    }
}
