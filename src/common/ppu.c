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

static bool ppu_sprite0_diag_collect_this_frame(const Ppu *ppu) {
    return (ppu->sprite0_diag.enabled &&
            ppu->frame_count >= ppu->sprite0_diag.frame_start &&
            ppu->frame_count <= ppu->sprite0_diag.frame_end) ||
           !ppu->sprite0_diag.first_hit_frame_valid;
}

static void ppu_sprite0_diag_add_example(PpuSprite0FrameDiag *diag, uint8_t reason, int x, int y) {
    if (!diag->valid || diag->example_count >= PPU_SPRITE0_DIAG_MAX_EXAMPLES) {
        return;
    }

    diag->examples[diag->example_count].reason = reason;
    diag->examples[diag->example_count].x = (uint16_t)x;
    diag->examples[diag->example_count].y = (uint16_t)y;
    ++diag->example_count;
}

static void ppu_sprite0_diag_begin_frame(Ppu *ppu) {
    PpuSprite0FrameDiag *diag = &ppu->sprite0_diag.current_frame;

    memset(diag, 0, sizeof(*diag));
    diag->first_hit_x = -1;
    diag->first_hit_y = -1;
    diag->first_raw_overlap_x = -1;
    diag->first_raw_overlap_y = -1;
    diag->first_effective_overlap_x = -1;
    diag->first_effective_overlap_y = -1;

    if (!ppu_sprite0_diag_collect_this_frame(ppu)) {
        return;
    }

    diag->valid = true;
    diag->completed = false;
    diag->render_frame_index = ppu->frame_count;
    diag->completed_frame_index = ppu->completed_frame_count;
    diag->sprite_y = ppu->oam[0];
    diag->sprite_tile = ppu->oam[1];
    diag->sprite_attributes = ppu->oam[2];
    diag->sprite_x = ppu->oam[3];
    diag->ctrl = ppu->ctrl;
    diag->mask = ppu->mask;
    diag->status = ppu->status;
    diag->scroll_x = ppu->scroll_x;
    diag->scroll_y = ppu->scroll_y;
    diag->render_scroll_x = ppu->render_scroll_x;
    diag->render_scroll_y = ppu->render_scroll_y;
    diag->render_base_nametable = ppu->render_base_nametable;
    diag->vram_addr = ppu->vram_addr;
    diag->temp_addr = ppu->temp_addr;
    diag->fine_x = ppu->fine_x;
    diag->show_bg = (ppu->mask & PPU_MASK_SHOW_BG) != 0;
    diag->show_sprites = (ppu->mask & PPU_MASK_SHOW_SPRITES) != 0;
    diag->show_bg_left = (ppu->mask & PPU_MASK_SHOW_BG_LEFT) != 0;
    diag->show_sprites_left = (ppu->mask & PPU_MASK_SHOW_SPRITES_LEFT) != 0;
    diag->last_sprite0_oam_update_render_frame = ppu->sprite0_diag.last_sprite0_oam_update_render_frame;
    diag->last_sprite0_oam_update_scanline = ppu->sprite0_diag.last_sprite0_oam_update_scanline;
    diag->last_sprite0_oam_update_cycle = ppu->sprite0_diag.last_sprite0_oam_update_cycle;
    diag->last_sprite0_oam_update_source = ppu->sprite0_diag.last_sprite0_oam_update_source;
}

static void ppu_sprite0_diag_commit_frame(Ppu *ppu) {
    PpuSprite0FrameDiag *diag = &ppu->sprite0_diag.current_frame;

    if (!diag->valid) {
        return;
    }

    diag->completed = true;
    diag->completed_frame_index = ppu->completed_frame_count;
    diag->status = ppu->status;
    diag->ctrl = ppu->ctrl;
    diag->mask = ppu->mask;
    diag->scroll_x = ppu->scroll_x;
    diag->scroll_y = ppu->scroll_y;
    diag->render_scroll_x = ppu->render_scroll_x;
    diag->render_scroll_y = ppu->render_scroll_y;
    diag->render_base_nametable = ppu->render_base_nametable;
    diag->vram_addr = ppu->vram_addr;
    diag->temp_addr = ppu->temp_addr;
    diag->fine_x = ppu->fine_x;

    if (diag->sprite0_hit_set_this_frame && !ppu->sprite0_diag.first_hit_frame_valid) {
        ppu->sprite0_diag.first_hit_frame = *diag;
        ppu->sprite0_diag.first_hit_frame_valid = true;
    }

    if (ppu->sprite0_diag.enabled &&
        diag->render_frame_index >= ppu->sprite0_diag.frame_start &&
        diag->render_frame_index <= ppu->sprite0_diag.frame_end &&
        ppu->sprite0_diag.frame_count < PPU_SPRITE0_DIAG_MAX_FRAMES) {
        ppu->sprite0_diag.frames[ppu->sprite0_diag.frame_count++] = *diag;
    }
}

