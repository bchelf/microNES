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

static inline uint64_t ppu_profile_now_us(const Ppu *ppu) {
#if MICRONES_ENABLE_STEP_PROFILING
    if (ppu->profile_now_us != NULL) {
        return ppu->profile_now_us(ppu->profile_now_user);
    }
#else
    (void)ppu;
#endif
    return 0;
}

#if MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
static uint64_t ppu_hash_framebuffer(const NesFrameBuffer *frame_buffer) {
    uint64_t hash = 1469598103934665603ull;

    for (uint32_t i = 0; i < NES_FRAME_WIDTH * NES_FRAME_HEIGHT; ++i) {
        hash ^= frame_buffer->pixels[i];
        hash *= 1099511628211ull;
    }

    return hash;
}
#else
static uint64_t ppu_hash_framebuffer(const NesFrameBuffer *frame_buffer) {
    (void)frame_buffer;
    return 0;
}
#endif

static bool ppu_sprite0_diag_collect_this_frame(const Ppu *ppu) {
#if MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
    return (ppu->sprite0_diag.enabled &&
            ppu->frame_count >= ppu->sprite0_diag.frame_start &&
            ppu->frame_count <= ppu->sprite0_diag.frame_end) ||
           !ppu->sprite0_diag.first_hit_frame_valid;
#else
    (void)ppu;
    return false;
#endif
}

static void ppu_sprite0_diag_add_example(PpuSprite0FrameDiag *diag, uint8_t reason, int x, int y) {
#if MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
    if (!diag->valid || diag->example_count >= PPU_SPRITE0_DIAG_MAX_EXAMPLES) {
        return;
    }

    diag->examples[diag->example_count].reason = reason;
    diag->examples[diag->example_count].x = (uint16_t)x;
    diag->examples[diag->example_count].y = (uint16_t)y;
    ++diag->example_count;
#else
    (void)diag;
    (void)reason;
    (void)x;
    (void)y;
#endif
}

static void ppu_sprite0_diag_begin_frame(Ppu *ppu) {
#if MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
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
#else
    (void)ppu;
#endif
}

static void ppu_sprite0_diag_commit_frame(Ppu *ppu) {
#if MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
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
#else
    (void)ppu;
#endif
}

static void ppu_record_sprite0_status_set(Ppu *ppu, int x, int y) {
#if MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
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
#else
    (void)ppu;
    (void)x;
    (void)y;
#endif
}

static void ppu_record_sprite0_status_clear(Ppu *ppu, bool expected) {
#if MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
    ++ppu->sprite0_diag.total_status_clear_count;
    ppu->sprite0_diag.last_status_clear_render_frame = ppu->frame_count;
    ppu->sprite0_diag.last_status_clear_scanline = ppu->scanline;
    ppu->sprite0_diag.last_status_clear_cycle = ppu->cycle;
    if (!expected) {
        ++ppu->sprite0_diag.suspicious_status_clear_count;
    }
#else
    (void)ppu;
    (void)expected;
#endif
}

static void ppu_latch_render_state(Ppu *ppu) {
    uint16_t v = ppu->vram_addr;
    uint16_t coarse_x = v & 0x001fu;
    uint16_t coarse_y = (v >> 5) & 0x001fu;
    uint16_t fine_y = (v >> 12) & 0x0007u;

    ppu->render_vram_addr = v;
    ppu->render_fine_x = ppu->fine_x;
    ppu->render_scroll_x = (uint8_t)(((coarse_x << 3) | ppu->render_fine_x) & 0xffu);
    ppu->render_scroll_y = (uint8_t)(((coarse_y << 3) | fine_y) & 0xffu);
    ppu->render_base_nametable = (uint8_t)((v >> 10) & 0x03u);
}

static bool ppu_rendering_enabled(const Ppu *ppu) {
    return (ppu->mask & (PPU_MASK_SHOW_BG | PPU_MASK_SHOW_SPRITES)) != 0;
}

static bool ppu_visible_scanline_active(const Ppu *ppu) {
    return ppu->scanline >= 0 && ppu->scanline < NES_FRAME_HEIGHT && ppu->cycle > 0 && ppu->cycle <= 256;
}

static void ppu_copy_horizontal_bits_from_temp(Ppu *ppu) {
    ppu->vram_addr = (uint16_t)((ppu->vram_addr & (uint16_t)~0x041fu) | (ppu->temp_addr & 0x041fu));
}

static void ppu_copy_vertical_bits_from_temp(Ppu *ppu) {
    ppu->vram_addr = (uint16_t)((ppu->vram_addr & (uint16_t)~0x7be0u) | (ppu->temp_addr & 0x7be0u));
}

static void ppu_refresh_visible_scanline_render_state(Ppu *ppu) {
    if (!ppu_rendering_enabled(ppu) || !ppu_visible_scanline_active(ppu)) {
        return;
    }

    ppu_copy_horizontal_bits_from_temp(ppu);
    ppu_latch_render_state(ppu);
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
#if MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
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
    ppu->last_completed_visible_write_diag_count = ppu->visible_write_diag_count;
    memcpy(
        ppu->last_completed_visible_write_diag,
        ppu->visible_write_diag,
        sizeof(ppu->last_completed_visible_write_diag)
    );
#else
    ++ppu->completed_frame_count;
    ppu->completed_frame_ready = true;
    ppu->last_completed_nonzero_pixels = 0;
    ppu->last_completed_frame_hash = 0;
    ppu->last_completed_sprite_pixels = 0;
    ppu->last_completed_visible_write_diag_count = 0;
#endif
}

