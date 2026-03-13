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
    PPU_STATUS_SPRITE0_HIT = 0x40,
    PPU_STATUS_VBLANK = 0x80,
    PPU_MASK_SHOW_BG_LEFT = 0x02,
    PPU_MASK_SHOW_SPRITES_LEFT = 0x04,
    PPU_MASK_SHOW_BG = 0x08,
    PPU_MASK_SHOW_SPRITES = 0x10,
};

typedef struct {
    uint8_t color;
    bool opaque;
} PpuPixelSample;

typedef struct {
    uint8_t oam_index;
    uint8_t y;
    uint8_t tile;
    uint8_t attributes;
    uint8_t x;
} PpuScanlineSprite;

typedef struct {
    uint8_t color;
    bool opaque;
    bool sprite0;
    bool behind_background;
} PpuSpritePixelSample;

enum { PPU_MAX_SCANLINE_SPRITES = 8 };

static uint64_t ppu_hash_framebuffer(const NesFrameBuffer *frame_buffer) {
    uint64_t hash = 1469598103934665603ull;

    for (uint32_t i = 0; i < NES_FRAME_WIDTH * NES_FRAME_HEIGHT; ++i) {
        hash ^= frame_buffer->pixels[i];
        hash *= 1099511628211ull;
    }

    return hash;
}

static void ppu_finalize_frame(Ppu *ppu) {
    uint32_t nonzero_pixels = 0;
    uint32_t sprite_pixels = 0;

    for (uint32_t i = 0; i < NES_FRAME_WIDTH * NES_FRAME_HEIGHT; ++i) {
        if (ppu->frame_buffer.pixels[i] != 0) {
            ++nonzero_pixels;
        }
    }

    ++ppu->completed_frame_count;
    ppu->completed_frame_ready = true;
    ppu->last_completed_nonzero_pixels = nonzero_pixels;
    ppu->last_completed_frame_hash = ppu_hash_framebuffer(&ppu->frame_buffer);
    sprite_pixels = (uint32_t)ppu->sprite_composited_pixel_count;
    ppu->last_completed_sprite_pixels = sprite_pixels;
    if (nonzero_pixels != 0 && ppu->first_nonblank_frame_index == 0) {
        ppu->first_nonblank_frame_index = ppu->completed_frame_count;
        ppu->first_nonblank_frame_hash = ppu->last_completed_frame_hash;
    }
    if (sprite_pixels != 0) {
        ++ppu->frames_with_sprite_pixels;
        if (ppu->first_frame_with_sprite_pixels == 0) {
            ppu->first_frame_with_sprite_pixels = ppu->completed_frame_count;
        }
    }
}

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

static PpuPixelSample ppu_background_pixel(const Ppu *ppu, const NesCartridge *cartridge, int x, int y) {
    PpuPixelSample sample;
    int scrolled_x = (x + ppu->scroll_x) & 0x1ff;
    int scrolled_y = (y + ppu->scroll_y) & 0x1ff;
    uint16_t base_nametable = (uint16_t)(ppu->ctrl & 0x03u);
    uint16_t name_table_x = (uint16_t)((scrolled_x >> 8) & 0x01u);
    uint16_t name_table_y = (uint16_t)(scrolled_y >= NES_FRAME_HEIGHT ? 0x02u : 0x00u);
    uint16_t effective_nametable = (uint16_t)(base_nametable ^ name_table_x ^ name_table_y);
    uint16_t name_table = (uint16_t)(effective_nametable * 0x0400u);
    uint16_t coarse_x = (uint16_t)((scrolled_x & 0xff) >> 3);
    uint16_t coarse_y = (uint16_t)(((uint32_t)scrolled_y % NES_FRAME_HEIGHT) >> 3);
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

    sample.opaque = (palette_index & 0x03u) != 0;
    if (!sample.opaque) {
        sample.color = ppu->palette[0];
        return sample;
    }
    sample.color = ppu->palette[palette_index & 0x1fu];
    return sample;
}

static int ppu_sprite_height(const Ppu *ppu) {
    return (ppu->ctrl & 0x20u) ? 16 : 8;
}

static bool ppu_sprite_intersects_scanline(const PpuScanlineSprite *sprite, int y, int sprite_height) {
    int sprite_top = (int)sprite->y + 1;
    return y >= sprite_top && y < sprite_top + sprite_height;
}

