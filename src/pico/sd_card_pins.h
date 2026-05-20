#ifndef MICRONES_SD_CARD_PINS_H
#define MICRONES_SD_CARD_PINS_H

/* SD-card pin configuration for the RP2350.
 *
 * All pin numbers and the chosen SPI block are individually overridable at
 * build time, e.g.
 *     -DMICRONES_SD_PIN_CS=9 -DMICRONES_SD_SPI_INDEX=1
 * so this header should never be edited unless the defaults change for a
 * new board revision.
 *
 * Defaults match the v0.1 PCB silkscreen labeling
 * (hardware/microNES_pcb_v0.1/microNES_pcb_v0.1.kicad_sch), which uses the
 * SD-bus signal names (CD/DAT0/CLK/CMD/DAT3) rather than the SPI signal
 * names.  Mapped to SPI:
 *   CARD_CD       = GP1   -> card-detect (plain GPIO input)
 *   CARD_DAT0     = GP2   -> SPI MISO
 *   CARD_CLX      = GP3   -> SPI SCK
 *   CARD_CMD      = GP4   -> SPI MOSI
 *   CARD_CD_DAT3  = GP5   -> SPI CS
 *
 * NOTE: On RP2350 the SPI peripheral's IO mux ties each signal to a fixed
 * GPIO offset (offset%4 selects RX/CS/SCK/TX), so the pin set above is not
 * a valid hardware-SPI pinout.  The pico-sdk will still configure the
 * mux, but the signals will be scrambled until the next board revision
 * routes the silicon-correct GPIOs.  This file lets you override every
 * pin from CMake when that day comes — no source edits required. */

#ifdef MICRONES_BOARD_V0_1
#include "board_pinout_v0_1.h"
#endif

#ifndef MICRONES_SD_PIN_CD
#ifdef MICRONES_BOARD_V0_1
#define MICRONES_SD_PIN_CD   MICRONES_V0_1_PIN_SD_CD
#else
#define MICRONES_SD_PIN_CD   1u
#endif
#endif
#ifndef MICRONES_SD_PIN_MISO
#ifdef MICRONES_BOARD_V0_1
#define MICRONES_SD_PIN_MISO MICRONES_V0_1_PIN_SD_MISO
#else
#define MICRONES_SD_PIN_MISO 2u
#endif
#endif
#ifndef MICRONES_SD_PIN_SCK
#ifdef MICRONES_BOARD_V0_1
#define MICRONES_SD_PIN_SCK  MICRONES_V0_1_PIN_SD_SCK
#else
#define MICRONES_SD_PIN_SCK  3u
#endif
#endif
#ifndef MICRONES_SD_PIN_MOSI
#ifdef MICRONES_BOARD_V0_1
#define MICRONES_SD_PIN_MOSI MICRONES_V0_1_PIN_SD_MOSI
#else
#define MICRONES_SD_PIN_MOSI 4u
#endif
#endif
#ifndef MICRONES_SD_PIN_CS
#ifdef MICRONES_BOARD_V0_1
#define MICRONES_SD_PIN_CS   MICRONES_V0_1_PIN_SD_CS
#else
#define MICRONES_SD_PIN_CS   5u
#endif
#endif

/* The SD driver runs on a PIO state machine, not the hardware SPI block,
 * because the v0.1 board's GPIO assignment for the card cannot reach the
 * silicon SPI mux on RP2350.  Any GPIOs are valid for PIO-SPI.
 *
 * Which PIO block to claim a state machine on.  0 -> pio0, 1 -> pio1,
 * 2 -> pio2 (RP2350 only).  Override with -DMICRONES_SD_PIO_INDEX=N if the
 * default conflicts with another PIO user (analog video uses pio0; the
 * parallel-TFT display uses pio1; pio2 is generally unused). */
#ifndef MICRONES_SD_PIO_INDEX
#define MICRONES_SD_PIO_INDEX 1u
#endif

/* Card init must happen at 100..400 kHz; we run real I/O much faster. */
#ifndef MICRONES_SD_INIT_BAUD_HZ
#define MICRONES_SD_INIT_BAUD_HZ   200000u
#endif
#ifndef MICRONES_SD_RUN_BAUD_HZ
#define MICRONES_SD_RUN_BAUD_HZ    12500000u
#endif

/* Some breakouts wire CD active-low (closed = card present), some
 * active-high.  Override if the silkscreen says otherwise. */
#ifndef MICRONES_SD_CD_ACTIVE_LOW
#define MICRONES_SD_CD_ACTIVE_LOW  1u
#endif

/* If your card socket has no card-detect line, set this to 1 to skip the
 * presence check and assume a card is always inserted.  The breadboard
 * SD module we're prototyping with has no CD switch, so this defaults
 * to 1; override with -DMICRONES_SD_CD_DISABLED=0 once a CD line is
 * wired (and configure MICRONES_SD_PIN_CD accordingly). */
#ifndef MICRONES_SD_CD_DISABLED
#define MICRONES_SD_CD_DISABLED 1u
#endif

/* Some SD breakouts (notably some HW-125 clones) gate the level-shifter
 * /OE to host CS — buffers only pass signals when CS is low.  That breaks
 * the SD spec's CS-high 74-clock warmup because no clocks reach the card.
 * Setting this to 1 sends the warmup with CS low; the card frames it as
 * idle bytes (0xFF = no command) and it usually works.  Off-spec on
 * compliant breakouts but harmless. */
#ifndef MICRONES_SD_WARMUP_CS_LOW
#define MICRONES_SD_WARMUP_CS_LOW 0u
#endif

#endif
