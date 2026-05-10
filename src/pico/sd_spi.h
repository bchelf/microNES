#ifndef MICRONES_SD_SPI_H
#define MICRONES_SD_SPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SD_OK              = 0,
    SD_ERR_NO_CARD     = -1,
    SD_ERR_CMD0        = -2,
    SD_ERR_CMD8        = -3,
    SD_ERR_INIT_TIMEOUT= -4,
    SD_ERR_OCR         = -5,
    SD_ERR_READ        = -6,
    SD_ERR_WRITE       = -7,
    SD_ERR_TIMEOUT     = -8,
    SD_ERR_NOT_INITIALIZED = -9,
} SdResult;

typedef enum {
    SD_TYPE_NONE = 0,
    SD_TYPE_SDV1,    /* SD v1.x byte-addressed */
    SD_TYPE_SDV2,    /* SD v2.0 byte-addressed (SDSC) */
    SD_TYPE_SDHC,    /* SD v2.0 block-addressed (SDHC/SDXC) */
} SdCardType;

/* True if a card is physically present (reads card-detect). */
bool sd_card_present(void);

/* Bring up the SPI bus and initialize the card.  Safe to call multiple
 * times; on success the card is left in run-clock mode ready for IO. */
SdResult sd_init(void);

/* Tear down GPIO/SPI configuration.  Useful after error to retry. */
void sd_deinit(void);

bool       sd_is_initialized(void);
SdCardType sd_card_type(void);
uint32_t   sd_block_count(void);    /* 512-byte sectors */

/* Block-addressed read (LBA = 512-byte sector index).  buf must be 512
 * bytes.  For SDSC cards the address is internally translated to bytes. */
SdResult sd_read_block(uint32_t lba, uint8_t *buf);

/* Block-addressed write.  buf must be 512 bytes.  Returns SD_OK after the
 * card finishes its internal write (busy line cleared). */
SdResult sd_write_block(uint32_t lba, const uint8_t *buf);

/* Print the saved state from the last sd_init() attempt to stdout —
 * pin map, the 10 MISO warmup bytes, and the R1 byte from each CMD0
 * attempt.  Cheap; safe to call any number of times.  Prints nothing if
 * sd_init() has never run. */
void sd_print_init_diag(void);

/* Slowly toggle each SD pin (CS, SCK, MOSI) as a plain GPIO output,
 * reading back the line state with gpio_get() to verify that the chip
 * itself can drive the pin.  Each pin holds high for 500 ms then low
 * for 500 ms, repeated 3 times — long enough for a multimeter or LED
 * to register.  Tears down any prior PIO/SD state and leaves the pins
 * configured for the next sd_init() to take over.
 *
 * What the readback tells you:
 *   - log says "drive=1 read=1, drive=0 read=0" → pin works on the chip
 *     side; if breakout still doesn't see it, suspect the wire or solder.
 *   - log says "drive=0 read=1" → pin is being pulled high by something
 *     downstream (short to power, wrong wire).
 *   - log says "drive=1 read=0" → pin shorted to ground. */
void sd_run_gpio_probe(void);

#endif
