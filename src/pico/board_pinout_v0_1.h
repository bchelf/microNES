#ifndef MICRONES_BOARD_PINOUT_V0_1_H
#define MICRONES_BOARD_PINOUT_V0_1_H

/*
 * microNES PCB v0.1 — canonical RP2350 GPIO pinout.
 *
 * Source of truth: hardware/microNES_pcb_v0.1/microNES_pcb_v0.1.kicad_sch
 * (U1, RP2350_60QFN, placed at (240.03, 105.41) in the schematic).
 *
 * The schematic uses local labels on each RP2350 GPIO pin.  This header
 * mirrors those labels exactly, so a build configured with
 * -DMICRONES_BOARD=v0_1 (or by defining MICRONES_BOARD_V0_1 in CMake) drives
 * the same nets the PCB silkscreen names.
 *
 *   Schematic label  | GPIO | Peripheral / function
 *   -----------------|------|-------------------------------------------
 *   GPIO0            |  0   | unused / test point
 *   CARD_CD          |  1   | SD card detect (active-low input)
 *   CARD_DAT0        |  2   | SD card DAT0  / SPI MISO (PIO-SPI)
 *   CARD_CLX         |  3   | SD card CLK   / SPI SCK  (PIO-SPI)
 *   CARD_CMD         |  4   | SD card CMD   / SPI MOSI (PIO-SPI)
 *   CARD_CD_DAT3     |  5   | SD card DAT3  / SPI CS   (PIO-SPI)
 *   NES_CLOCK        |  6   | NES 4021 controller CLOCK (output)
 *   NES_LATCH        |  7   | NES 4021 controller LATCH (output)
 *   NES_DATA_1       |  8   | NES controller #1 DATA (input, pull-up)
 *   NES_DATA_2       |  9   | NES controller #2 DATA (input, pull-up)
 *   (unused)         | 10   | -
 *   HDMI_HPD         | 11   | HDMI hot-plug detect (input)
 *   (header)         | 12   | exposed via expansion header (no on-board fn)
 *   (header)         | 13   | exposed via expansion header
 *   (header)         | 14   | exposed via expansion header
 *   (header)         | 15   | exposed via expansion header
 *   (header)         | 16   | exposed via expansion header
 *   (header)         | 17   | exposed via expansion header
 *   (header)         | 18   | exposed via expansion header
 *   (header)         | 19   | exposed via expansion header
 *   RST_BTN          | 20   | reset / user button (input, pull-up)
 *   PWR_LED          | 21   | power LED (output, active-high)
 *   (unused)         | 22   | -
 *   GP_ADAC_IN       | 23   | audio DAC PWM input (drives ADAC_OUT chain)
 *   GP_VDAC_SUM0     | 24   | composite video DAC bit 0 (LSB, 1000 Ω)
 *   GP_VDAC_SUM1     | 25   | composite video DAC bit 1 (485 Ω)
 *   GP_VDAC_SUM2     | 26   | composite video DAC bit 2 (242 Ω)
 *   GP_VDAC_SUM3     | 27   | composite video DAC bit 3 (MSB, 120 Ω)
 *   GP_VDAC_GATE     | 28   | sync-clamp N-MOSFET gate (active-high)
 *   (unused)         | 29   | -
 *
 * Notes on consequences of this pinout:
 *
 * - The composite video DAC occupies GP24..GP27, a contiguous 4-pin block,
 *   so the existing PIO program (`out pins, 4`) still works unchanged —
 *   only MICRONES_VIDEO_PIN_BASE needs to change from 10 to 24.
 *
 * - The sync clamp (GP_VDAC_GATE) is GP28, a plain GPIO toggled by CPU.
 *
 * - Audio uses PWM on GP23 → external analog DAC / filter chain
 *   (ADAC_OUT → AUD_A/AUD_B line outputs).  The MAX98357 I2S backend is
 *   NOT present on the v0.1 PCB; that backend remains targeted at
 *   breadboard rigs and is unaffected by this header.
 *
 * - The SD card slot uses GP1..GP5 with non-standard signal ordering
 *   (CD, DAT0, CLK, CMD, DAT3).  This cannot reach the RP2350 hardware
 *   SPI peripheral's IO mux, so the SD driver runs SPI through PIO; that
 *   was already true in `sd_card_pins.h` and matches this layout.
 *
 * - The NES controller block uses GP6..GP9 (vs. GP18..GP20 on breadboard).
 *   The PCB exposes a second-player DATA line on GP9 (NES_DATA_2); the
 *   existing single-controller firmware ignores it.
 */

/* --- Composite NTSC video DAC ---------------------------------------------- */
#define MICRONES_V0_1_PIN_VDAC_BIT0     24u  /* GP_VDAC_SUM0, LSB */
#define MICRONES_V0_1_PIN_VDAC_BIT1     25u  /* GP_VDAC_SUM1 */
#define MICRONES_V0_1_PIN_VDAC_BIT2     26u  /* GP_VDAC_SUM2 */
#define MICRONES_V0_1_PIN_VDAC_BIT3     27u  /* GP_VDAC_SUM3, MSB */
#define MICRONES_V0_1_PIN_VDAC_BASE     MICRONES_V0_1_PIN_VDAC_BIT0
#define MICRONES_V0_1_PIN_VDAC_GATE     28u  /* GP_VDAC_GATE, sync-clamp MOSFET */

/* --- Audio PWM (drives external ADAC / analog filter) --------------------- */
#define MICRONES_V0_1_PIN_AUDIO_PWM     23u  /* GP_ADAC_IN */

/* --- NES controller (4021 PISO shift register) ---------------------------- */
#define MICRONES_V0_1_PIN_NES_CLOCK     6u   /* NES_CLOCK,  output */
#define MICRONES_V0_1_PIN_NES_LATCH     7u   /* NES_LATCH,  output */
#define MICRONES_V0_1_PIN_NES_DATA      8u   /* NES_DATA_1, input, pull-up */
#define MICRONES_V0_1_PIN_NES_DATA_P2   9u   /* NES_DATA_2, input, pull-up (second player) */

/* --- SD card (PIO-SPI) ---------------------------------------------------- */
#define MICRONES_V0_1_PIN_SD_CD         1u   /* CARD_CD,       card-detect */
#define MICRONES_V0_1_PIN_SD_MISO       2u   /* CARD_DAT0,     SPI MISO */
#define MICRONES_V0_1_PIN_SD_SCK        3u   /* CARD_CLX,      SPI SCK  */
#define MICRONES_V0_1_PIN_SD_MOSI       4u   /* CARD_CMD,      SPI MOSI */
#define MICRONES_V0_1_PIN_SD_CS         5u   /* CARD_CD_DAT3,  SPI CS   */

/* --- Other on-board peripherals ------------------------------------------- */
#define MICRONES_V0_1_PIN_HDMI_HPD      11u  /* HDMI hot-plug detect (input) */
#define MICRONES_V0_1_PIN_RST_BUTTON    20u  /* user/reset button (input)    */
#define MICRONES_V0_1_PIN_PWR_LED       21u  /* power LED (output)           */

#endif /* MICRONES_BOARD_PINOUT_V0_1_H */
