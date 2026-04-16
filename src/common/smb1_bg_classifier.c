#include "smb1_bg_classifier.h"

/* =====================================================================
 * SMB1 background-tile allowlist — STUB
 *
 * Goal: return true for tile indices that represent anything Mario can
 * physically stand on, bump, break, or otherwise interact with.  Return
 * false for tile indices that are purely visual (sky, clouds, bushes,
 * hills, castles in the backdrop, HUD lettering you want hidden).
 *
 * This file ships as a STUB.  The correct tile indices must be derived
 * from the SMB1 ROM's CHR-ROM and the game's own metatile tables.  Two
 * reasonable strategies:
 *
 *   1. CHR visual inspection.  Render the 256-entry CHR pattern bank
 *      used for background tiles (the one selected by PPUCTRL bit 4
 *      during gameplay) and hand-pick the indices for:
 *        - ground top, ground middle, ground cap
 *        - brick (solid + broken states)
 *        - question block (4 animation frames + empty)
 *        - pipe pieces (top-left, top-right, body-left, body-right)
 *        - stair/step blocks
 *        - underground platform blocks
 *        - flagpole shaft + ball
 *        - used/block tile (solid after being hit)
 *        - coin (background tile, animated)
 *        - axe (castle end-of-level)
 *        - trampoline/spring (if treated as BG)
 *
 *   2. Disassembly cross-reference.  The SMB1 disassembly exposes a
 *      BlockBufferCollisionData table keyed by metatile id.  Each
 *      metatile decomposes into 2x2 CHR tile indices via
 *      MetatileGraphics_Low/High.  Walking metatiles with non-zero
 *      collision class yields the canonical interactive-tile set.
 *
 * Until the table below is filled in, this function returns true for
 * every tile, which preserves the default opaque rendering.  The
 * transparency feature becomes a visible no-op in that state — exactly
 * what we want as a safe default so smoke determinism is unaffected. */

/* One entry per possible CHR tile index (0x00..0xFF).  true = keep the
 * pixel, false = emit the transparency sentinel. */
static const bool k_smb1_bg_tile_interactive[256] = {
    /* STUB: leave everything true until the allowlist is authored
     * against the ROM.  Flip the specific indices to false for the
     * decorative tiles (sky, clouds, bushes, hills, castle backdrop,
     * HUD letters/digits).  Example slots to populate once known:
     *
     *   [0x24] = false,  // blank sky (most common decorative tile)
     *   [0x47] = false,  // cloud upper-left
     *   [0x48] = false,  // cloud upper-middle
     *   ...
     *
     * And mark the interactive tiles true (they already default true,
     * so this is implicit, but documenting here):
     *
     *   [0x54] = true,   // ground block (example - verify against ROM)
     *   [0xAA] = true,   // brick (example - verify against ROM)
     *   ...
     */
    [0 ... 255] = true,
};

bool smb1_bg_tile_is_interactive(uint8_t tile_index, void *user) {
    (void)user;
    return k_smb1_bg_tile_interactive[tile_index];
}
