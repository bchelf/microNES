#ifndef MICRONES_PICO_INPUT_H
#define MICRONES_PICO_INPUT_H

#include "input.h"

// NES hardware controller GPIO pins (original 4021 controller).
// Connect the 7-pin NES controller cable as follows:
//   NES pin 1 (VCC)   → 3.3 V
//   NES pin 2 (CLOCK) → GP5 (output from Pico)
//   NES pin 3 (LATCH) → GP6 (output from Pico)
//   NES pin 4 (DATA)  → GP7 (input to Pico; pull-up enabled in firmware)
//   NES pin 7 (GND)   → GND
// The controller now lives on one contiguous 3-pin block clear of both TFT
// display mappings and the MAX98357 audio backend.
#define PICO_NES_CTL_CLOCK  5u
#define PICO_NES_CTL_LATCH  6u
#define PICO_NES_CTL_DATA   7u

// Initialize all controller GPIOs (call once at startup).
void pico_input_init(void);

// Read all buttons and return the current controller state.
NesControllerState pico_input_read(void);

#endif