static void ppu_record_visible_write_diag(Ppu *ppu, uint8_t reg, uint8_t value) {
#if MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
    PpuVisibleWriteDiag *diag;

    if (ppu->scanline < 0 || ppu->scanline >= NES_FRAME_HEIGHT) {
        return;
    }
    if (ppu->cycle <= 0 || ppu->cycle > 256) {
        return;
    }
    if (ppu->visible_write_diag_count >= PPU_VISIBLE_WRITE_DIAG_MAX) {
        return;
    }

    diag = &ppu->visible_write_diag[ppu->visible_write_diag_count++];
    diag->frame_index = ppu->frame_count;
    diag->scanline = (uint16_t)ppu->scanline;
    diag->cycle = (uint16_t)ppu->cycle;
    diag->reg = reg;
    diag->value = value;
    diag->vram_addr = ppu->vram_addr;
    diag->temp_addr = ppu->temp_addr;
    diag->fine_x = ppu->fine_x;
    diag->ctrl = ppu->ctrl;
    diag->mask = ppu->mask;
#else
    (void)ppu;
    (void)reg;
    (void)value;
#endif
}

static uint16_t ppu_palette_index(uint16_t addr) {
    uint16_t index = addr & 0x1fu;
    if (index == 0x10u) index = 0x00u;
    if (index == 0x14u) index = 0x04u;
    if (index == 0x18u) index = 0x08u;
    if (index == 0x1cu) index = 0x0cu;
    return index;
}

static uint16_t MICRONES_HOT_FUNC(ppu_nametable_index)(const NesCartridge *cartridge, uint16_t addr) {
    uint16_t offset = (uint16_t)(addr - 0x2000u) & 0x0fffu;
    uint16_t table = offset >> 10;
    uint16_t inner = offset & 0x03ffu;
    uint16_t physical;

    if (cartridge->mirror_mode == NES_MIRROR_VERTICAL) {
        physical = table & 0x01u;
    } else {
        physical = (uint16_t)((table >> 1) & 0x01u);
    }

    return (uint16_t)(physical * 0x0400u + inner);
}

static void ppu_fill_render_tile_diag(
    const Ppu *ppu,
    const NesCartridge *cartridge,
    int tile_x,
    PpuRenderTileDiag *diag
) {
    uint16_t base_v = ppu->render_vram_addr;
    uint16_t total_x = (uint16_t)(ppu->render_fine_x + tile_x * 8);
    uint16_t coarse_x = (uint16_t)((base_v & 0x001fu) + (total_x >> 3));
    uint16_t coarse_y = (base_v >> 5) & 0x001fu;
    uint16_t effective_nametable = (uint16_t)(((base_v >> 10) & 0x0003u) ^ ((coarse_x >> 5) & 0x0001u));
    uint16_t fine_y = (base_v >> 12) & 0x0007u;
    uint16_t name_table = (uint16_t)(effective_nametable * 0x0400u);
    uint16_t nametable_addr;
    uint16_t attribute_addr;
    uint8_t tile_index;
    uint16_t pattern_base;
    uint16_t pattern_addr;

    coarse_x &= 0x001fu;
    nametable_addr = (uint16_t)(0x2000u + name_table + coarse_y * 32u + coarse_x);
    attribute_addr = (uint16_t)(0x23c0u + name_table + ((coarse_y >> 2) * 8u) + (coarse_x >> 2));
    tile_index = ppu->nametables[ppu_nametable_index(cartridge, nametable_addr)];
    pattern_base = (ppu->ctrl & 0x10u) ? 0x1000u : 0x0000u;
    pattern_addr = (uint16_t)(pattern_base + tile_index * 16u + fine_y);

    memset(diag, 0, sizeof(*diag));
    diag->tile_x = (uint8_t)tile_x;
    diag->coarse_x = (uint8_t)coarse_x;
    diag->coarse_y = (uint8_t)coarse_y;
    diag->fine_y = (uint8_t)fine_y;
    diag->nametable = (uint8_t)effective_nametable;
    diag->nametable_addr = nametable_addr;
    diag->attribute_addr = attribute_addr;
    diag->pattern_base = pattern_base;
    diag->tile_index = tile_index;
    diag->pattern_addr = pattern_addr;
    diag->pattern_low = nrom_ppu_read(cartridge, pattern_addr);
    diag->pattern_high = nrom_ppu_read(cartridge, (uint16_t)(pattern_addr + 8u));
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
    uint16_t total_x = (uint16_t)(ppu->render_fine_x + x);
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
    const uint8_t *row_pixels = nrom_ppu_row_pixels(cartridge, pattern_addr);
    uint8_t color_bits;
    uint8_t palette_select = (attr >> ((((coarse_y & 0x02u) << 1) | (coarse_x & 0x02u)))) & 0x03u;
    uint8_t palette_index;

    if (row_pixels != NULL) {
        color_bits = row_pixels[total_x & 0x07u];
    } else {
        uint8_t low = nrom_ppu_read(cartridge, pattern_addr);
        uint8_t high = nrom_ppu_read(cartridge, (uint16_t)(pattern_addr + 8u));
        uint8_t bit = (uint8_t)(7 - (total_x & 0x07u));
        color_bits = (uint8_t)((((high >> bit) & 0x01u) << 1) | ((low >> bit) & 0x01u));
    }
    palette_index = (uint8_t)((palette_select << 2) | color_bits);

    sample.opaque = (palette_index & 0x03u) != 0;
    if (!sample.opaque) {
        sample.color = ppu->palette[0];
        return sample;
    }
    sample.color = ppu->palette[palette_index & 0x1fu];
    return sample;
}