static void ppu_record_sprite0_status_set(Ppu *ppu, int x, int y) {
    PpuSprite0FrameDiag *diag = &ppu->sprite0_diag.current_frame;

    ++ppu->sprite0_diag.total_status_set_count;
    ppu->sprite0_diag.last_status_set_render_frame = ppu->frame_count;
    ppu->sprite0_diag.last_status_set_scanline = ppu->scanline;
    ppu->sprite0_diag.last_status_set_cycle = ppu->cycle;

    if (!diag->valid) {
        return;
    }
    ++diag->sprite0_hit_set_count;
    diag->sprite0_hit_set_this_frame = true;
    if (diag->first_hit_x < 0) {
        diag->first_hit_x = x;
        diag->first_hit_y = y;
    }
}

static void ppu_record_sprite0_status_clear(Ppu *ppu, bool expected) {
    ++ppu->sprite0_diag.total_status_clear_count;
    ppu->sprite0_diag.last_status_clear_render_frame = ppu->frame_count;
    ppu->sprite0_diag.last_status_clear_scanline = ppu->scanline;
    ppu->sprite0_diag.last_status_clear_cycle = ppu->cycle;
    if (!expected) {
        ++ppu->sprite0_diag.suspicious_status_clear_count;
    }
}

static void ppu_latch_render_state(Ppu *ppu) {
    uint16_t v = ppu->vram_addr;
    uint16_t coarse_x = v & 0x001fu;
    uint16_t coarse_y = (v >> 5) & 0x001fu;
    uint16_t fine_y = (v >> 12) & 0x0007u;

    ppu->render_vram_addr = v;
    ppu->render_scroll_x = (uint8_t)(((coarse_x << 3) | ppu->fine_x) & 0xffu);
    ppu->render_scroll_y = (uint8_t)(((coarse_y << 3) | fine_y) & 0xffu);
    ppu->render_base_nametable = (uint8_t)((v >> 10) & 0x03u);
}

static bool ppu_rendering_enabled(const Ppu *ppu) {
    return (ppu->mask & (PPU_MASK_SHOW_BG | PPU_MASK_SHOW_SPRITES)) != 0;
}

static void ppu_copy_horizontal_bits_from_temp(Ppu *ppu) {
    ppu->vram_addr = (uint16_t)((ppu->vram_addr & (uint16_t)~0x041fu) | (ppu->temp_addr & 0x041fu));
}

static void ppu_copy_vertical_bits_from_temp(Ppu *ppu) {
    ppu->vram_addr = (uint16_t)((ppu->vram_addr & (uint16_t)~0x7be0u) | (ppu->temp_addr & 0x7be0u));
}

