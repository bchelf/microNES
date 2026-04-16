#ifndef MICRONES_PPU_H
#define MICRONES_PPU_H

#include "cart.h"
#include "framebuffer.h"
#include "runtime_config.h"
#include "scanline.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    PPU_SPRITE0_DIAG_MAX_FRAMES = 64,
    PPU_SPRITE0_DIAG_MAX_EXAMPLES = 6,
    PPU_RENDER_ARTIFACT_TILE_WINDOW = 8,
    PPU_VISIBLE_WRITE_DIAG_MAX = 32,
};

/* Sentinel color value written to the framebuffer when a BG pixel is
 * classified as non-interactive (see ppu_set_bg_tile_classifier).  Valid
 * NES palette indices are 0x00..0x3F, so 0xFE cannot collide with real
 * palette output.  Host frontends interpret this value as "fully
 * transparent" when rendering into a composited window. */
#define PPU_COLOR_TRANSPARENT 0xFEu

/* Classifier callback: given a background nametable tile index, return
 * true if the tile represents something the player can physically
 * interact with (ground, bricks, pipes, blocks, etc.) and should remain
 * visible, or false for decorative background (sky, clouds, hills, HUD
 * you want hidden, etc.).  The classifier is invoked once per covering
 * tile per scanline, not per pixel. */
typedef bool (*PpuBgTileClassifier)(uint8_t tile_index, void *user);

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
    uint64_t step_calls;
    uint64_t cycles_requested;
    uint64_t event_us_total;
    uint64_t render_us_total;
    uint64_t render_cycles_total;
    uint64_t scanline_render_count;
} PpuStepProfile;

typedef struct {
    /* ── Hot scalars first: Xtensa LX7 can address these with a single L32I /
     * L8UI from the Nes struct base (Ppu starts at Nes offset ~36, so all
     * fields within the first ~984 bytes of Ppu are within the L32I direct
     * immediate range of ≤ 1020 bytes).
     *
     * Previously scanline/cycle were buried at Ppu offset ~2360 (behind the
     * oam/nametable/palette arrays), requiring an ADDMI+L32I two-instruction
     * sequence.  Moving them here cuts that to a single L32I. */
    int scanline;                        /* Ppu+0:  updated every ppu_step_cycles call */
    int cycle;                           /* Ppu+4:  maintained by slow path; stale in fast path */
    /* cycles_remaining = 341 - cycle when cycle > 0, else 0.  Maintained by
     * ppu_step_cycles_try_fast (hot path) and ppu_step_cycles (slow path).
     * Replaces the per-call "advance = 341 - cycle" subtraction in try_fast. */
    int32_t cycles_remaining;            /* Ppu+8:  countdown to next scanline boundary */
    bool frame_ready;                    /* Ppu+12 */
    bool scanline_ready;                 /* Ppu+13: tested every CPU instruction */
    bool nmi_pending;                    /* Ppu+14 */
    bool completed_frame_ready;          /* Ppu+15 */
    uint8_t ctrl;                        /* Ppu+16 */
    uint8_t mask;                        /* Ppu+17 */
    uint8_t status;                      /* Ppu+18 */
    uint8_t oam_addr;                    /* Ppu+19 */
    uint8_t read_buffer;                 /* Ppu+20 */
    uint8_t fine_x;                      /* Ppu+21 */
    bool write_toggle;                   /* Ppu+22 */
    uint8_t scroll_x;                    /* Ppu+23 */
    uint8_t scroll_y;                    /* Ppu+24 */
    uint8_t render_fine_x;               /* Ppu+25 */
    uint8_t render_scroll_x;             /* Ppu+26 */
    uint8_t render_scroll_y;             /* Ppu+27 */
    uint8_t render_base_nametable;       /* Ppu+28 */
    bool sprite0_hit_ever;               /* Ppu+29 */
    uint8_t max_scanline_sprite_count;   /* Ppu+30 */
    uint8_t visible_write_diag_count;    /* Ppu+31 */
    uint8_t last_completed_visible_write_diag_count; /* Ppu+32 */
    /* 1 byte implicit pad → */
    uint16_t vram_addr;                  /* Ppu+30 */
    uint16_t temp_addr;                  /* Ppu+32 */
    uint16_t render_vram_addr;           /* Ppu+34 */
    /* 4 bytes implicit pad for uint64_t alignment → */
    uint64_t frame_count;                /* Ppu+40 */
    uint64_t completed_frame_count;      /* Ppu+48 */
    uint64_t last_completed_frame_hash;  /* Ppu+56 */
    uint32_t last_completed_nonzero_pixels; /* Ppu+64 */
    uint32_t last_completed_sprite_pixels;  /* Ppu+68 */
    uint64_t first_nonblank_frame_index; /* Ppu+72 */
    uint64_t first_nonblank_frame_hash;  /* Ppu+80 */
    uint64_t sprite0_hit_count;          /* Ppu+88 */
    uint64_t sprite0_opaque_pixel_count; /* Ppu+96 */
    uint64_t sprite0_background_overlap_count; /* Ppu+104 */
    uint64_t first_sprite0_hit_frame;    /* Ppu+112 */
    int first_sprite0_hit_scanline;      /* Ppu+120 */
    int first_sprite0_hit_x;             /* Ppu+124 */
    uint64_t first_sprite0_opaque_frame; /* Ppu+128 */
    int first_sprite0_opaque_scanline;   /* Ppu+136 */
    int first_sprite0_opaque_x;          /* Ppu+140 */
    uint64_t sprite_composited_pixel_count; /* Ppu+144 */
    uint64_t frames_with_sprite_pixels;  /* Ppu+152 */
    uint64_t first_frame_with_sprite_pixels; /* Ppu+160 */
    /* ── Large tables pushed below the hot scalars ── */
    uint8_t oam[256];                    /* Ppu+168 */
    uint8_t nametables[2048];            /* Ppu+424 */
    uint8_t palette[32];                 /* Ppu+2472 */
    /* ── Diagnostics and frame buffer (cold, large) ── */
    PpuSprite0Diag sprite0_diag;
    PpuRenderArtifactDiag render_artifact_diag;
    PpuVisibleWriteDiag visible_write_diag[PPU_VISIBLE_WRITE_DIAG_MAX];
    PpuVisibleWriteDiag last_completed_visible_write_diag[PPU_VISIBLE_WRITE_DIAG_MAX];
    micrones_profile_now_us_fn profile_now_us;
    void *profile_now_user;
    PpuStepProfile step_profile;
    NesFrameBuffer frame_buffer;
    NesFrameBuffer *active_frame_buffer; /* render target; points to frame_buffer by default */
    NesScanline scanline_buffer;
    /* Optional non-interactive BG tile filter.  When set, any BG pixel
     * whose covering nametable tile fails the classifier is replaced with
     * PPU_COLOR_TRANSPARENT in the framebuffer before sprite composition
     * reads it.  Sprite-0 hit and other diagnostics see the real palette
     * values, not the sentinel. */
    PpuBgTileClassifier bg_tile_classifier;
    void *bg_tile_classifier_user;
} Ppu;