static uint8_t ppu_collect_scanline_sprites(const Ppu *ppu, int y, PpuScanlineSprite *sprites) {
    uint8_t count = 0;
    int sprite_height = ppu_sprite_height(ppu);

    if ((ppu->mask & PPU_MASK_SHOW_SPRITES) == 0) {
        return 0;
    }

    for (uint8_t sprite_index = 0; sprite_index < 64; ++sprite_index) {
        PpuScanlineSprite sprite;
        uint16_t base = (uint16_t)sprite_index * 4u;

        sprite.oam_index = sprite_index;
        sprite.y = ppu->oam[base + 0];
        sprite.tile = ppu->oam[base + 1];
        sprite.attributes = ppu->oam[base + 2];
        sprite.x = ppu->oam[base + 3];

        if (!ppu_sprite_intersects_scanline(&sprite, y, sprite_height)) {
            continue;
        }

        if (count < PPU_MAX_SCANLINE_SPRITES) {
            sprites[count++] = sprite;
        } else {
            break;
        }
    }

    return count;
}

static PpuSpritePixelSample ppu_sample_sprite_pixel(
    const Ppu *ppu,
    const NesCartridge *cartridge,
    const PpuScanlineSprite *sprite,
    int x,
    int y
) {
    PpuSpritePixelSample sample = { 0, false, false, false };
    int sprite_height = ppu_sprite_height(ppu);
    int local_x;
    int local_y;
    uint16_t pattern_addr;
    uint8_t low;
    uint8_t high;
    uint8_t bit;
    uint8_t color_bits;

    if (x < sprite->x || x >= sprite->x + 8) {
        return sample;
    }
    if (x < 8 && ((ppu->mask & PPU_MASK_SHOW_SPRITES_LEFT) == 0)) {
        return sample;
    }

    local_x = x - sprite->x;
    local_y = y - ((int)sprite->y + 1);
    if (local_y < 0 || local_y >= sprite_height) {
        return sample;
    }

    if (sprite->attributes & 0x40u) {
        local_x = 7 - local_x;
    }
    if (sprite->attributes & 0x80u) {
        local_y = sprite_height - 1 - local_y;
    }

    if (sprite_height == 16) {
        uint8_t tile_row = (uint8_t)(local_y >> 3);
        uint8_t row_in_tile = (uint8_t)(local_y & 0x07);
        uint16_t pattern_base = (sprite->tile & 0x01u) ? 0x1000u : 0x0000u;
        uint8_t tile_number = (uint8_t)((sprite->tile & 0xfeu) + tile_row);
        pattern_addr = (uint16_t)(pattern_base + tile_number * 16u + row_in_tile);
    } else {
        uint16_t pattern_base = (ppu->ctrl & 0x08u) ? 0x1000u : 0x0000u;
        pattern_addr = (uint16_t)(pattern_base + sprite->tile * 16u + local_y);
    }

    low = nrom_ppu_read(cartridge, pattern_addr);
    high = nrom_ppu_read(cartridge, (uint16_t)(pattern_addr + 8u));
    bit = (uint8_t)(7 - local_x);
    color_bits = (uint8_t)((((high >> bit) & 0x01u) << 1) | ((low >> bit) & 0x01u));
    if (color_bits == 0) {
        return sample;
    }

    sample.opaque = true;
    sample.color = ppu->palette[(0x10u + ((sprite->attributes & 0x03u) << 2) + color_bits) & 0x1fu];
    sample.sprite0 = sprite->oam_index == 0;
    sample.behind_background = (sprite->attributes & 0x20u) != 0;
    return sample;
}

static PpuSpritePixelSample ppu_visible_sprite_pixel(
    const Ppu *ppu,
    const NesCartridge *cartridge,
    const PpuScanlineSprite *sprites,
    uint8_t sprite_count,
    int x,
    int y
) {
    for (uint8_t i = 0; i < sprite_count; ++i) {
        PpuSpritePixelSample sample = ppu_sample_sprite_pixel(ppu, cartridge, &sprites[i], x, y);
        if (sample.opaque) {
            return sample;
        }
    }

    return (PpuSpritePixelSample){ 0, false, false, false };
}

