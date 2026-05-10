#pragma once

#include "input.h"

// NES original controller driver – 4021 PISO shift register.
//
// Wiring (NES 7-pin connector):
//   pin 1  VCC  → 3.3 V (or 5 V; the 4021 works fine at 3.3 V)
//   pin 2  CLOCK  ← MCU output (idle low)
//   pin 3  LATCH  ← MCU output (idle low)
//   pin 4  DATA   → MCU input  (active-low; MCU pull-up required)
//   pin 7  GND   → GND
//
// Protocol (called once per NES frame, ~60 Hz):
//   1. Assert LATCH high for NES_HW_CTL_LATCH_US.
//      The 4021 captures all eight button states in parallel.
//   2. De-assert LATCH (low).  DATA immediately carries bit 0 (A button).
//   3. For each of the 8 buttons:
//        a. Sample DATA (0 = pressed, 1 = released).
//        b. Pulse CLOCK high then low → next bit appears on DATA.
//
// Button order on the wire (NES standard, LSB first):
//   0: A       4: UP
//   1: B       5: DOWN
//   2: SELECT  6: LEFT
//   3: START   7: RIGHT
//
// Both the ESP32-S3 and Pico targets implement this interface.
// Timing constants are shared here; GPIO operations are platform-specific.
//
// ── Pin assignments ──────────────────────────────────────────────────────
//
//  ESP32-S3 (Waveshare ESP32-S3-Touch-AMOLED-1.91) – defined in board.h:
//    GPIO 11 → CLOCK
//    GPIO 12 → LATCH
//    GPIO 13 → DATA
//
//  RP2350 / Pico 2 – defined in pico_input.h:
//    GP20 → CLOCK
//    GP19 → LATCH
//    GP18 → DATA

// Pulse widths for the 4021.  Original NES spec is ≥ 12 µs LATCH and
// 6 µs CLOCK half-period (12 µs full period, ~83 kHz).  Earlier values
// (6 / 2 µs) ran clean on the bench but turned out to be marginal on
// breadboard wiring — the shift register would catch the first one or
// two bits and then DATA would latch low for the rest of the read.
// Going to spec gives plenty of margin and only costs ~108 µs/read,
// negligible against the 16.6 ms frame budget.
#define NES_HW_CTL_LATCH_US  12u
#define NES_HW_CTL_CLOCK_US  6u

// Initialise GPIO pins.  Call once at startup before nes_hw_controller_read.
void nes_hw_controller_init(void);

// Perform one full read cycle and return the combined button state.
// Blocking: takes roughly (LATCH_US + 8 * 2 * CLOCK_US) ≈ 42 µs.
// At 60 Hz this is ~0.25 % of a frame budget.
NesControllerState nes_hw_controller_read(void);
