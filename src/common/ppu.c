#include "ppu.h"

#include "nrom.h"

#include <string.h>

enum {
    PPU_REG_PPUCTRL = 0,
    PPU_REG_PPUMASK = 1,
    PPU_REG_PPUSTATUS = 2,
    PPU_REG_OAMADDR = 3,
    PPU_REG_OAMDATA = 4,
    PPU_REG_PPUSCROLL = 5,
    PPU_REG_PPUADDR = 6,
    PPU_REG_PPUDATA = 7,
    PPU_STATUS_VBLANK = 0x80,
};

static uint16_t ppu_palette_index(uint16_t addr) {
    uint16_t index = addr & 0x1fu;
    if (index == 0x10u) index = 0x00u;
    if (index == 0x14u) index = 0x04u;
    if (index == 0x18u) index = 0x08u;
    if (index == 0x1cu) index = 0x0cu;
    return index;
}

static uint16_t ppu_nametable_index(const NesCartridge *cartridge, uint16_t addr) {
    uint16_t offset = (uint16_t)(addr - 0x2000u) & 0x0fffu;
    uint16_t table = offset / 0x0400u;
    uint16_t inner = offset & 0x03ffu;
    uint16_t physical;

    if (cartridge->mirror_mode == NES_MIRROR_VERTICAL) {
        physical = table & 0x01u;
    } else {
        physical = (uint16_t)((table >> 1) & 0x01u);
    }

    return (uint16_t)(physical * 0x0400u + inner);
}

static uint8_t ppu_vram_read(Ppu *ppu, NesCartridge *cartridge, uint16_t addr) {
    uint16_t masked = addr & 0x3fffu;

    if (masked < 0x2000u) {
        return nrom_ppu_read(cartridge, masked);
    }
    if (masked < 0x3f00u) {
        return ppu->nametables[ppu_nametable_index(cartridge, masked)];
    }
    return ppu->palette[ppu_palette_index(masked)];
}

static void ppu_vram_write(Ppu *ppu, NesCartridge *cartridge, uint16_t addr, uint8_t value) {
    uint16_t masked = addr & 0x3fffu;

    if (masked < 0x2000u) {
        nrom_ppu_write(cartridge, masked, value);
        return;
    }
    if (masked < 0x3f00u) {
        ppu->nametables[ppu_nametable_index(cartridge, masked)] = value;
        return;
    }
    ppu->palette[ppu_palette_index(masked)] = value;
}

static uint8_t ppu_background_pixel(const Ppu *ppu, const NesCartridge *cartridge, int x, int y) {
    int scrolled_x = (x + ppu->scroll_x) & 0x1ff;
    int scrolled_y = (y + ppu->scroll_y) & 0x1ff;
    uint16_t name_table = (uint16_t)((ppu->ctrl & 0x03u) * 0x0400u);
    uint16_t coarse_x = (uint16_t)((scrolled_x >> 3) & 0x1fu);
    uint16_t coarse_y = (uint16_t)((scrolled_y >> 3) & 0x1du);
    uint16_t tile_index_addr = (uint16_t)(0x2000u + name_table + coarse_y * 32u + coarse_x);
    uint16_t attr_addr = (uint16_t)(0x23c0u + name_table + ((coarse_y >> 2) * 8u) + (coarse_x >> 2));
    uint8_t tile = ppu->nametables[ppu_nametable_index(cartridge, tile_index_addr)];
    uint8_t attr = ppu->nametables[ppu_nametable_index(cartridge, attr_addr)];
    uint16_t pattern_base = (ppu->ctrl & 0x10u) ? 0x1000u : 0x0000u;
    uint16_t row = (uint16_t)(scrolled_y & 0x07);
    uint16_t pattern_addr = (uint16_t)(pattern_base + tile * 16u + row);
    uint8_t low = nrom_ppu_read(cartridge, pattern_addr);
    uint8_t high = nrom_ppu_read(cartridge, (uint16_t)(pattern_addr + 8u));
    uint8_t bit = (uint8_t)(7 - (scrolled_x & 0x07));
    uint8_t color_low = (low >> bit) & 0x01u;
    uint8_t color_high = (high >> bit) & 0x01u;
    uint8_t palette_select = (attr >> ((((coarse_y & 0x02u) << 1) | (coarse_x & 0x02u)))) & 0x03u;
    uint8_t palette_index = (uint8_t)((palette_select << 2) | (color_high << 1) | color_low);

    if ((palette_index & 0x03u) == 0) {
        return ppu->palette[0];
    }
    return ppu->palette[palette_index & 0x1fu];
}

static void ppu_render_scanline(Ppu *ppu, NesCartridge *cartridge, int y) {
    uint8_t *dst = nes_framebuffer_scanline(&ppu->frame_buffer, (uint16_t)y);

    for (int x = 0; x < NES_FRAME_WIDTH; ++x) {
        uint8_t color = ppu_background_pixel(ppu, cartridge, x, y);
        dst[x] = color;
        ppu->scanline_buffer.pixels[x] = color;
    }

    ppu->scanline_buffer.y = (uint16_t)y;
    ppu->scanline_buffer.frame_index = ppu->frame_count;
    ppu->scanline_buffer.ready = true;
    ppu->scanline_ready = true;
}

void ppu_init(Ppu *ppu) {
    memset(ppu, 0, sizeof(*ppu));
    ppu->scanline = 261;
}

