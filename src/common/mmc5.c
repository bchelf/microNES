#include "mmc5.h"

static const uint8_t *mmc5_prg_rom_bank(const NesCartridge *cart, uint8_t bank) {
    size_t bank_count_8k = cart->prg_rom_size / 0x2000u;
    return cart->prg_rom + (size_t)(bank % bank_count_8k) * 0x2000u;
}

static void mmc5_update_prg_banks(NesCartridge *cart) {
    uint8_t last = (uint8_t)(cart->prg_rom_size / 0x2000u - 1u);

    switch (cart->mmc5_prg_mode & 0x03u) {
    case 0: {
        uint8_t bank = (uint8_t)(cart->mmc5_prg_bank[4] & 0xfcu);
        cart->prg_banks_8k[0] = mmc5_prg_rom_bank(cart, bank);
        cart->prg_banks_8k[1] = mmc5_prg_rom_bank(cart, (uint8_t)(bank + 1u));
        cart->prg_banks_8k[2] = mmc5_prg_rom_bank(cart, (uint8_t)(bank + 2u));
        cart->prg_banks_8k[3] = mmc5_prg_rom_bank(cart, (uint8_t)(bank + 3u));
        break;
    }
    case 1: {
        uint8_t bank_a = (uint8_t)(cart->mmc5_prg_bank[2] & 0xfeu);
        uint8_t bank_b = (uint8_t)(cart->mmc5_prg_bank[4] & 0xfeu);
        cart->prg_banks_8k[0] = mmc5_prg_rom_bank(cart, bank_a);
        cart->prg_banks_8k[1] = mmc5_prg_rom_bank(cart, (uint8_t)(bank_a + 1u));
        cart->prg_banks_8k[2] = mmc5_prg_rom_bank(cart, bank_b);
        cart->prg_banks_8k[3] = mmc5_prg_rom_bank(cart, (uint8_t)(bank_b + 1u));
        break;
    }
    case 2: {
        uint8_t bank_a = (uint8_t)(cart->mmc5_prg_bank[2] & 0xfeu);
        cart->prg_banks_8k[0] = mmc5_prg_rom_bank(cart, bank_a);
        cart->prg_banks_8k[1] = mmc5_prg_rom_bank(cart, (uint8_t)(bank_a + 1u));
        cart->prg_banks_8k[2] = mmc5_prg_rom_bank(cart, cart->mmc5_prg_bank[3]);
        cart->prg_banks_8k[3] = mmc5_prg_rom_bank(cart, cart->mmc5_prg_bank[4]);
        break;
    }
    default:
        cart->prg_banks_8k[0] = mmc5_prg_rom_bank(cart, cart->mmc5_prg_bank[1]);
        cart->prg_banks_8k[1] = mmc5_prg_rom_bank(cart, cart->mmc5_prg_bank[2]);
        cart->prg_banks_8k[2] = mmc5_prg_rom_bank(cart, cart->mmc5_prg_bank[3]);
        cart->prg_banks_8k[3] = mmc5_prg_rom_bank(cart, cart->mmc5_prg_bank[4]);
        break;
    }

    if ((cart->mmc5_prg_bank[4] & 0x80u) == 0) {
        cart->prg_banks_8k[3] = mmc5_prg_rom_bank(cart, last);
    }
}

void mmc5_cart_init(NesCartridge *cart) {
    uint8_t last = (uint8_t)(cart->prg_rom_size / 0x2000u - 1u);

    cart->mmc5_prg_mode = 3;
    cart->mmc5_chr_mode = 3;
    cart->mmc5_prg_ram_protect1 = 0;
    cart->mmc5_prg_ram_protect2 = 0;
    for (int i = 0; i < 5; ++i) {
        cart->mmc5_prg_bank[i] = (uint8_t)(0x80u | (last - (4 - i)));
    }
    for (int i = 0; i < 8; ++i) {
        cart->mmc5_chr_sprite_bank[i] = (uint8_t)i;
    }
    for (int i = 0; i < 4; ++i) {
        cart->mmc5_chr_bg_bank[i] = (uint8_t)i;
    }
    cart->mmc5_nametable_mapping = 0x44u;
    cart->mmc5_fill_tile = 0;
    cart->mmc5_fill_attr = 0;
    cart->mmc5_irq_scanline = 0;
    cart->mmc5_irq_status = 0;
    cart->mmc5_irq_enabled = false;
    cart->mmc5_scanline = 0;
    cart->mmc5_mul_a = 0;
    cart->mmc5_mul_b = 0;
    cart->irq_pending = false;
    mmc5_update_prg_banks(cart);
}

void mmc5_rebase_banks(NesCartridge *cart, const uint8_t *old_prg_base) {
    for (int i = 0; i < 4; ++i) {
        size_t off = (size_t)(cart->prg_banks_8k[i] - old_prg_base);
        cart->prg_banks_8k[i] = cart->prg_rom + off;
    }
}