void ppu_init(Ppu *ppu);
void ppu_reset(Ppu *ppu);
void ppu_set_render_target(Ppu *ppu, NesFrameBuffer *fb);
void ppu_set_bg_tile_classifier(Ppu *ppu, PpuBgTileClassifier fn, void *user);
void ppu_set_sprite0_diag_window(Ppu *ppu, uint64_t frame_start, uint64_t frame_end);
void ppu_step_cycles(Ppu *ppu, NesCartridge *cartridge, uint32_t cycles);

// Inline fast path for ppu_step_cycles.  Returns true if the scanline did not
// end (~98 % of calls).  Uses the pre-computed cycles_remaining countdown so
// the hot path is: L32I rem / BLTU / SUB / S32I (4 instructions) rather than
// the 7-instruction "load cycle, compute 341-cycle, compare, add, store" sequence.
// When false the caller must call ppu_step_cycles() – which reconstructs
// ppu->cycle from cycles_remaining before running the slow-path logic.
static inline bool ppu_step_cycles_try_fast(Ppu *ppu, uint32_t cycles) {
    uint32_t rem = (uint32_t)ppu->cycles_remaining;
    if (__builtin_expect(rem > cycles, 1)) {
        ppu->cycles_remaining = (int32_t)(rem - cycles);
        return true;
    }
    return false;
}
uint8_t ppu_cpu_read(Ppu *ppu, NesCartridge *cartridge, uint16_t addr);
void ppu_cpu_write(Ppu *ppu, NesCartridge *cartridge, uint16_t addr, uint8_t value);
void ppu_oam_write_byte(Ppu *ppu, uint8_t index, uint8_t value, bool via_dma);

const NesFrameBuffer *ppu_framebuffer(const Ppu *ppu);
const NesScanline *ppu_scanline(const Ppu *ppu);
const PpuSprite0Diag *ppu_sprite0_diag(const Ppu *ppu);

#endif
