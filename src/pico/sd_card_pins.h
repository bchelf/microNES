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
 * Defaults match the current board's silkscreen labeling, which uses the
 * SD-bus signal names (CD/DAT0/CLK/CMD/DAT3) rather than the SPI signal
 * names.  Mapped to SPI:
 *   CARD_CD   = GP1   -> card-detect (plain GPIO input)
 *   CARD_DAT0 = GP2   -> SPI MISO
 *   CARD_CLK  = GP3   -> SPI SCK
 *   CARD_CMD  = GP4   -> SPI MOSI
 *   CARD_DAT3 = GP5   -> SPI CS
 *
 * NOTE: On RP2350 the SPI peripheral's IO mux ties each signal to a fixed
 * GPIO offset (offset%4 selects RX/CS/SCK/TX), so the pin set above is not
 * a valid hardware-SPI pinout.  The pico-sdk will still configure the
 * mux, but the signals will be scrambled until the next board revision
 * routes the silicon-correct GPIOs.  This file lets you override every
 * pin from CMake when that day comes — no source edits required. */

#ifndef MICRONES_SD_PIN_CD
#define MICRONES_SD_PIN_CD   1u
#endif
#ifndef MICRONES_SD_PIN_MISO
#define MICRONES_SD_PIN_MISO 2u
#endif
#ifndef MICRONES_SD_PIN_SCK
#define MICRONES_SD_PIN_SCK  3u
#endif
#ifndef MICRONES_SD_PIN_MOSI
#define MICRONES_SD_PIN_MOSI 4u
#endif
#ifndef MICRONES_SD_PIN_CS
#define MICRONES_SD_PIN_CS   5u
#endif

/* Which SPI peripheral to use.  0 -> spi0, 1 -> spi1.  Override with
 * -DMICRONES_SD_SPI_INDEX=1 when the pin map dictates. */
#ifndef MICRONES_SD_SPI_INDEX
#define MICRONES_SD_SPI_INDEX 0u
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
 * presence check and assume a card is always inserted. */
#ifndef MICRONES_SD_CD_DISABLED
#define MICRONES_SD_CD_DISABLED 0u
#endif

#endif