static void ppu_detect_render_artifact(
    Ppu *ppu,
    const NesCartridge *cartridge,
    int y,
    const uint8_t *row
) {
#if !MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
    (void)ppu;
    (void)cartridge;
    (void)y;
    (void)row;
    return;
#else
    const uint8_t *prev;
    uint16_t equal_prev_pixels = 0;
    uint16_t repeated_prev_chunks = 0;
    uint16_t transitions = 0;
    uint16_t longest_run = 1;
    uint16_t current_run = 1;
    int first_transition_x = -1;

    if (y <= 0 || ppu->render_artifact_diag.valid) {
        return;
    }

    prev = nes_framebuffer_scanline(&ppu->frame_buffer, (uint16_t)(y - 1));
    for (int x = 0; x < NES_FRAME_WIDTH; ++x) {
        if (row[x] == prev[x]) {
            ++equal_prev_pixels;
        }
        if (x > 0) {
            if (row[x] != row[x - 1]) {
                ++transitions;
                current_run = 1;
                if (first_transition_x < 0) {
                    first_transition_x = x;
                }
            } else {
                ++current_run;
                if (current_run > longest_run) {
                    longest_run = current_run;
                }
            }
        }
    }

    for (int x = 0; x < NES_FRAME_WIDTH; x += 8) {
        if (memcmp(&row[x], &prev[x], 8) == 0) {
            ++repeated_prev_chunks;
        }
    }

    if (ppu->render_fine_x == ppu->fine_x) {
        return;
    }

    if (!(equal_prev_pixels >= 224u && repeated_prev_chunks >= 20u && transitions >= 12u)) {
        return;
    }

    ppu->render_artifact_diag.valid = true;
    ppu->render_artifact_diag.frame_index = ppu->frame_count;
    ppu->render_artifact_diag.scanline = (uint16_t)y;
    ppu->render_artifact_diag.equal_prev_pixels = equal_prev_pixels;
    ppu->render_artifact_diag.repeated_prev_chunks = repeated_prev_chunks;
    ppu->render_artifact_diag.transitions = transitions;
    ppu->render_artifact_diag.longest_run = longest_run;
    ppu->render_artifact_diag.focus_x = (uint16_t)(first_transition_x >= 0 ? first_transition_x : 0);

    {
        int center_tile = ppu->render_artifact_diag.focus_x / 8;
        int start_tile = center_tile - (PPU_RENDER_ARTIFACT_TILE_WINDOW / 2);
        if (start_tile < 0) {
            start_tile = 0;
        }
        if (start_tile > 32 - PPU_RENDER_ARTIFACT_TILE_WINDOW) {
            start_tile = 32 - PPU_RENDER_ARTIFACT_TILE_WINDOW;
        }

        ppu->render_artifact_diag.tile_count = PPU_RENDER_ARTIFACT_TILE_WINDOW;
        for (int i = 0; i < PPU_RENDER_ARTIFACT_TILE_WINDOW; ++i) {
            ppu_fill_render_tile_diag(ppu, cartridge, start_tile + i, &ppu->render_artifact_diag.tiles[i]);
        }
    }
#endif
}

static int MICRONES_HOT_FUNC(ppu_sprite_height)(const Ppu *ppu) {
    return (ppu->ctrl & 0x20u) ? 16 : 8;
}

static bool ppu_sprite_intersects_scanline(const PpuScanlineSprite *sprite, int y, int sprite_height) {
    int sprite_top = (int)sprite->y + 1;
    return y >= sprite_top && y < sprite_top + sprite_height;
}

