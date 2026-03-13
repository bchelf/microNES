#ifndef SMB2350_PPU_H
#define SMB2350_PPU_H

#include "cart.h"
#include "framebuffer.h"
#include "scanline.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    PPU_SPRITE0_DIAG_MAX_FRAMES = 64,
    PPU_SPRITE0_DIAG_MAX_EXAMPLES = 6,
    PPU_RENDER_ARTIFACT_TILE_WINDOW = 8,
    PPU_VISIBLE_WRITE_DIAG_MAX = 32,
};

typedef enum {
    PPU_SPRITE0_REJECT_NONE = 0,
    PPU_SPRITE0_REJECT_RENDER_DISABLED,
    PPU_SPRITE0_REJECT_LEFT_MASK,
    PPU_SPRITE0_REJECT_SPRITE_TRANSPARENT,
    PPU_SPRITE0_REJECT_BG_TRANSPARENT,
    PPU_SPRITE0_REJECT_X255,
    PPU_SPRITE0_REJECT_NOT_IN_SCANLINE_SELECTION,
    PPU_SPRITE0_REJECT_OCCLUDED_BY_EARLIER_SPRITE,
} PpuSprite0RejectReason;

typedef enum {
    PPU_OAM_UPDATE_NONE = 0,
    PPU_OAM_UPDATE_OAMDATA = 1,
    PPU_OAM_UPDATE_DMA = 2,
} PpuOamUpdateSource;

typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t reason;
} PpuSprite0DiagExample;

typedef struct {
    bool valid;
    bool completed;
    uint64_t render_frame_index;
    uint64_t completed_frame_index;
    uint8_t sprite_y;
    uint8_t sprite_tile;
    uint8_t sprite_attributes;
    uint8_t sprite_x;
    uint8_t ctrl;
    uint8_t mask;
    uint8_t status;
    uint8_t scroll_x;
    uint8_t scroll_y;
    uint16_t render_vram_addr;
    uint8_t render_fine_x;
    uint8_t render_scroll_x;
    uint8_t render_scroll_y;
    uint8_t render_base_nametable;
    uint16_t vram_addr;
    uint16_t temp_addr;
    uint8_t fine_x;
    bool show_bg;
    bool show_sprites;
    bool show_bg_left;
    bool show_sprites_left;
    bool sprite0_intersects_visible;
    uint8_t sprite0_visible_scanline_count;
    uint8_t sprite0_selected_scanline_count;
    uint8_t sprite0_hit_set_count;
    bool sprite0_hit_set_this_frame;
    int first_hit_x;
    int first_hit_y;
    uint32_t visible_candidate_pixels;
    uint32_t sprite0_opaque_pixels_raw;
    uint32_t bg_opaque_pixels_in_sprite_bounds;
    uint32_t raw_overlap_pixels;
    uint32_t effective_overlap_pixels;
    uint32_t reject_render_disabled;
    uint32_t reject_left_mask;
    uint32_t reject_sprite_transparent;
    uint32_t reject_bg_transparent;
    uint32_t reject_x255;
    uint32_t reject_not_in_scanline_selection;
    uint32_t reject_occluded_by_earlier_sprite;
    bool first_raw_overlap_valid;
    int first_raw_overlap_x;
    int first_raw_overlap_y;
    bool first_effective_overlap_valid;
    int first_effective_overlap_x;
    int first_effective_overlap_y;
    uint8_t sprite0_oam_changed_mask;
    uint64_t last_sprite0_oam_update_render_frame;
    int last_sprite0_oam_update_scanline;
    int last_sprite0_oam_update_cycle;
    uint8_t last_sprite0_oam_update_source;
    uint8_t example_count;
    PpuSprite0DiagExample examples[PPU_SPRITE0_DIAG_MAX_EXAMPLES];
} PpuSprite0FrameDiag;

typedef struct {
    bool enabled;
    uint64_t frame_start;
    uint64_t frame_end;
    PpuSprite0FrameDiag current_frame;
    PpuSprite0FrameDiag frames[PPU_SPRITE0_DIAG_MAX_FRAMES];
    uint8_t frame_count;
    bool first_hit_frame_valid;
    PpuSprite0FrameDiag first_hit_frame;
    uint64_t total_status_set_count;
    uint64_t total_status_clear_count;
    uint64_t suspicious_status_clear_count;
    uint64_t last_sprite0_oam_update_render_frame;
    int last_sprite0_oam_update_scanline;
    int last_sprite0_oam_update_cycle;
    uint8_t last_sprite0_oam_update_source;
    uint64_t last_status_set_render_frame;
    int last_status_set_scanline;
    int last_status_set_cycle;
    uint64_t last_status_clear_render_frame;
    int last_status_clear_scanline;
    int last_status_clear_cycle;
} PpuSprite0Diag;

