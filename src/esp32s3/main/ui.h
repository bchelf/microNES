#pragma once

#include <stdint.h>

// Draw the static controller overlay onto the display.
//
// This function writes to the hardware display directly:
//   Left  zone (0..139   × 0..239): D-pad
//   Right zone (396..535 × 0..239): A, B, Start, Select
//
// Call once after display_init() and before the main emulator loop.
// The AMOLED panel retains the image; subsequent NES frames only
// update the centre region.
void ui_draw_overlay(void);
