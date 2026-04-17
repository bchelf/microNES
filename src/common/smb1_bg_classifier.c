#include "smb1_bg_classifier.h"

/* =====================================================================
 * SMB1 background-tile allowlist
 *
 * Derived empirically from CHR bank 0 visual inspection combined with
 * nametable analysis at representative gameplay frames (frames 600–1200,
 * covering the World 1-1 demo with ground, pipes, bricks, question
 * blocks, and castle backdrop).
 *
 * Default = false (decorative / transparent).
 * Explicit true entries = interactive (ground, bricks, pipes, blocks,
 * stairs, coins, flagpole, vines).
 *
 * Tiles left false (transparent):
 *   0x00–0x23  HUD character set (letters A–Z, digits 0–9, symbols)
 *   0x24       blank sky
 *   0x25–0x38  hills, bushes, clouds, decorative scenery
 *   0x39–0x3C  cloud base row
 *   0x42–0x44, 0x46, 0x48–0x4A  castle edge/tower decorations
 *   0x28, 0x29, 0x2E  HUD punctuation
 *   0xCE, 0xCF  HUD symbol variants
 *   0xD0–0xE8  castle wall tiles
 */

/* One entry per possible CHR tile index (0x00..0xFF).
 * true  = keep the pixel (interactive tile)
 * false = emit the transparency sentinel (decorative tile)        */
static const bool k_smb1_bg_tile_interactive[256] = {
    /* ground — overworld main (rows 26–29 in every overworld frame) */
    [0xB4] = true, [0xB5] = true, [0xB6] = true, [0xB7] = true,

    /* ground — castle / underground level base */
    [0x5F] = true,
    [0x78] = true, [0x79] = true, [0x7A] = true,
    [0x95] = true, [0x96] = true, [0x97] = true, [0x98] = true,

    /* bricks */
    [0x45] = true, [0x47] = true, [0x52] = true,

    /* question blocks (all animation frames + empty/used state) */
    [0x53] = true, [0x54] = true, [0x55] = true, [0x56] = true,
    [0x57] = true, [0x58] = true, [0x59] = true, [0x5A] = true,
    [0x5B] = true,

    /* coins (background tile, animated frames) */
    [0x50] = true, [0x74] = true, [0x75] = true, [0x76] = true,

    /* pipes — cap and shaft; 0x26 is the pipe-shaft inner tile
     * (confirmed: nametable shows shaft rows as 0x68 0x69 0x26 0x6A) */
    [0x26] = true,
    [0x60] = true, [0x61] = true, [0x62] = true, [0x63] = true,
    [0x64] = true, [0x65] = true, [0x66] = true, [0x67] = true,
    [0x68] = true, [0x69] = true, [0x6A] = true,
    [0x6B] = true, [0x6C] = true,

    /* underground / bonus-room ground */
    [0x84] = true, [0x85] = true, [0x86] = true, [0x87] = true,

    /* staircase / step blocks — 0x7E/0x7F are the solid step fill;
     * 0x70/0x71 are the exposed face profiles (triangular staircase silhouette)
     * visible on the end-of-level staircase; 0x72/0x73 are the block surfaces
     * that form the step tops and flagpole-base block faces */
    [0x70] = true, [0x71] = true, [0x72] = true, [0x73] = true,
    [0x7E] = true, [0x7F] = true,

    /* vine / flagpole shaft */
    [0x81] = true, [0x83] = true,
};

bool smb1_bg_tile_is_interactive(uint8_t tile_index, void *user) {
    (void)user;
    return k_smb1_bg_tile_interactive[tile_index];
}