static void ppu_increment_vertical_v(Ppu *ppu) {
    if ((ppu->vram_addr & 0x7000u) != 0x7000u) {
        ppu->vram_addr += 0x1000u;
        return;
    }

    ppu->vram_addr &= (uint16_t)~0x7000u;
    {
        uint16_t coarse_y = (ppu->vram_addr & 0x03e0u) >> 5;

        if (coarse_y == 29u) {
            coarse_y = 0u;
            ppu->vram_addr ^= 0x0800u;
        } else if (coarse_y == 31u) {
            coarse_y = 0u;
        } else {
            ++coarse_y;
        }

        ppu->vram_addr = (uint16_t)((ppu->vram_addr & (uint16_t)~0x03e0u) | (coarse_y << 5));
    }
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
    uint16_t base_v = ppu->render_vram_addr;
    uint16_t total_x = (uint16_t)(ppu->fine_x + x);
    uint16_t coarse_x = (uint16_t)((base_v & 0x001fu) + (total_x >> 3));
    uint16_t coarse_y = (base_v >> 5) & 0x001fu;
    uint16_t effective_nametable = (uint16_t)(((base_v >> 10) & 0x0003u) ^ ((coarse_x >> 5) & 0x0001u));
    coarse_x &= 0x001fu;
    uint16_t name_table = (uint16_t)(effective_nametable * 0x0400u);
    uint16_t row = (base_v >> 12) & 0x0007u;
    uint16_t tile_index_addr = (uint16_t)(0x2000u + name_table + coarse_y * 32u + coarse_x);
    uint16_t attr_addr = (uint16_t)(0x23c0u + name_table + ((coarse_y >> 2) * 8u) + (coarse_x >> 2));
    uint8_t tile = ppu->nametables[ppu_nametable_index(cartridge, tile_index_addr)];
    uint8_t attr = ppu->nametables[ppu_nametable_index(cartridge, attr_addr)];
    uint16_t pattern_base = (ppu->ctrl & 0x10u) ? 0x1000u : 0x0000u;
    uint16_t pattern_addr = (uint16_t)(pattern_base + tile * 16u + row);
    uint8_t low = nrom_ppu_read(cartridge, pattern_addr);
    uint8_t high = nrom_ppu_read(cartridge, (uint16_t)(pattern_addr + 8u));
    uint8_t bit = (uint8_t)(7 - (total_x & 0x07u));
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

static PpuSpritePixelSample ppu_sample_sprite_pixel_internal(
    const Ppu *ppu,
    const NesCartridge *cartridge,
    const PpuScanlineSprite *sprite,
    int x,
    int y,
    bool apply_left_mask
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
    if (apply_left_mask && x < 8 && ((ppu->mask & PPU_MASK_SHOW_SPRITES_LEFT) == 0)) {
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

static PpuSpritePixelSample ppu_sample_sprite_pixel(
    const Ppu *ppu,
    const NesCartridge *cartridge,
    const PpuScanlineSprite *sprite,
    int x,
    int y
) {
    return ppu_sample_sprite_pixel_internal(ppu, cartridge, sprite, x, y, true);
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

static void ppu_diag_note_sprite0_pixel(
    Ppu *ppu,
    const NesCartridge *cartridge,
    const PpuScanlineSprite *selected_sprites,
    uint8_t selected_sprite_count,
    const PpuSpritePixelSample *winning_sprite,
    int x,
    int y
) {
    PpuSprite0FrameDiag *diag = &ppu->sprite0_diag.current_frame;
    PpuScanlineSprite sprite0;
    PpuSpritePixelSample sprite0_raw;
    PpuPixelSample background_raw;
    bool sprite0_selected = false;

    if (!diag->valid) {
        return;
    }

    sprite0.oam_index = 0;
    sprite0.y = ppu->oam[0];
    sprite0.tile = ppu->oam[1];
    sprite0.attributes = ppu->oam[2];
    sprite0.x = ppu->oam[3];

    if (!ppu_sprite_intersects_scanline(&sprite0, y, ppu_sprite_height(ppu))) {
        return;
    }
    if (x < sprite0.x || x >= sprite0.x + 8) {
        return;
    }

    diag->sprite0_intersects_visible = true;
    ++diag->visible_candidate_pixels;

    sprite0_raw = ppu_sample_sprite_pixel_internal(ppu, cartridge, &sprite0, x, y, false);
    background_raw = ppu_background_pixel(ppu, cartridge, x, y);
    if (background_raw.opaque) {
        ++diag->bg_opaque_pixels_in_sprite_bounds;
    }

    if (!diag->show_bg || !diag->show_sprites) {
        ++diag->reject_render_disabled;
        ppu_sprite0_diag_add_example(diag, PPU_SPRITE0_REJECT_RENDER_DISABLED, x, y);
        return;
    }
    if (x < 8 && (!diag->show_bg_left || !diag->show_sprites_left)) {
        ++diag->reject_left_mask;
        ppu_sprite0_diag_add_example(diag, PPU_SPRITE0_REJECT_LEFT_MASK, x, y);
        return;
    }
    if (!sprite0_raw.opaque) {
        ++diag->reject_sprite_transparent;
        ppu_sprite0_diag_add_example(diag, PPU_SPRITE0_REJECT_SPRITE_TRANSPARENT, x, y);
        return;
    }

    ++diag->sprite0_opaque_pixels_raw;
    if (!background_raw.opaque) {
        ++diag->reject_bg_transparent;
        ppu_sprite0_diag_add_example(diag, PPU_SPRITE0_REJECT_BG_TRANSPARENT, x, y);
        return;
    }

    ++diag->raw_overlap_pixels;
    if (!diag->first_raw_overlap_valid) {
        diag->first_raw_overlap_valid = true;
        diag->first_raw_overlap_x = x;
        diag->first_raw_overlap_y = y;
    }

    if (x >= 255) {
        ++diag->reject_x255;
        ppu_sprite0_diag_add_example(diag, PPU_SPRITE0_REJECT_X255, x, y);
        return;
    }

    for (uint8_t i = 0; i < selected_sprite_count; ++i) {
        if (selected_sprites[i].oam_index == 0) {
            sprite0_selected = true;
            break;
        }
    }
    if (!sprite0_selected) {
        ++diag->reject_not_in_scanline_selection;
        ppu_sprite0_diag_add_example(diag, PPU_SPRITE0_REJECT_NOT_IN_SCANLINE_SELECTION, x, y);
        return;
    }
    if (winning_sprite->opaque && !winning_sprite->sprite0) {
        ++diag->reject_occluded_by_earlier_sprite;
        ppu_sprite0_diag_add_example(diag, PPU_SPRITE0_REJECT_OCCLUDED_BY_EARLIER_SPRITE, x, y);
        return;
    }

    ++diag->effective_overlap_pixels;
    if (!diag->first_effective_overlap_valid) {
        diag->first_effective_overlap_valid = true;
        diag->first_effective_overlap_x = x;
        diag->first_effective_overlap_y = y;
    }
}

static void ppu_note_sprite0_hit(Ppu *ppu, int x, int y) {
    if (ppu->status & PPU_STATUS_SPRITE0_HIT) {
        return;
    }

    ppu->status |= PPU_STATUS_SPRITE0_HIT;
    ppu_record_sprite0_status_set(ppu, x, y);
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
    PpuScanlineSprite sprite0;
    bool sprite0_visible_on_scanline;
    bool sprite0_selected_on_scanline = false;

    sprite0.oam_index = 0;
    sprite0.y = ppu->oam[0];
    sprite0.tile = ppu->oam[1];
    sprite0.attributes = ppu->oam[2];
    sprite0.x = ppu->oam[3];
    sprite0_visible_on_scanline = ppu_sprite_intersects_scanline(&sprite0, y, ppu_sprite_height(ppu));

    if (sprite_count > ppu->max_scanline_sprite_count) {
        ppu->max_scanline_sprite_count = sprite_count;
    }

    if (ppu->sprite0_diag.current_frame.valid && sprite0_visible_on_scanline) {
        ++ppu->sprite0_diag.current_frame.sprite0_visible_scanline_count;
    }
    for (uint8_t i = 0; i < sprite_count; ++i) {
        if (sprites[i].oam_index == 0) {
            sprite0_selected_on_scanline = true;
            break;
        }
    }
    if (ppu->sprite0_diag.current_frame.valid && sprite0_selected_on_scanline) {
        ++ppu->sprite0_diag.current_frame.sprite0_selected_scanline_count;
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
        ppu_diag_note_sprite0_pixel(ppu, cartridge, sprites, sprite_count, &sprite, x, y);
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
    bool diag_enabled = ppu->sprite0_diag.enabled;
    uint64_t diag_start = ppu->sprite0_diag.frame_start;
    uint64_t diag_end = ppu->sprite0_diag.frame_end;

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
    ppu->render_vram_addr = 0;
    ppu->render_scroll_x = 0;
    ppu->render_scroll_y = 0;
    ppu->render_base_nametable = 0;
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
    memset(&ppu->sprite0_diag, 0, sizeof(ppu->sprite0_diag));
    ppu->sprite0_diag.enabled = diag_enabled;
    ppu->sprite0_diag.frame_start = diag_start;
    ppu->sprite0_diag.frame_end = diag_end;
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

void ppu_set_sprite0_diag_window(Ppu *ppu, uint64_t frame_start, uint64_t frame_end) {
    memset(&ppu->sprite0_diag, 0, sizeof(ppu->sprite0_diag));
    ppu->sprite0_diag.enabled = frame_end >= frame_start;
    ppu->sprite0_diag.frame_start = frame_start;
    ppu->sprite0_diag.frame_end = frame_end;
}

void ppu_oam_write_byte(Ppu *ppu, uint8_t index, uint8_t value, bool via_dma) {
    ppu->oam[index] = value;

    if (index >= 4u) {
        return;
    }

    ppu->sprite0_diag.current_frame.sprite0_oam_changed_mask |= (uint8_t)(1u << index);
    ppu->sprite0_diag.current_frame.last_sprite0_oam_update_render_frame = ppu->frame_count;
    ppu->sprite0_diag.current_frame.last_sprite0_oam_update_scanline = ppu->scanline;
    ppu->sprite0_diag.current_frame.last_sprite0_oam_update_cycle = ppu->cycle;
    ppu->sprite0_diag.current_frame.last_sprite0_oam_update_source =
        via_dma ? PPU_OAM_UPDATE_DMA : PPU_OAM_UPDATE_OAMDATA;
    ppu->sprite0_diag.last_sprite0_oam_update_render_frame = ppu->frame_count;
    ppu->sprite0_diag.last_sprite0_oam_update_scanline = ppu->scanline;
    ppu->sprite0_diag.last_sprite0_oam_update_cycle = ppu->cycle;
    ppu->sprite0_diag.last_sprite0_oam_update_source =
        via_dma ? PPU_OAM_UPDATE_DMA : PPU_OAM_UPDATE_OAMDATA;

    if (ppu->sprite0_diag.current_frame.valid) {
        ppu->sprite0_diag.current_frame.sprite_y = ppu->oam[0];
        ppu->sprite0_diag.current_frame.sprite_tile = ppu->oam[1];
        ppu->sprite0_diag.current_frame.sprite_attributes = ppu->oam[2];
        ppu->sprite0_diag.current_frame.sprite_x = ppu->oam[3];
    }
}

void ppu_step_cycles(Ppu *ppu, NesCartridge *cartridge, uint32_t cycles) {
    for (uint32_t i = 0; i < cycles; ++i) {
        ++ppu->cycle;

        if (ppu->cycle == 1 && ppu->scanline >= 0 && ppu->scanline < NES_FRAME_HEIGHT) {
            if (ppu_rendering_enabled(ppu)) {
                if (ppu->scanline == 0) {
                    ppu_copy_vertical_bits_from_temp(ppu);
                }
                ppu_copy_horizontal_bits_from_temp(ppu);
            }
            ppu_latch_render_state(ppu);
        }

        if (ppu->scanline == 241 && ppu->cycle == 1) {
            ppu_finalize_frame(ppu);
            ppu_sprite0_diag_commit_frame(ppu);
            ppu->status |= PPU_STATUS_VBLANK;
            ppu->frame_ready = true;
            if (ppu->ctrl & 0x80u) {
                ppu->nmi_pending = true;
            }
        } else if (ppu->scanline == 261 && ppu->cycle == 1) {
            ppu_record_sprite0_status_clear(ppu, true);
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
                if (ppu_rendering_enabled(ppu)) {
                    ppu_increment_vertical_v(ppu);
                    ppu_copy_horizontal_bits_from_temp(ppu);
                }
            }

            ++ppu->scanline;
            if (ppu->scanline > 261) {
                ppu->scanline = 0;
                ++ppu->frame_count;
                ppu->sprite_composited_pixel_count = 0;
                ppu->frame_buffer.frame_index = ppu->frame_count;
                ppu_latch_render_state(ppu);
                ppu_sprite0_diag_begin_frame(ppu);
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
        ppu->temp_addr = (uint16_t)((ppu->temp_addr & (uint16_t)~0x0c00u) | (((uint16_t)value & 0x03u) << 10));
        break;
    case PPU_REG_PPUMASK:
        ppu->mask = value;
        break;
    case PPU_REG_OAMADDR:
        ppu->oam_addr = value;
        break;
    case PPU_REG_OAMDATA:
        ppu_oam_write_byte(ppu, ppu->oam_addr++, value, false);
        break;
    case PPU_REG_PPUSCROLL:
        if (!ppu->write_toggle) {
            ppu->scroll_x = value;
            ppu->fine_x = value & 0x07u;
            ppu->temp_addr = (uint16_t)((ppu->temp_addr & (uint16_t)~0x001fu) | ((uint16_t)value >> 3));
        } else {
            ppu->scroll_y = value;
            ppu->temp_addr = (uint16_t)(
                (ppu->temp_addr & (uint16_t)~0x73e0u) |
                (((uint16_t)value & 0x07u) << 12) |
                (((uint16_t)value & 0xf8u) << 2)
            );
        }
        ppu->write_toggle = !ppu->write_toggle;
        break;
    case PPU_REG_PPUADDR:
        if (!ppu->write_toggle) {
            ppu->temp_addr = (uint16_t)((ppu->temp_addr & 0x00ffu) | (((uint16_t)value & 0x3fu) << 8));
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

const PpuSprite0Diag *ppu_sprite0_diag(const Ppu *ppu) {
    return &ppu->sprite0_diag;
}