static void ppu_note_sprite0_hit(Ppu *ppu, int x, int y) {
    if (ppu->status & PPU_STATUS_SPRITE0_HIT) {
        return;
    }

    ppu->status |= PPU_STATUS_SPRITE0_HIT;
    ++ppu->sprite0_hit_count;
    if (!ppu->sprite0_hit_ever) {
        ppu->sprite0_hit_ever = true;
        ppu->first_sprite0_hit_frame = ppu->frame_count;
        ppu->first_sprite0_hit_scanline = y;
        ppu->first_sprite0_hit_x = x;
    }
}

static void ppu_note_sprite0_opaque(Ppu *ppu, int x, int y) {
    ++ppu->sprite0_opaque_pixel_count;
    if (ppu->first_sprite0_opaque_scanline < 0) {
        ppu->first_sprite0_opaque_frame = ppu->frame_count;
        ppu->first_sprite0_opaque_scanline = y;
        ppu->first_sprite0_opaque_x = x;
    }
}

static void ppu_render_scanline(Ppu *ppu, NesCartridge *cartridge, int y) {
    uint8_t *dst = nes_framebuffer_scanline(&ppu->frame_buffer, (uint16_t)y);
    PpuScanlineSprite sprites[PPU_MAX_SCANLINE_SPRITES];
    uint8_t sprite_count = ppu_collect_scanline_sprites(ppu, y, sprites);

    if (sprite_count > ppu->max_scanline_sprite_count) {
        ppu->max_scanline_sprite_count = sprite_count;
    }

    for (int x = 0; x < NES_FRAME_WIDTH; ++x) {
        PpuPixelSample background = { ppu->palette[0], false };
        PpuSpritePixelSample sprite = { 0, false, false, false };
        uint8_t color;
        bool use_sprite = false;

        if ((ppu->mask & PPU_MASK_SHOW_BG) != 0 && (x >= 8 || (ppu->mask & PPU_MASK_SHOW_BG_LEFT) != 0)) {
            background = ppu_background_pixel(ppu, cartridge, x, y);
        }
        sprite = ppu_visible_sprite_pixel(ppu, cartridge, sprites, sprite_count, x, y);
        if (sprite.opaque && sprite.sprite0) {
            ppu_note_sprite0_opaque(ppu, x, y);
        }

        if (x < 255 && background.opaque && sprite.opaque && sprite.sprite0) {
            ++ppu->sprite0_background_overlap_count;
            ppu_note_sprite0_hit(ppu, x, y);
        }

        color = background.color;
        if (sprite.opaque && (!sprite.behind_background || !background.opaque)) {
            color = sprite.color;
            use_sprite = true;
        }

        if (use_sprite) {
            ++ppu->sprite_composited_pixel_count;
        }

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
    ppu->completed_frame_count = 0;
    ppu->completed_frame_ready = false;
    ppu->last_completed_frame_hash = 0;
    ppu->last_completed_nonzero_pixels = 0;
    ppu->first_nonblank_frame_index = 0;
    ppu->first_nonblank_frame_hash = 0;
    ppu->sprite0_hit_ever = false;
    ppu->sprite0_hit_count = 0;
    ppu->sprite0_opaque_pixel_count = 0;
    ppu->sprite0_background_overlap_count = 0;
    ppu->first_sprite0_hit_frame = 0;
    ppu->first_sprite0_hit_scanline = -1;
    ppu->first_sprite0_hit_x = -1;
    ppu->first_sprite0_opaque_frame = 0;
    ppu->first_sprite0_opaque_scanline = -1;
    ppu->first_sprite0_opaque_x = -1;
    ppu->sprite_composited_pixel_count = 0;
    ppu->frames_with_sprite_pixels = 0;
    ppu->last_completed_sprite_pixels = 0;
    ppu->first_frame_with_sprite_pixels = 0;
    ppu->max_scanline_sprite_count = 0;
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
            ppu_finalize_frame(ppu);
            ppu->status |= PPU_STATUS_VBLANK;
            ppu->frame_ready = true;
            if (ppu->ctrl & 0x80u) {
                ppu->nmi_pending = true;
            }
        } else if (ppu->scanline == 261 && ppu->cycle == 1) {
            ppu->status &= (uint8_t)~(PPU_STATUS_VBLANK | PPU_STATUS_SPRITE0_HIT);
            ppu->frame_ready = false;
            ppu->completed_frame_ready = false;
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
                ppu->sprite_composited_pixel_count = 0;
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
