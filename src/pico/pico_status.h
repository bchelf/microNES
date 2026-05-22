#ifndef MICRONES_PICO_STATUS_H
#define MICRONES_PICO_STATUS_H

/*
 * Front-panel I/O (v0.1 PCB only).
 *
 * The v0.1 PCB exposes a 5-pin NES-style front-panel connector (U8) with a
 * power LED and a momentary reset button.  These land on the RP2350 as:
 *
 *   GP21  PWR_LED   output, drives the panel LED indicator
 *   GP20  RST_BTN   input,  reads the panel reset switch
 *
 * Polarity from the board wiring:
 *   - RST_BTN is held low when idle and shorted to +3V3 when the button is
 *     pressed -> ACTIVE-HIGH input.  The internal pull-down is enabled so an
 *     unpopulated connector reads "not pressed".
 *   - PWR_LED is wired as a common-anode indicator (anode tied to a panel
 *     voltage rail, cathode returned via this pin), so driving GP21 HIGH
 *     sources current toward the rail and lights the LED.  The panel
 *     wiring isn't fully unambiguous in the v0.1 schematic; if the LED
 *     ends up inverted on real hardware, build with
 *     -DMICRONES_PWR_LED_ACTIVE_LOW=1 to flip the drive level.
 *
 * On boards without these nets (breadboard default) the whole module is a
 * no-op — GP20/GP21 are used as NES controller lines there and must not
 * be touched by this code.
 */

#include <stdbool.h>

#ifndef MICRONES_PWR_LED_ACTIVE_LOW
#define MICRONES_PWR_LED_ACTIVE_LOW 0
#endif

#ifndef MICRONES_RST_BTN_ACTIVE_LOW
#define MICRONES_RST_BTN_ACTIVE_LOW 0
#endif

/* Initialise GP20 (reset button input, pull-down by default) and GP21 (power
 * LED output, driven to the "on" state immediately).  No-op when
 * MICRONES_BOARD_V0_1 is not defined. */
void pico_status_init(void);

/* Turn the panel power LED on/off.  Honours MICRONES_PWR_LED_ACTIVE_LOW.
 * No-op on non-v0.1 builds. */
void pico_status_set_led(bool on);

/* Edge-detected reset-button read.  Returns true exactly once per
 * press (released → pressed transition).  Always returns false on
 * non-v0.1 builds. */
bool pico_status_reset_button_pressed(void);

#endif /* MICRONES_PICO_STATUS_H */
