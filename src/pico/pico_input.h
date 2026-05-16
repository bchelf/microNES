#ifndef MICRONES_PICO_INPUT_H
#define MICRONES_PICO_INPUT_H

#include "input.h"

// NES hardware controller GPIO pins (original 4021 controllers).
//
// Breadboard default (matches the legacy breadboard rig, single controller):
//   NES pin 2 (CLOCK) → GP20    NES pin 3 (LATCH) → GP19
//   NES pin 4 (DATA)  → GP18
//
// v0.1 PCB (build with -DMICRONES_BOARD=v0_1, two controllers):
//   NES pin 2 (CLOCK)  → GP6  (NES_CLOCK in the schematic; shared)
//   NES pin 3 (LATCH)  → GP7  (NES_LATCH; shared)
//   Player 1 DATA      → GP8  (NES_DATA_1)
//   Player 2 DATA      → GP9  (NES_DATA_2)
//
// Both controllers share CLOCK/LATCH because the 4021 shift registers in
// each pad are clocked in lock-step; only DATA is per-port.  Reading both
// ports therefore takes the same eight clock edges as reading one.
//
// Common wiring (all boards):
//   NES pin 1 (VCC)   → 3.3 V
//   NES pin 7 (GND)   → GND
//   DATA has a firmware-enabled pull-up so an unconnected port reads as
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

/* Player-2 DATA pin.  Optional — leave undefined to disable the second
 * controller entirely (the firmware will report "no buttons" for it).
 * The v0.1 PCB wires this to GP9 (NES_DATA_2); the breadboard has no
 * second-port footprint so the default is "not present". */
#ifndef PICO_NES_CTL_DATA_P2
#ifdef MICRONES_BOARD_V0_1
#define PICO_NES_CTL_DATA_P2  MICRONES_V0_1_PIN_NES_DATA_P2
#endif
#endif

/* Returned by pico_input_read_pair() for boards that route two controllers. */
typedef struct {
    NesControllerState players[2];
} PicoControllerPair;

// Initialize all controller GPIOs (call once at startup).
void pico_input_init(void);

// Read player 1 only.  Equivalent to pico_input_read_pair().players[0].
NesControllerState pico_input_read(void);

// Read both controllers in a single CLOCK/LATCH cycle.  On boards without
// a second-port DATA pin (PICO_NES_CTL_DATA_P2 undefined), players[1] is
// always zero.
PicoControllerPair pico_input_read_pair(void);

#endif