void ppu_reset(Ppu *ppu) {
    ppu->ctrl = 0;
    ppu->mask = 0;
    ppu->status = 0;
    ppu->oam_addr = 0;
    ppu->read_buffer = 0;
    ppu->vram_addr = 0;
    ppu->temp_addr = 0;
    ppu->fine_x = 0;
    ppu->write_toggle = false;
    ppu->scroll_x = 0;
    ppu->scroll_y = 0;
    ppu->scanline = 261;
    ppu->cycle = 0;
    ppu->frame_count = 0;
    ppu->frame_ready = false;
    ppu->scanline_ready = false;
    ppu->nmi_pending = false;
    ppu->frame_buffer.frame_index = 0;
    ppu->scanline_buffer.frame_index = 0;
    ppu->scanline_buffer.y = 0;
    ppu->scanline_buffer.ready = false;
    memset(ppu->oam, 0, sizeof(ppu->oam));
    memset(ppu->nametables, 0, sizeof(ppu->nametables));
    memset(ppu->palette, 0, sizeof(ppu->palette));
    memset(ppu->frame_buffer.pixels, 0, sizeof(ppu->frame_buffer.pixels));
    memset(ppu->scanline_buffer.pixels, 0, sizeof(ppu->scanline_buffer.pixels));
}

void ppu_step_cycles(Ppu *ppu, NesCartridge *cartridge, uint32_t cycles) {
    for (uint32_t i = 0; i < cycles; ++i) {
        ++ppu->cycle;

        if (ppu->scanline == 241 && ppu->cycle == 1) {
            ppu->status |= PPU_STATUS_VBLANK;
            ppu->frame_ready = true;
            if (ppu->ctrl & 0x80u) {
                ppu->nmi_pending = true;
            }
        } else if (ppu->scanline == 261 && ppu->cycle == 1) {
            ppu->status &= (uint8_t)~PPU_STATUS_VBLANK;
            ppu->frame_ready = false;
            ppu->scanline_ready = false;
            ppu->scanline_buffer.ready = false;
        }

        if (ppu->cycle > 340) {
            ppu->cycle = 0;

            if (ppu->scanline >= 0 && ppu->scanline < NES_FRAME_HEIGHT) {
                ppu_render_scanline(ppu, cartridge, ppu->scanline);
            }

            ++ppu->scanline;
            if (ppu->scanline > 261) {
                ppu->scanline = 0;
                ++ppu->frame_count;
                ppu->frame_buffer.frame_index = ppu->frame_count;
            }
        }
    }
}

uint8_t ppu_cpu_read(Ppu *ppu, NesCartridge *cartridge, uint16_t addr) {
    uint8_t reg = (uint8_t)(addr & 0x07u);

    switch (reg) {
    case PPU_REG_PPUSTATUS: {
        uint8_t value = (uint8_t)((ppu->status & 0xe0u) | (ppu->read_buffer & 0x1fu));
        ppu->status &= (uint8_t)~PPU_STATUS_VBLANK;
        ppu->write_toggle = false;
        return value;
    }
    case PPU_REG_OAMDATA:
        return ppu->oam[ppu->oam_addr];
    case PPU_REG_PPUDATA: {
        uint16_t addr_before = ppu->vram_addr;
        uint8_t value = ppu_vram_read(ppu, cartridge, addr_before);

        ppu->vram_addr += (ppu->ctrl & 0x04u) ? 32u : 1u;
        if ((addr_before & 0x3fffu) >= 0x3f00u) {
            ppu->read_buffer = ppu_vram_read(ppu, cartridge, (uint16_t)(addr_before - 0x1000u));
            return value;
        }

        {
            uint8_t buffered = ppu->read_buffer;
            ppu->read_buffer = value;
            return buffered;
        }
    }
    default:
        return 0;
    }
}

void ppu_cpu_write(Ppu *ppu, NesCartridge *cartridge, uint16_t addr, uint8_t value) {
    uint8_t reg = (uint8_t)(addr & 0x07u);

    switch (reg) {
    case PPU_REG_PPUCTRL:
        ppu->ctrl = value;
        break;
    case PPU_REG_PPUMASK:
        ppu->mask = value;
        break;
    case PPU_REG_OAMADDR:
        ppu->oam_addr = value;
        break;
    case PPU_REG_OAMDATA:
        ppu->oam[ppu->oam_addr++] = value;
        break;
    case PPU_REG_PPUSCROLL:
        if (!ppu->write_toggle) {
            ppu->scroll_x = value;
        } else {
            ppu->scroll_y = value;
        }
        ppu->write_toggle = !ppu->write_toggle;
        break;
    case PPU_REG_PPUADDR:
        if (!ppu->write_toggle) {
            ppu->temp_addr = (uint16_t)((value & 0x3fu) << 8);
        } else {
            ppu->temp_addr = (uint16_t)((ppu->temp_addr & 0xff00u) | value);
            ppu->vram_addr = ppu->temp_addr;
        }
        ppu->write_toggle = !ppu->write_toggle;
        break;
    case PPU_REG_PPUDATA:
        ppu_vram_write(ppu, cartridge, ppu->vram_addr, value);
        ppu->vram_addr += (ppu->ctrl & 0x04u) ? 32u : 1u;
        break;
    default:
        break;
    }
}

const NesFrameBuffer *ppu_framebuffer(const Ppu *ppu) {
    return &ppu->frame_buffer;
}

const NesScanline *ppu_scanline(const Ppu *ppu) {
    return &ppu->scanline_buffer;
}
