#include "smb1_bg_classifier.h"

/* =====================================================================
 * SMB1 background-tile allowlist
 *
 * Derived empirically from nametable analysis at representative frames
 * covering: title card, World 1-1 overworld, underground bonus rooms,
 * end-of-level staircase, and flagpole.
 *
 * Default = false (decorative / transparent).
 * Explicit true entries = solid/visible tiles (ground, bricks, pipes,
 * blocks, stairs, coins, flagpole, title card, vines).
 *
 * Tiles left false (transparent):
 *   0x24       blank sky
 *   0x25       decorative scenery (cloud/hill fill)
 *   0x35–0x3C  cloud/hill tops and bases
 *   0xCE       HUD symbol variant (unused in visible text)
 */

/* One entry per possible CHR tile index (0x00..0xFF).
 * true  = keep the pixel (interactive tile)
 * false = emit the transparency sentinel (decorative tile)        */
static const bool k_smb1_bg_tile_interactive[256] = {
    /* HUD character set — digits 0–9 (0x00–0x09), letters A–Z (0x0A–0x23).
     * Made visible so that score, lives count, world number, "GAME OVER",
     * and all other on-screen text renders opaque on every screen. */
    [0x00] = true, [0x01] = true, [0x02] = true, [0x03] = true,
    [0x04] = true, [0x05] = true, [0x06] = true, [0x07] = true,
    [0x08] = true, [0x09] = true,
    [0x0A] = true, [0x0B] = true, [0x0C] = true, [0x0D] = true,
    [0x0E] = true, [0x0F] = true, [0x10] = true, [0x11] = true,
    [0x12] = true, [0x13] = true, [0x14] = true, [0x15] = true,
    [0x16] = true, [0x17] = true, [0x18] = true, [0x19] = true,
    [0x1A] = true, [0x1B] = true, [0x1C] = true, [0x1D] = true,
    [0x1E] = true, [0x1F] = true, [0x20] = true, [0x21] = true,
    [0x22] = true, [0x23] = true,

    /* HUD punctuation — dash (0x2E), coin/× markers (0x28, 0x29) */
    [0x28] = true, [0x29] = true, [0x2E] = true,

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

    /* pipes — vertical cap and shaft; 0x26 is the pipe-shaft inner tile
     * (confirmed: nametable shows shaft rows as 0x68 0x69 0x26 0x6A) */
    [0x26] = true,
    [0x60] = true, [0x61] = true, [0x62] = true, [0x63] = true,
    [0x64] = true, [0x65] = true, [0x66] = true, [0x67] = true,
    [0x68] = true, [0x69] = true, [0x6A] = true,
    [0x6B] = true, [0x6C] = true, [0x6D] = true, [0x6E] = true, [0x6F] = true,

    /* pipes — horizontal body (underground); 0xA5/0xA6 top row, 0xA7/0xA8 bottom row */
    [0xA5] = true, [0xA6] = true, [0xA7] = true, [0xA8] = true,

    /* pipes — horizontal cap/junction (where horizontal meets vertical):
     * top edge: 0x86-0x89, upper body: 0x8A-0x8D,
     * lower body: 0x8E-0x90, bottom edge: 0x91-0x94 */
    [0x86] = true, [0x87] = true, [0x88] = true, [0x89] = true,
    [0x8A] = true, [0x8B] = true, [0x8C] = true, [0x8D] = true,
    [0x8E] = true, [0x8F] = true, [0x90] = true,
    [0x91] = true, [0x92] = true, [0x93] = true, [0x94] = true,

    /* underground / bonus-room ground */
    [0x84] = true, [0x85] = true,

    /* end-of-level staircase — confirmed from nametable at flagpole frame:
     * 0x30 = left outer edge, 0x31 = top-left cap, 0x32 = top-right cap,
     * 0x33 = right outer edge, 0x34 = inner step face;
     * 0x26 (already listed above under pipes) is the stair fill tile */
    [0x30] = true, [0x31] = true, [0x32] = true, [0x33] = true, [0x34] = true,

    /* flagpole — confirmed from nametable:
     * 0x2F/0x3D are the top cap (row 5),
     * 0xA2/0xA3 are the repeating shaft columns (rows 6-23),
     * 0xAB/0xAC/0xAD/0xAE are the base block (rows 24-25) */
    [0x2F] = true, [0x3D] = true,
    [0xA2] = true, [0xA3] = true,
    [0xAB] = true, [0xAC] = true, [0xAD] = true, [0xAE] = true,

    /* mushroom platforms — confirmed from nametable frame 11925:
     * top surface = A5/A6/A7/A8 (horizontal pipe, already listed above)
     * underside row 1: 0x6C/0x6D (repeating) + 0x6E/0x6F (right edge) — 6C listed above
     * underside row 2: 0x70 (junction/left) + 0x71/0x72 (repeating) + 0x73/0x74 (right edge)
     * stem connector:  0x75/0x76
     * stem (0xBA/0xBB) omitted — also appears as decorative fence rail in 1-2
     * junction tiles between flagpole base and mushroom: 0x2C/0x2D */
    [0x2C] = true, [0x2D] = true,
    [0x70] = true, [0x71] = true, [0x72] = true, [0x73] = true, [0x74] = true,
    [0x75] = true, [0x76] = true,

    /* title card border — confirmed from nametable frame 219:
     * 0x44 = top-left corner, 0x48 = top edge, 0x49 = top-right corner,
     * 0x46 = left edge, 0x4A = right edge */
    [0x44] = true, [0x46] = true,
    [0x48] = true, [0x49] = true, [0x4A] = true,

    /* title card interior misc tiles (0x42/0x43 appear inside card body) */
    [0x42] = true, [0x43] = true,

    /* title card artwork — confirmed from nametable frame 219:
     * 0xD0–0xE8 are the mario/logo/castle graphic tiles filling the card */
    [0xD0] = true, [0xD1] = true, [0xD2] = true, [0xD3] = true,
    [0xD4] = true, [0xD5] = true, [0xD6] = true, [0xD7] = true,
    [0xD8] = true, [0xD9] = true, [0xDA] = true, [0xDB] = true,
    [0xDC] = true, [0xDD] = true, [0xDE] = true, [0xDF] = true,
    [0xE0] = true, [0xE1] = true, [0xE2] = true, [0xE3] = true,
    [0xE4] = true, [0xE5] = true, [0xE6] = true, [0xE7] = true,
    [0xE8] = true,

    /* copyright symbol (row 15 of title screen: "©1985 NINTENDO") */
    [0xCF] = true,

    /* bullet bill shooters — confirmed from nametable frame 16929:
     * 2×4 tile block: C6/C7 (top) through CC/CD (bottom) */
    [0xC6] = true, [0xC7] = true, [0xC8] = true, [0xC9] = true,
    [0xCA] = true, [0xCB] = true, [0xCC] = true, [0xCD] = true,

    /* castle — confirmed from nametable at end-of-level frame:
     * 0x9D/0x9E = battlements/crenellations (top of each tower level),
     * 0xA9/0xAA = castle wall blocks,
     * 0x9B/0x9C = door arch,
     * 0x27 = arrow-slit windows and door interior */
    [0x27] = true,
    [0x9B] = true, [0x9C] = true, [0x9D] = true, [0x9E] = true,
    [0xA9] = true, [0xAA] = true,
};

bool smb1_bg_tile_is_interactive(uint8_t tile_index, void *user) {
    (void)user;
    return k_smb1_bg_tile_interactive[tile_index];
}