static uint8_t MICRONES_HOT_FUNC(ppu_collect_scanline_sprites)(const Ppu *ppu, int y, PpuScanlineSprite *sprites) {
    uint8_t count = 0;
    int sprite_height = ppu_sprite_height(ppu);

    if ((ppu->mask & PPU_MASK_SHOW_SPRITES) == 0) {
        return 0;
    }

    for (uint8_t sprite_index = 0; sprite_index < 64; ++sprite_index) {
        uint16_t base = (uint16_t)sprite_index * 4u;

        /* Check y-intersection using only the first OAM byte before loading
         * tile/attr/x.  For a typical scanline, 60+ of 64 sprites miss, so
         * deferring the other three loads saves ~0.3 ms/frame. */
        int sprite_top = (int)ppu->oam[base] + 1;
        if (y < sprite_top || y >= sprite_top + sprite_height) {
            continue;
        }

        if (count >= PPU_MAX_SCANLINE_SPRITES) {
            break;
        }

        sprites[count].oam_index = sprite_index;
        sprites[count].y = (uint8_t)(sprite_top - 1);
        sprites[count].tile = ppu->oam[base + 1];
        sprites[count].attributes = ppu->oam[base + 2];
        sprites[count].x = ppu->oam[base + 3];
        ++count;
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

    {
        const uint8_t *row_pixels = nrom_ppu_row_pixels(cartridge, pattern_addr);
        if (row_pixels != NULL) {
            color_bits = row_pixels[local_x];
        } else {
            uint8_t low = nrom_ppu_read(cartridge, pattern_addr);
            uint8_t high = nrom_ppu_read(cartridge, (uint16_t)(pattern_addr + 8u));
            uint8_t bit = (uint8_t)(7 - local_x);
            color_bits = (uint8_t)((((high >> bit) & 0x01u) << 1) | ((low >> bit) & 0x01u));
        }
    }
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
#if MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
    ppu_record_sprite0_status_set(ppu, x, y);
    ++ppu->sprite0_hit_count;
    if (!ppu->sprite0_hit_ever) {
        ppu->sprite0_hit_ever = true;
        ppu->first_sprite0_hit_frame = ppu->frame_count;
        ppu->first_sprite0_hit_scanline = y;
        ppu->first_sprite0_hit_x = x;
    }
#else
    (void)x;
    (void)y;
#endif
}

static void ppu_note_sprite0_opaque(Ppu *ppu, int x, int y) {
#if MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
    ++ppu->sprite0_opaque_pixel_count;
    if (ppu->first_sprite0_opaque_scanline < 0) {
        ppu->first_sprite0_opaque_frame = ppu->frame_count;
        ppu->first_sprite0_opaque_scanline = y;
        ppu->first_sprite0_opaque_x = x;
    }
#else
    (void)ppu;
    (void)x;
    (void)y;
#endif
}

static void MICRONES_HOT_FUNC(ppu_render_scanline)(Ppu *ppu, NesCartridge *cartridge, int y) {
#if !MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
#if MICRONES_ENABLE_FRAMEBUFFER
    uint8_t *dst = nes_framebuffer_scanline(&ppu->frame_buffer, (uint16_t)y);
#else
    uint8_t *dst = ppu->scanline_buffer.pixels;
#endif
    uint8_t *bg_opaque = ppu->render_bg_opaque;
    uint8_t *sprite_prio = ppu->render_sprite_prio;
    PpuScanlineSprite sprites[PPU_MAX_SCANLINE_SPRITES];
    bool show_bg = (ppu->mask & PPU_MASK_SHOW_BG) != 0;
    bool show_bg_left = (ppu->mask & PPU_MASK_SHOW_BG_LEFT) != 0;
    bool show_sprites = (ppu->mask & PPU_MASK_SHOW_SPRITES) != 0;
    bool show_sprites_left = (ppu->mask & PPU_MASK_SHOW_SPRITES_LEFT) != 0;
    uint8_t sprite_count = show_sprites ? ppu_collect_scanline_sprites(ppu, y, sprites) : 0;
    bool needs_sprite_comp = sprite_count != 0;
    uint8_t universal_color = ppu->palette[0];
    uint16_t bg_base_v = ppu->render_vram_addr;
    uint16_t bg_base_coarse_x = bg_base_v & 0x001fu;
    uint16_t bg_coarse_y = (bg_base_v >> 5) & 0x001fu;
    uint16_t bg_base_nametable = (bg_base_v >> 10) & 0x0003u;
    uint16_t bg_row = (bg_base_v >> 12) & 0x0007u;
    uint16_t bg_pattern_base = (ppu->ctrl & 0x10u) ? 0x1000u : 0x0000u;

    if (needs_sprite_comp) {
        memset(bg_opaque, 0, NES_FRAME_WIDTH);
        memset(sprite_prio, 0, NES_FRAME_WIDTH);
    }

    /* When show_bg is false the pixel buffer must be pre-filled with
     * universal_color so sprites composite correctly against the backdrop.
     * When show_bg is true every pixel is written by the tile loop below
     * (transparent tiles write universal_color inline), so the memset is
     * unnecessary and we skip it to save ~60 KB of writes per frame. */
    if (!show_bg) {
        memset(dst, universal_color, NES_FRAME_WIDTH);
    }

    if (show_bg) {
        for (int tile_group = 0; tile_group < 33; ++tile_group) {
            int screen_start_x = tile_group * 8 - ppu->render_fine_x;
            uint16_t coarse_x_total = (uint16_t)(bg_base_coarse_x + tile_group);
            uint16_t coarse_x = coarse_x_total & 0x001fu;
            uint16_t effective_nametable =
                (uint16_t)(bg_base_nametable ^ ((coarse_x_total >> 5) & 0x0001u));
            uint16_t name_table = (uint16_t)(effective_nametable * 0x0400u);
            uint16_t tile_index_addr =
                (uint16_t)(0x2000u + name_table + bg_coarse_y * 32u + coarse_x);
            uint16_t attr_addr =
                (uint16_t)(0x23c0u + name_table + ((bg_coarse_y >> 2) * 8u) + (coarse_x >> 2));
            uint8_t tile = ppu->nametables[ppu_nametable_index(cartridge, tile_index_addr)];
            uint8_t attr = ppu->nametables[ppu_nametable_index(cartridge, attr_addr)];
            uint8_t palette_select =
                (uint8_t)((attr >> ((((bg_coarse_y & 0x02u) << 1) | (coarse_x & 0x02u)))) & 0x03u);
            uint16_t pattern_addr = (uint16_t)(bg_pattern_base + tile * 16u + bg_row);
            const uint8_t *row_pixels = nrom_ppu_row_pixels(cartridge, pattern_addr);
            uint8_t pal_base = (uint8_t)(palette_select << 2);

            if (row_pixels != NULL) {
                if (needs_sprite_comp) {
                    if ((uint32_t)(tile_group - 2) <= 29u) {
                        /* Fast path: all 8 pixels are on-screen; write
                         * universal_color for transparent pixels so the
                         * pre-fill memset is not needed. */
                        for (int px = 0; px < 8; ++px) {
                            uint8_t color_bits = row_pixels[px];
                            int screen_x = screen_start_x + px;
                            if (color_bits != 0) {
                                bg_opaque[screen_x] = 1;
                                dst[screen_x] = ppu->palette[pal_base | color_bits];
                            } else {
                                dst[screen_x] = universal_color;
                            }
                        }
                    } else {
                        for (int px = 0; px < 8; ++px) {
                            int screen_x = screen_start_x + px;
                            if (screen_x < 0 || screen_x >= NES_FRAME_WIDTH) continue;
                            if (screen_x < 8 && !show_bg_left) {
                                dst[screen_x] = universal_color;
                                continue;
                            }
                            uint8_t color_bits = row_pixels[px];
                            if (color_bits != 0) {
                                bg_opaque[screen_x] = 1;
                                dst[screen_x] = ppu->palette[pal_base | color_bits];
                            } else {
                                dst[screen_x] = universal_color;
                            }
                        }
                    }
                } else {
                    if ((uint32_t)(tile_group - 2) <= 29u) {
                        /* Fast path: unconditional write eliminates branch and
                         * the need for a pre-fill memset. */
                        for (int px = 0; px < 8; ++px) {
                            uint8_t color_bits = row_pixels[px];
                            dst[screen_start_x + px] = color_bits
                                ? ppu->palette[pal_base | color_bits]
                                : universal_color;
                        }
                    } else {
                        for (int px = 0; px < 8; ++px) {
                            int screen_x = screen_start_x + px;
                            if (screen_x < 0 || screen_x >= NES_FRAME_WIDTH) continue;
                            if (screen_x < 8 && !show_bg_left) {
                                dst[screen_x] = universal_color;
                                continue;
                            }
                            uint8_t color_bits = row_pixels[px];
                            dst[screen_x] = color_bits
                                ? ppu->palette[pal_base | color_bits]
                                : universal_color;
                        }
                    }
                }
            } else {
                /* row_pixels == NULL: decode both CHR planes into a local
                 * 8-element array first (item 4), then render with the same
                 * logic as the row_pixels paths above. */
                uint8_t low = nrom_ppu_read(cartridge, pattern_addr);
                uint8_t high = nrom_ppu_read(cartridge, (uint16_t)(pattern_addr + 8u));
                uint8_t decoded[8];
                for (int px = 0; px < 8; ++px) {
                    uint8_t bit = (uint8_t)(7 - px);
                    decoded[px] = (uint8_t)((((high >> bit) & 0x01u) << 1) | ((low >> bit) & 0x01u));
                }
                if (needs_sprite_comp) {
                    for (int px = 0; px < 8; ++px) {
                        int screen_x = screen_start_x + px;
                        if (screen_x < 0 || screen_x >= NES_FRAME_WIDTH) continue;
                        if (screen_x < 8 && !show_bg_left) {
                            dst[screen_x] = universal_color;
                            continue;
                        }
                        uint8_t color_bits = decoded[px];
                        if (color_bits != 0) {
                            bg_opaque[screen_x] = 1;
                            dst[screen_x] = ppu->palette[pal_base | color_bits];
                        } else {
                            dst[screen_x] = universal_color;
                        }
                    }
                } else {
                    for (int px = 0; px < 8; ++px) {
                        int screen_x = screen_start_x + px;
                        if (screen_x < 0 || screen_x >= NES_FRAME_WIDTH) continue;
                        if (screen_x < 8 && !show_bg_left) {
                            dst[screen_x] = universal_color;
                            continue;
                        }
                        uint8_t color_bits = decoded[px];
                        dst[screen_x] = color_bits
                            ? ppu->palette[pal_base | color_bits]
                            : universal_color;
                    }
                }
            }
        }
    }

    for (int i = 0; i < (int)sprite_count; ++i) {
        const PpuScanlineSprite *sprite = &sprites[i];
        int sprite_height = ppu_sprite_height(ppu);
        int local_y = y - ((int)sprite->y + 1);
        uint16_t pattern_addr;
        const uint8_t *row_pixels;
        uint8_t low;
        uint8_t high;
        uint8_t colors[4];
        uint16_t x_start;
        uint16_t x_end;
        bool flip_h;
        bool behind_bg;

        if (local_y < 0 || local_y >= sprite_height) {
            continue;
        }
        if (sprite->attributes & 0x80u) {
            local_y = sprite_height - 1 - local_y;
        }

        if (sprite_height == 16) {
            uint8_t tile_row = (uint8_t)(local_y >> 3);
            uint8_t row_in_tile = (uint8_t)(local_y & 0x07u);
            uint16_t pattern_base = (sprite->tile & 0x01u) ? 0x1000u : 0x0000u;
            uint8_t tile_number = (uint8_t)((sprite->tile & 0xfeu) + tile_row);
            pattern_addr = (uint16_t)(pattern_base + tile_number * 16u + row_in_tile);
        } else {
            uint16_t pattern_base = (ppu->ctrl & 0x08u) ? 0x1000u : 0x0000u;
            pattern_addr = (uint16_t)(pattern_base + sprite->tile * 16u + local_y);
        }

        x_start = sprite->x;
        if (!show_sprites_left && x_start < 8u) {
            x_start = 8u;
        }
        x_end = (uint16_t)sprite->x + 8u;
        if (x_end > NES_FRAME_WIDTH) {
            x_end = NES_FRAME_WIDTH;
        }
        if (x_start >= x_end) {
            continue;
        }

        row_pixels = nrom_ppu_row_pixels(cartridge, pattern_addr);
        if (row_pixels == NULL) {
            low = nrom_ppu_read(cartridge, pattern_addr);
            high = nrom_ppu_read(cartridge, (uint16_t)(pattern_addr + 8u));
        } else {
            low = 0;
            high = 0;
        }

        colors[0] = 0;
        colors[1] = ppu->palette[(0x10u + ((sprite->attributes & 0x03u) << 2) + 1u) & 0x1fu];
        colors[2] = ppu->palette[(0x10u + ((sprite->attributes & 0x03u) << 2) + 2u) & 0x1fu];
        colors[3] = ppu->palette[(0x10u + ((sprite->attributes & 0x03u) << 2) + 3u) & 0x1fu];
        flip_h = (sprite->attributes & 0x40u) != 0;
        behind_bg = (sprite->attributes & 0x20u) != 0;

        if (row_pixels != NULL) {
            if (!flip_h) {
                int source_x = (int)(x_start - sprite->x);
                for (uint16_t screen_x = x_start; screen_x < x_end; ++screen_x, ++source_x) {
                    uint8_t color_bits = row_pixels[source_x];
                    if (color_bits == 0) continue;
                    if (sprite_prio[screen_x]) continue;
                    sprite_prio[screen_x] = 1;
                    if (sprite->oam_index == 0 && bg_opaque[screen_x] && screen_x < 255u) {
                        ppu_note_sprite0_hit(ppu, (int)screen_x, y);
                    }
                    if (!behind_bg || !bg_opaque[screen_x]) {
                        dst[screen_x] = colors[color_bits];
                    }
                }
            } else {
                int source_x = 7 - (int)(x_start - sprite->x);
                for (uint16_t screen_x = x_start; screen_x < x_end; ++screen_x, --source_x) {
                    uint8_t color_bits = row_pixels[source_x];
                    if (color_bits == 0) continue;
                    if (sprite_prio[screen_x]) continue;
                    sprite_prio[screen_x] = 1;
                    if (sprite->oam_index == 0 && bg_opaque[screen_x] && screen_x < 255u) {
                        ppu_note_sprite0_hit(ppu, (int)screen_x, y);
                    }
                    if (!behind_bg || !bg_opaque[screen_x]) {
                        dst[screen_x] = colors[color_bits];
                    }
                }
            }
        } else if (!flip_h) {
            int source_x = (int)(x_start - sprite->x);
            for (uint16_t screen_x = x_start; screen_x < x_end; ++screen_x, ++source_x) {
                uint8_t bit = (uint8_t)(7 - source_x);
                uint8_t color_bits =
                    (uint8_t)((((high >> bit) & 0x01u) << 1) | ((low >> bit) & 0x01u));
                if (color_bits == 0) continue;
                if (sprite_prio[screen_x]) continue;
                sprite_prio[screen_x] = 1;
                if (sprite->oam_index == 0 && bg_opaque[screen_x] && screen_x < 255u) {
                    ppu_note_sprite0_hit(ppu, (int)screen_x, y);
                }
                if (!behind_bg || !bg_opaque[screen_x]) {
                    dst[screen_x] = colors[color_bits];
                }
            }
        } else {
            int source_x = 7 - (int)(x_start - sprite->x);
            for (uint16_t screen_x = x_start; screen_x < x_end; ++screen_x, --source_x) {
                uint8_t bit = (uint8_t)(7 - source_x);
                uint8_t color_bits =
                    (uint8_t)((((high >> bit) & 0x01u) << 1) | ((low >> bit) & 0x01u));
                if (color_bits == 0) continue;
                if (sprite_prio[screen_x]) continue;
                sprite_prio[screen_x] = 1;
                if (sprite->oam_index == 0 && bg_opaque[screen_x] && screen_x < 255u) {
                    ppu_note_sprite0_hit(ppu, (int)screen_x, y);
                }
                if (!behind_bg || !bg_opaque[screen_x]) {
                    dst[screen_x] = colors[color_bits];
                }
            }
        }
    }

#if MICRONES_ENABLE_SCANLINE_BUFFER
#if MICRONES_ENABLE_FRAMEBUFFER
    memcpy(ppu->scanline_buffer.pixels, dst, NES_FRAME_WIDTH);
#endif
#endif
    ppu->scanline_buffer.y = (uint16_t)y;
    ppu->scanline_buffer.frame_index = ppu->frame_count;
    ppu->scanline_buffer.ready = true;
    ppu->scanline_ready = true;
    return;
#else /* MICRONES_ENABLE_RUNTIME_DIAGNOSTICS */
    uint8_t *dst =
#if MICRONES_ENABLE_FRAMEBUFFER
        nes_framebuffer_scanline(&ppu->frame_buffer, (uint16_t)y);
#else
        NULL;
#endif
    PpuScanlineSprite sprites[PPU_MAX_SCANLINE_SPRITES];
    uint8_t sprite_count = ppu_collect_scanline_sprites(ppu, y, sprites);
    PpuScanlineSprite sprite0;
    bool sprite0_visible_on_scanline;
    bool sprite0_selected_on_scanline = false;
    bool show_bg = (ppu->mask & PPU_MASK_SHOW_BG) != 0;
    bool show_bg_left = (ppu->mask & PPU_MASK_SHOW_BG_LEFT) != 0;
    uint16_t bg_base_v = ppu->render_vram_addr;
    uint16_t bg_base_coarse_x = bg_base_v & 0x001fu;
    uint16_t bg_coarse_y = (bg_base_v >> 5) & 0x001fu;
    uint16_t bg_base_nametable = (bg_base_v >> 10) & 0x0003u;
    uint16_t bg_row = (bg_base_v >> 12) & 0x0007u;
    uint16_t bg_pattern_base = (ppu->ctrl & 0x10u) ? 0x1000u : 0x0000u;
    uint16_t cached_tile_group = 0xffffu;
    uint8_t cached_low = 0;
    uint8_t cached_high = 0;
    uint8_t cached_palette_select = 0;

    sprite0.oam_index = 0;
    sprite0.y = ppu->oam[0];
    sprite0.tile = ppu->oam[1];
    sprite0.attributes = ppu->oam[2];
    sprite0.x = ppu->oam[3];
    sprite0_visible_on_scanline = ppu_sprite_intersects_scanline(&sprite0, y, ppu_sprite_height(ppu));

    if (sprite_count > ppu->max_scanline_sprite_count) {
        ppu->max_scanline_sprite_count = sprite_count;
    }

#if MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
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
#endif

    for (int x = 0; x < NES_FRAME_WIDTH; ++x) {
        PpuPixelSample background = { ppu->palette[0], false };
        PpuSpritePixelSample sprite = { 0, false, false, false };
        uint8_t color;
        bool use_sprite = false;

        if (show_bg && (x >= 8 || show_bg_left)) {
            uint16_t total_x = (uint16_t)(ppu->render_fine_x + x);
            uint16_t tile_group = total_x >> 3;

            if (tile_group != cached_tile_group) {
                uint16_t coarse_x_total = (uint16_t)(bg_base_coarse_x + tile_group);
                uint16_t coarse_x = coarse_x_total & 0x001fu;
                uint16_t effective_nametable =
                    (uint16_t)(bg_base_nametable ^ ((coarse_x_total >> 5) & 0x0001u));
                uint16_t name_table = (uint16_t)(effective_nametable * 0x0400u);
                uint16_t tile_index_addr =
                    (uint16_t)(0x2000u + name_table + bg_coarse_y * 32u + coarse_x);
                uint16_t attr_addr =
                    (uint16_t)(0x23c0u + name_table + ((bg_coarse_y >> 2) * 8u) + (coarse_x >> 2));
                uint8_t tile = ppu->nametables[ppu_nametable_index(cartridge, tile_index_addr)];
                uint8_t attr = ppu->nametables[ppu_nametable_index(cartridge, attr_addr)];
                uint16_t pattern_addr = (uint16_t)(bg_pattern_base + tile * 16u + bg_row);

                cached_low = nrom_ppu_read(cartridge, pattern_addr);
                cached_high = nrom_ppu_read(cartridge, (uint16_t)(pattern_addr + 8u));
                cached_palette_select =
                    (uint8_t)((attr >> ((((bg_coarse_y & 0x02u) << 1) | (coarse_x & 0x02u)))) & 0x03u);
                cached_tile_group = tile_group;
            }

            {
                uint8_t bit = (uint8_t)(7 - (total_x & 0x07u));
                uint8_t color_low = (cached_low >> bit) & 0x01u;
                uint8_t color_high = (cached_high >> bit) & 0x01u;
                uint8_t palette_index =
                    (uint8_t)((cached_palette_select << 2) | (color_high << 1) | color_low);

                background.opaque = (palette_index & 0x03u) != 0;
                background.color = background.opaque ? ppu->palette[palette_index & 0x1fu] : ppu->palette[0];
            }
        }
        sprite = ppu_visible_sprite_pixel(ppu, cartridge, sprites, sprite_count, x, y);
#if MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
        ppu_diag_note_sprite0_pixel(ppu, cartridge, sprites, sprite_count, &sprite, x, y);
        if (sprite.opaque && sprite.sprite0) {
            ppu_note_sprite0_opaque(ppu, x, y);
        }
#endif

        if (x < 255 && background.opaque && sprite.opaque && sprite.sprite0) {
#if MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
            ++ppu->sprite0_background_overlap_count;
#endif
            ppu_note_sprite0_hit(ppu, x, y);
        }

        color = background.color;
        if (sprite.opaque && (!sprite.behind_background || !background.opaque)) {
            color = sprite.color;
            use_sprite = true;
        }

        if (use_sprite) {
#if MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
            ++ppu->sprite_composited_pixel_count;
#endif
        }

#if MICRONES_ENABLE_FRAMEBUFFER
        dst[x] = color;
#endif
        ppu->scanline_buffer.pixels[x] = color;
    }

#if MICRONES_ENABLE_FRAMEBUFFER
    ppu_detect_render_artifact(ppu, cartridge, y, dst);
#else
    (void)cartridge;
#endif

    ppu->scanline_buffer.y = (uint16_t)y;
    ppu->scanline_buffer.frame_index = ppu->frame_count;
    ppu->scanline_buffer.ready = true;
    ppu->scanline_ready = true;
#endif
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
    ppu->render_fine_x = 0;
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
    memset(&ppu->render_artifact_diag, 0, sizeof(ppu->render_artifact_diag));
#if MICRONES_ENABLE_FRAMEBUFFER
    ppu->frame_buffer.frame_index = 0;
#endif
    ppu->scanline_buffer.frame_index = 0;
    ppu->scanline_buffer.y = 0;
    ppu->scanline_buffer.ready = false;
    memset(ppu->oam, 0, sizeof(ppu->oam));
    memset(ppu->nametables, 0, sizeof(ppu->nametables));
    memset(ppu->palette, 0, sizeof(ppu->palette));
#if MICRONES_ENABLE_FRAMEBUFFER
    memset(ppu->frame_buffer.pixels, 0, sizeof(ppu->frame_buffer.pixels));
#endif
#if MICRONES_ENABLE_SCANLINE_BUFFER
    memset(ppu->scanline_buffer.pixels, 0, sizeof(ppu->scanline_buffer.pixels));
#endif
    memset(ppu->visible_write_diag, 0, sizeof(ppu->visible_write_diag));
    memset(ppu->last_completed_visible_write_diag, 0, sizeof(ppu->last_completed_visible_write_diag));
    ppu->visible_write_diag_count = 0;
    ppu->last_completed_visible_write_diag_count = 0;
    memset(&ppu->step_profile, 0, sizeof(ppu->step_profile));
}

void ppu_set_sprite0_diag_window(Ppu *ppu, uint64_t frame_start, uint64_t frame_end) {
    memset(&ppu->sprite0_diag, 0, sizeof(ppu->sprite0_diag));
    ppu->sprite0_diag.enabled = frame_end >= frame_start;
    ppu->sprite0_diag.frame_start = frame_start;
    ppu->sprite0_diag.frame_end = frame_end;
}

void ppu_oam_write_byte(Ppu *ppu, uint8_t index, uint8_t value, bool via_dma) {
    ppu->oam[index] = value;

#if !MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
    (void)via_dma;
    return;
#else
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
#endif
}

static inline void ppu_step_cycle_1(Ppu *ppu) {
    if (ppu->scanline >= 0 && ppu->scanline < NES_FRAME_HEIGHT) {
        if (ppu_rendering_enabled(ppu)) {
            if (ppu->scanline == 0) {
                ppu_copy_vertical_bits_from_temp(ppu);
            }
            ppu_copy_horizontal_bits_from_temp(ppu);
        }
        ppu_latch_render_state(ppu);
    }

    if (ppu->scanline == 241) {
        ppu_finalize_frame(ppu);
        ppu_sprite0_diag_commit_frame(ppu);
        ppu->status |= PPU_STATUS_VBLANK;
        ppu->frame_ready = true;
        if (ppu->ctrl & 0x80u) {
            ppu->nmi_pending = true;
        }
    } else if (ppu->scanline == 261) {
        ppu_record_sprite0_status_clear(ppu, true);
        ppu->status &= (uint8_t)~(PPU_STATUS_VBLANK | PPU_STATUS_SPRITE0_HIT);
        ppu->frame_ready = false;
        ppu->completed_frame_ready = false;
        ppu->scanline_ready = false;
        ppu->scanline_buffer.ready = false;
    }
}

static inline void ppu_finish_scanline(Ppu *ppu, NesCartridge *cartridge) {
    if (ppu->scanline >= 0 && ppu->scanline < NES_FRAME_HEIGHT) {
        uint64_t render_started_us = ppu_profile_now_us(ppu);
        uint32_t render_started_cycles = micrones_profile_now_cycles();
        bool counted_render = false;
        ppu_render_scanline(ppu, cartridge, ppu->scanline);
        if (render_started_us != 0) {
            ppu->step_profile.render_us_total += ppu_profile_now_us(ppu) - render_started_us;
            ++ppu->step_profile.scanline_render_count;
            counted_render = true;
        }
        if (render_started_cycles != 0) {
            ppu->step_profile.render_cycles_total +=
                (uint32_t)(micrones_profile_now_cycles() - render_started_cycles);
            if (!counted_render) {
                ++ppu->step_profile.scanline_render_count;
            }
        }
        if (ppu_rendering_enabled(ppu)) {
            ppu_increment_vertical_v(ppu);
            ppu_copy_horizontal_bits_from_temp(ppu);
        }
    }

    ++ppu->scanline;
    if (ppu->scanline > 261) {
        ppu->scanline = 0;
        ++ppu->frame_count;
#if MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
        ppu->sprite_composited_pixel_count = 0;
#if MICRONES_ENABLE_FRAMEBUFFER
        ppu->frame_buffer.frame_index = ppu->frame_count;
#endif
        memset(&ppu->render_artifact_diag, 0, sizeof(ppu->render_artifact_diag));
        ppu->visible_write_diag_count = 0;
        memset(ppu->visible_write_diag, 0, sizeof(ppu->visible_write_diag));
#endif
        ppu_latch_render_state(ppu);
        ppu_sprite0_diag_begin_frame(ppu);
    }
}

void MICRONES_HOT_FUNC(ppu_step_cycles)(Ppu *ppu, NesCartridge *cartridge, uint32_t cycles) {
    // Fast path: the vast majority of calls don't cross a scanline boundary.
    // Avoid loop and branch overhead by returning immediately for the common case.
    if (ppu->cycle != 0) {
        uint32_t advance = 341u - (uint32_t)ppu->cycle;
        if (advance > cycles) {
            ppu->cycle += (int)cycles;
            return;
        }
    }

    // Slow path: at scanline cycle 0, or the step spans a boundary.
    while (cycles > 0) {
        if (ppu->cycle == 0) {
            ppu->cycle = 1;
            --cycles;
            ppu_step_cycle_1(ppu);
            continue;
        }

        {
            uint32_t advance = 341u - (uint32_t)ppu->cycle;

            if (advance > cycles) {
                ppu->cycle += (int)cycles;
                break;
            }

            ppu->cycle += (int)advance;
            cycles -= advance;
        }

        ppu->cycle = 0;
        ppu_finish_scanline(ppu, cartridge);
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
        ppu_record_visible_write_diag(ppu, reg, value);
        ppu_refresh_visible_scanline_render_state(ppu);
        break;
    case PPU_REG_PPUMASK:
        ppu->mask = value;
        ppu_record_visible_write_diag(ppu, reg, value);
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
        ppu_record_visible_write_diag(ppu, reg, value);
        ppu_refresh_visible_scanline_render_state(ppu);
        break;
    case PPU_REG_PPUADDR:
        if (!ppu->write_toggle) {
            ppu->temp_addr = (uint16_t)((ppu->temp_addr & 0x00ffu) | (((uint16_t)value & 0x3fu) << 8));
        } else {
            ppu->temp_addr = (uint16_t)((ppu->temp_addr & 0xff00u) | value);
            ppu->vram_addr = ppu->temp_addr;
        }
        ppu->write_toggle = !ppu->write_toggle;
        ppu_record_visible_write_diag(ppu, reg, value);
        ppu_refresh_visible_scanline_render_state(ppu);
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
