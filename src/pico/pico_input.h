#ifndef MICRONES_PICO_INPUT_H
#define MICRONES_PICO_INPUT_H

#include "input.h"

// NES hardware controller GPIO pins (original 4021 controller).
// Connect the 7-pin NES controller cable as follows:
//   NES pin 1 (VCC)   → 3.3 V
//   NES pin 2 (CLOCK) → GP20 (output from Pico)
//   NES pin 3 (LATCH) → GP19 (output from Pico)
//   NES pin 4 (DATA)  → GP18 (input to Pico; pull-up enabled in firmware)
//   NES pin 7 (GND)   → GND
// The controller lives on a contiguous 3-pin block in the upper-right
// of the Pico header, clear of the SD card (GP2-5), video DAC (GP10-14),
// and the audio output (GP16).
#define PICO_NES_CTL_CLOCK  20u
#define PICO_NES_CTL_LATCH  19u
#define PICO_NES_CTL_DATA   18u

// Initialize all controller GPIOs (call once at startup).
void pico_input_init(void);

// Read all buttons and return the current controller state.
NesControllerState pico_input_read(void);

#endif
