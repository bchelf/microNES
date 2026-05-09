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

#endif
