#pragma once

#include "touch.h"
#include "input.h"  // NES_BUTTON_* constants

// Map a set of touch points (in landscape display coordinates) to a
// NesControllerState bitmask.
//
// Layout (landscape 536×240):
//   Left  zone [  0..139] × [0..239] – D-pad
//   Right zone [396..535] × [0..239] – A / B / Start / Select
NesControllerState nes_input_from_touch(const TouchData *touch);
