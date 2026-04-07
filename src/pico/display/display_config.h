#ifndef MICRONES_DISPLAY_CONFIG_H
#define MICRONES_DISPLAY_CONFIG_H

#include "hardware/pio.h"

#include <stdint.h>

/*
 * Build selection:
 *   - MICRONES_PICO_TFT_BACKEND=spi_ili9341 preserves the original SPI path
 *   - MICRONES_PICO_TFT_BACKEND=parallel_8080 enables the PIO+DMA 8-bit bus
 *
 * Controller init is selected independently with MICRONES_PICO_TFT_CONTROLLER.
 * For 3.5" Arduino shields, start with:
 *   -DMICRONES_PICO_TFT_BACKEND=parallel_8080
 *   -DMICRONES_PICO_TFT_CONTROLLER=ili9486
 */

#define MICRONES_TFT_MAX_PANEL_WIDTH  480u
#define MICRONES_TFT_MAX_PANEL_HEIGHT 320u
#define MICRONES_TFT_PIN_UNUSED       0xffffffffu

#if defined(MICRONES_DISPLAY_BACKEND_SPI_ILI9341)
enum {
    /*
     * SPI TFT pin block intentionally overlaps the parallel shield region.
     * The SPI and parallel display backends are mutually exclusive at build
     * time, so reusing these pins keeps more of the upper GPIO range free for
     * the controller and audio wiring.
     *
     * This mapping uses SPI1 on the RP2350:
     *   GP8  = RX/MISO
     *   GP10 = SCK
     *   GP11 = TX/MOSI
     */
    MICRONES_TFT_PIN_MISO = 8u,
    MICRONES_TFT_PIN_CS = 9u,
    MICRONES_TFT_PIN_SCK = 10u,
    MICRONES_TFT_PIN_MOSI = 11u,
    MICRONES_TFT_PIN_RS = 12u,
    MICRONES_TFT_PIN_RST = 13u,
    MICRONES_TFT_PIN_BL = 14u,
    MICRONES_TFT_SPI_BAUD_HZ = 62500000u,
};
#elif defined(MICRONES_DISPLAY_BACKEND_PARALLEL_8080)
/*
 * Parallel shield pin block.
 *
 * Edit this one block to match the board wiring. The 8-bit data bus must stay
 * contiguous because the PIO state machine drives it with a single out pins, 8.
 *
 * This build assumes:
 *   - LCD_CS is tied LOW to GND, so the panel is permanently selected
 *   - LCD_RD is tied HIGH to 3v3, so read cycles are disabled in hardware
 *
 * If you later want software control again, uncomment these and set real pins:
 *   // MICRONES_TFT_PIN_RD = 9u,
 *   // MICRONES_TFT_PIN_CS = 11u,
 *   // MICRONES_TFT_PIN_BL = 13u,
 */
enum {
    MICRONES_TFT_PIN_D0_BASE = 0u,
    MICRONES_TFT_PIN_WR = 8u,
    MICRONES_TFT_PIN_RS = 10u,
    MICRONES_TFT_PIN_RST = 12u,
};

#define MICRONES_TFT_PIN_RD                 MICRONES_TFT_PIN_UNUSED
#define MICRONES_TFT_PIN_CS                 MICRONES_TFT_PIN_UNUSED
#define MICRONES_TFT_PIN_BL                 MICRONES_TFT_PIN_UNUSED

#define MICRONES_TFT_PARALLEL_PIO              pio1
#define MICRONES_TFT_PARALLEL_SM               0u
/*
 * Byte clock for 8080 writes.
 *
 * The PIO program emits one byte every 2 SM cycles:
 *   byte_rate = clk_sys / (2 * clkdiv)
 *
 * At 250 MHz:
 *   clkdiv=4  -> 31.25 MB/s  (tWL=tWH=16ns; ILI9486 min is 15ns — meets spec)
 *   clkdiv=6  -> 20.83 MB/s  (conservative fallback if panel shows artifacts)
 *   clkdiv=16 -> 7.81 MB/s   (matches SPI — no longer needed)
 *
 * clkdiv=4 is correct now that WR setup/hold timing is fixed in the PIO program.
 * Back off to 6 only if color artifacts reappear at higher speed.
 */
#define MICRONES_TFT_PARALLEL_CLKDIV           4.0f
#define MICRONES_TFT_PARALLEL_KEEP_CS_ASSERTED 1
#else
#error "No TFT transport backend selected"
#endif

#endif