typedef struct {
    uint8_t tile_x;
    uint8_t coarse_x;
    uint8_t coarse_y;
    uint8_t fine_y;
    uint8_t nametable;
    uint16_t nametable_addr;
    uint16_t attribute_addr;
    uint16_t pattern_base;
    uint8_t tile_index;
    uint16_t pattern_addr;
    uint8_t pattern_low;
    uint8_t pattern_high;
} PpuRenderTileDiag;

typedef struct {
    bool valid;
    uint64_t frame_index;
    uint16_t scanline;
    uint16_t equal_prev_pixels;
    uint16_t repeated_prev_chunks;
    uint16_t transitions;
    uint16_t longest_run;
    uint16_t focus_x;
    uint8_t tile_count;
    PpuRenderTileDiag tiles[PPU_RENDER_ARTIFACT_TILE_WINDOW];
} PpuRenderArtifactDiag;

typedef struct {
    uint64_t frame_index;
    uint16_t scanline;
    uint16_t cycle;
    uint8_t reg;
    uint8_t value;
    uint16_t vram_addr;
    uint16_t temp_addr;
    uint8_t fine_x;
    uint8_t ctrl;
    uint8_t mask;
} PpuVisibleWriteDiag;

typedef struct {
    uint8_t ctrl;
    uint8_t mask;
    uint8_t status;
    uint8_t oam_addr;
    uint8_t oam[256];
    uint8_t nametables[2048];
    uint8_t palette[32];
    uint8_t read_buffer;
    uint16_t vram_addr;
    uint16_t temp_addr;
    uint8_t fine_x;
    bool write_toggle;
    uint8_t scroll_x;
    uint8_t scroll_y;
    uint16_t render_vram_addr;
    uint8_t render_fine_x;
    uint8_t render_scroll_x;
    uint8_t render_scroll_y;
    uint8_t render_base_nametable;
    int scanline;
    int cycle;
    uint64_t frame_count;
    bool frame_ready;
    bool scanline_ready;
    bool nmi_pending;
    uint64_t completed_frame_count;
    bool completed_frame_ready;
    uint64_t last_completed_frame_hash;
    uint32_t last_completed_nonzero_pixels;
    uint64_t first_nonblank_frame_index;
    uint64_t first_nonblank_frame_hash;
    bool sprite0_hit_ever;
    uint64_t sprite0_hit_count;
    uint64_t sprite0_opaque_pixel_count;
    uint64_t sprite0_background_overlap_count;
    uint64_t first_sprite0_hit_frame;
    int first_sprite0_hit_scanline;
    int first_sprite0_hit_x;
    uint64_t first_sprite0_opaque_frame;
    int first_sprite0_opaque_scanline;
    int first_sprite0_opaque_x;
    uint64_t sprite_composited_pixel_count;
    uint64_t frames_with_sprite_pixels;
    uint32_t last_completed_sprite_pixels;
    uint64_t first_frame_with_sprite_pixels;
    uint8_t max_scanline_sprite_count;
    PpuSprite0Diag sprite0_diag;
    PpuRenderArtifactDiag render_artifact_diag;
    PpuVisibleWriteDiag visible_write_diag[PPU_VISIBLE_WRITE_DIAG_MAX];
    uint8_t visible_write_diag_count;
    PpuVisibleWriteDiag last_completed_visible_write_diag[PPU_VISIBLE_WRITE_DIAG_MAX];
    uint8_t last_completed_visible_write_diag_count;
    NesFrameBuffer frame_buffer;
    NesScanline scanline_buffer;
} Ppu;

void ppu_init(Ppu *ppu);
void ppu_reset(Ppu *ppu);
void ppu_set_sprite0_diag_window(Ppu *ppu, uint64_t frame_start, uint64_t frame_end);
void ppu_step_cycles(Ppu *ppu, NesCartridge *cartridge, uint32_t cycles);
uint8_t ppu_cpu_read(Ppu *ppu, NesCartridge *cartridge, uint16_t addr);
void ppu_cpu_write(Ppu *ppu, NesCartridge *cartridge, uint16_t addr, uint8_t value);
void ppu_oam_write_byte(Ppu *ppu, uint8_t index, uint8_t value, bool via_dma);

const NesFrameBuffer *ppu_framebuffer(const Ppu *ppu);
const NesScanline *ppu_scanline(const Ppu *ppu);
const PpuSprite0Diag *ppu_sprite0_diag(const Ppu *ppu);

#endif
