#ifndef MICRONES_PICO_INPUT_H
#define MICRONES_PICO_INPUT_H

#include "input.h"

// NES hardware controller GPIO pins (original 4021 controller).
// Connect the 7-pin NES controller cable as follows:
//   NES pin 1 (VCC)   → 3.3 V
//   NES pin 2 (CLOCK) → GP3  (output from Pico)
//   NES pin 3 (LATCH) → GP4  (output from Pico)
//   NES pin 4 (DATA)  → GP6  (input to Pico; pull-up enabled in firmware)
//   NES pin 7 (GND)   → GND
// GP0/GP1 (video), GP2 (spare button), and GP5 (audio) remain free.
#define PICO_NES_CTL_CLOCK  3u
#define PICO_NES_CTL_LATCH  4u
#define PICO_NES_CTL_DATA   6u

// Initialize all controller GPIOs (call once at startup).
void pico_input_init(void);

// Read all buttons and return the current controller state.
NesControllerState pico_input_read(void);

#endif
