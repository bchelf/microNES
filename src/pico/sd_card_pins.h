#ifndef MICRONES_SD_CARD_PINS_H
#define MICRONES_SD_CARD_PINS_H

/* SD-card pin configuration for the RP2350.
 *
 * This branch uses the **hardware SPI peripheral**, which on RP2350 ties
 * each SPI signal to a fixed GPIO offset (offset%4 selects RX/CS/SCK/TX).
 * The pin map below is a silicon-correct SPI0 set on the upper GPIO
 * bank (16-23), chosen so that:
 *   - it doesn't collide with video (GP10-14), audio (GP16), or
 *     controllers (GP6-8);
 *   - the four signals are physically adjacent on the Pico header for a
 *     clean breadboard run;
 *   - GP21 is also a valid SPI0 CSn pin if you ever want hardware-driven
 *     CS — we drive it as plain GPIO for flexibility.
 *
 * Per-pin overrides are honoured: -DMICRONES_SD_PIN_*=N from CMake. */

#ifndef MICRONES_SD_PIN_MISO
#define MICRONES_SD_PIN_MISO 20u   /* SPI0 RX  (GP20 % 4 == 0) */
#endif
#ifndef MICRONES_SD_PIN_SCK
#define MICRONES_SD_PIN_SCK  18u   /* SPI0 SCK (GP18 % 4 == 2) */
#endif
#ifndef MICRONES_SD_PIN_MOSI
#define MICRONES_SD_PIN_MOSI 19u   /* SPI0 TX  (GP19 % 4 == 3) */
#endif
#ifndef MICRONES_SD_PIN_CS
#define MICRONES_SD_PIN_CS   21u   /* plain GPIO output */
#endif
#ifndef MICRONES_SD_PIN_CD
#define MICRONES_SD_PIN_CD   17u   /* plain GPIO input; only consulted when
                                      MICRONES_SD_CD_DISABLED == 0 */
#endif

/* Which hardware SPI block.  0 -> spi0, 1 -> spi1.  GP18-21 are SPI0,
 * which is the default. */
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
 * presence check and assume a card is always inserted.  The breadboard
 * SD module we're prototyping with has no CD switch, so this defaults
 * to 1; override with -DMICRONES_SD_CD_DISABLED=0 once a CD line is
 * wired (and configure MICRONES_SD_PIN_CD accordingly). */
#ifndef MICRONES_SD_CD_DISABLED
#define MICRONES_SD_CD_DISABLED 1u
#endif


#endif
