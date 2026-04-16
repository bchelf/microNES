#ifndef MICRONES_SMB1_BG_CLASSIFIER_H
#define MICRONES_SMB1_BG_CLASSIFIER_H

#include <stdbool.h>
#include <stdint.h>

/* SMB1-specific classifier that decides whether a background nametable
 * tile index is "interactive" (ground, bricks, pipes, blocks, stairs,
 * question blocks, coins, flagpole...) versus "decorative" (sky, clouds,
 * hills, bushes, castles, HUD).  The result is consumed by
 * ppu_set_bg_tile_classifier() and drives the transparency sentinel in
 * the PPU compositor.
 *
 * The allowlist is intentionally hand-curated against the SMB1 CHR ROM
 * and is not portable to other games.  The `user` argument is ignored. */
bool smb1_bg_tile_is_interactive(uint8_t tile_index, void *user);

#endif