uint8_t mmc5_cpu_read(NesCartridge *cart, uint16_t addr) {
    uint16_t product = (uint16_t)cart->mmc5_mul_a * (uint16_t)cart->mmc5_mul_b;

    if (addr >= 0x5c00u && addr < 0x6000u) {
        return cart->mmc5_exram[addr & 0x03ffu];
    }

    switch (addr) {
    case 0x5204u: {
        uint8_t status = cart->mmc5_irq_status;
        cart->mmc5_irq_status &= (uint8_t)~0x80u;
        cart->irq_pending = false;
        return status;
    }
    case 0x5205u:
        return (uint8_t)product;
    case 0x5206u:
        return (uint8_t)(product >> 8);
    default:
        return 0;
    }
}

void mmc5_cpu_write(NesCartridge *cart, uint16_t addr, uint8_t value) {
    if (addr >= 0x5c00u && addr < 0x6000u) {
        cart->mmc5_exram[addr & 0x03ffu] = value;
        return;
    }

    switch (addr) {
    case 0x5100u:
        cart->mmc5_prg_mode = value & 0x03u;
        mmc5_update_prg_banks(cart);
        break;
    case 0x5101u:
        cart->mmc5_chr_mode = value & 0x03u;
        break;
    case 0x5102u:
        cart->mmc5_prg_ram_protect1 = value & 0x03u;
        break;
    case 0x5103u:
        cart->mmc5_prg_ram_protect2 = value & 0x03u;
        break;
    case 0x5105u:
        cart->mmc5_nametable_mapping = value;
        break;
    case 0x5106u:
        cart->mmc5_fill_tile = value;
        break;
    case 0x5107u:
        cart->mmc5_fill_attr = value & 0x03u;
        break;
    case 0x5113u:
    case 0x5114u:
    case 0x5115u:
    case 0x5116u:
    case 0x5117u:
        cart->mmc5_prg_bank[addr - 0x5113u] = value;
        mmc5_update_prg_banks(cart);
        break;
    case 0x5120u:
    case 0x5121u:
    case 0x5122u:
    case 0x5123u:
    case 0x5124u:
    case 0x5125u:
    case 0x5126u:
    case 0x5127u:
        cart->mmc5_chr_sprite_bank[addr - 0x5120u] = value;
        break;
    case 0x5128u:
    case 0x5129u:
    case 0x512au:
    case 0x512bu:
        cart->mmc5_chr_bg_bank[addr - 0x5128u] = value;
        break;
    case 0x5203u:
        cart->mmc5_irq_scanline = value;
        break;
    case 0x5204u:
        cart->mmc5_irq_enabled = (value & 0x80u) != 0;
        if (!cart->mmc5_irq_enabled) {
            cart->irq_pending = false;
        }
        break;
    case 0x5205u:
        cart->mmc5_mul_a = value;
        break;
    case 0x5206u:
        cart->mmc5_mul_b = value;
        break;
    default:
        break;
    }
}

void mmc5_scanline_tick(NesCartridge *cart, bool rendering_enabled) {
    if (!rendering_enabled) {
        cart->mmc5_scanline = 0;
        cart->mmc5_irq_status &= (uint8_t)~0x40u;
        return;
    }

    cart->mmc5_irq_status |= 0x40u;
    ++cart->mmc5_scanline;
    if (cart->mmc5_scanline == cart->mmc5_irq_scanline) {
        cart->mmc5_irq_status |= 0x80u;
        if (cart->mmc5_irq_enabled) {
            cart->irq_pending = true;
        }
    }
}

static uint8_t mmc5_fill_value(const NesCartridge *cart, uint16_t inner) {
    if (inner < 0x03c0u) {
        return cart->mmc5_fill_tile;
    }
    return (uint8_t)(cart->mmc5_fill_attr * 0x55u);
}

uint8_t mmc5_nametable_read(const NesCartridge *cart, const uint8_t *ciram, uint16_t addr) {
    uint16_t offset = (uint16_t)(addr - 0x2000u) & 0x0fffu;
    uint16_t table = offset >> 10;
    uint16_t inner = offset & 0x03ffu;
    uint8_t target = (uint8_t)((cart->mmc5_nametable_mapping >> (table * 2u)) & 0x03u);

    if (target < 2u) {
        return ciram[(uint16_t)target * 0x0400u + inner];
    }
    if (target == 2u) {
        return cart->mmc5_exram[inner];
    }
    return mmc5_fill_value(cart, inner);
}

void mmc5_nametable_write(NesCartridge *cart, uint8_t *ciram, uint16_t addr, uint8_t value) {
    uint16_t offset = (uint16_t)(addr - 0x2000u) & 0x0fffu;
    uint16_t table = offset >> 10;
    uint16_t inner = offset & 0x03ffu;
    uint8_t target = (uint8_t)((cart->mmc5_nametable_mapping >> (table * 2u)) & 0x03u);

    if (target < 2u) {
        ciram[(uint16_t)target * 0x0400u + inner] = value;
    } else if (target == 2u) {
        cart->mmc5_exram[inner] = value;
    }
}
