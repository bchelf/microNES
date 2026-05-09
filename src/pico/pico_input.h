#ifndef MICRONES_PICO_INPUT_H
#define MICRONES_PICO_INPUT_H

#include "input.h"

// NES hardware controller GPIO pins (original 4021 controller).
// Connect the 7-pin NES controller cable as follows:
//   NES pin 1 (VCC)   → 3.3 V
//   NES pin 2 (CLOCK) → GP6 (output from Pico)
//   NES pin 3 (LATCH) → GP7 (output from Pico)
//   NES pin 4 (DATA)  → GP8 (input to Pico; pull-up enabled in firmware)
//   NES pin 7 (GND)   → GND
// The controller lives on one contiguous 3-pin block clear of the SD card
// (GP2-5), TFT display backends, and the audio outputs.
#define PICO_NES_CTL_CLOCK  6u
#define PICO_NES_CTL_LATCH  7u
#define PICO_NES_CTL_DATA   8u

// Initialize all controller GPIOs (call once at startup).
void pico_input_init(void);

// Read all buttons and return the current controller state.
NesControllerState pico_input_read(void);

#endif
