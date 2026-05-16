#ifndef MICRONES_PICO_INPUT_H
#define MICRONES_PICO_INPUT_H

#include "input.h"

// NES hardware controller GPIO pins (original 4021 controller).
//
// Breadboard default (matches the legacy breadboard rig):
//   NES pin 2 (CLOCK) → GP20    NES pin 3 (LATCH) → GP19
//   NES pin 4 (DATA)  → GP18
//
// v0.1 PCB (build with -DMICRONES_BOARD=v0_1):
//   NES pin 2 (CLOCK) → GP6  (NES_CLOCK in the schematic)
//   NES pin 3 (LATCH) → GP7  (NES_LATCH)
//   NES pin 4 (DATA)  → GP8  (NES_DATA_1)
//   (the PCB also routes a second-player DATA to GP9, NES_DATA_2,
//    available as MICRONES_V0_1_PIN_NES_DATA_P2 but not yet used by
//    the firmware.)
//
// Common wiring (both boards):
//   NES pin 1 (VCC)   → 3.3 V
//   NES pin 7 (GND)   → GND
//   DATA has a firmware-enabled pull-up so an unconnected port reads
//   "no buttons pressed".
#ifdef MICRONES_BOARD_V0_1
#include "board_pinout_v0_1.h"
#endif

#ifndef PICO_NES_CTL_CLOCK
#ifdef MICRONES_BOARD_V0_1
#define PICO_NES_CTL_CLOCK  MICRONES_V0_1_PIN_NES_CLOCK
#else
#define PICO_NES_CTL_CLOCK  20u
#endif
#endif
#ifndef PICO_NES_CTL_LATCH
#ifdef MICRONES_BOARD_V0_1
#define PICO_NES_CTL_LATCH  MICRONES_V0_1_PIN_NES_LATCH
#else
#define PICO_NES_CTL_LATCH  19u
#endif
#endif
#ifndef PICO_NES_CTL_DATA
#ifdef MICRONES_BOARD_V0_1
#define PICO_NES_CTL_DATA   MICRONES_V0_1_PIN_NES_DATA
#else
#define PICO_NES_CTL_DATA   18u
#endif
#endif

// Initialize all controller GPIOs (call once at startup).
void pico_input_init(void);

// Read all buttons and return the current controller state.
NesControllerState pico_input_read(void);

#endif
