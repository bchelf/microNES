#include "sd_spi.h"

#include "sd_card_pins.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include <string.h>

/* SD-over-SPI command set (subset). */
enum {
    CMD0  = 0,   /* GO_IDLE_STATE */
    CMD8  = 8,   /* SEND_IF_COND */
    CMD9  = 9,   /* SEND_CSD */
    CMD12 = 12,  /* STOP_TRANSMISSION */
    CMD13 = 13,  /* SEND_STATUS */
    CMD16 = 16,  /* SET_BLOCKLEN */
    CMD17 = 17,  /* READ_SINGLE_BLOCK */
    CMD18 = 18,  /* READ_MULTIPLE_BLOCK */
    CMD24 = 24,  /* WRITE_BLOCK */
    CMD25 = 25,  /* WRITE_MULTIPLE_BLOCK */
    CMD55 = 55,  /* APP_CMD */
    CMD58 = 58,  /* READ_OCR */
    ACMD41 = 41, /* SD_SEND_OP_COND */
};

#define DATA_TOKEN_BLOCK_READ_WRITE  0xFEu
#define DATA_RESPONSE_MASK           0x1Fu
#define DATA_RESPONSE_ACCEPTED       0x05u

#define R1_IDLE          0x01u
#define R1_ILLEGAL_CMD   0x04u

static spi_inst_t *sd_spi_block(void) {
    return MICRONES_SD_SPI_INDEX == 0u ? spi0 : spi1;
}

typedef struct {
    bool        initialized;
    SdCardType  type;
    uint32_t    block_count;
} SdState;

static SdState s_sd;

static inline void cs_assert(void)   { gpio_put(MICRONES_SD_PIN_CS, 0); }
static inline void cs_deassert(void) { gpio_put(MICRONES_SD_PIN_CS, 1); }

static uint8_t spi_xfer(uint8_t v) {
    uint8_t out;
    spi_write_read_blocking(sd_spi_block(), &v, &out, 1);
    return out;
}

static void spi_send_dummy(size_t count) {
    for (size_t i = 0; i < count; ++i) {
        (void)spi_xfer(0xFFu);
    }
}

/* Wait for the card to release the bus (MISO high) for up to timeout_ms. */
static bool wait_not_busy(uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!time_reached(deadline)) {
        if (spi_xfer(0xFFu) == 0xFFu) {
            return true;
        }
    }
    return false;
}

/* Standard CRC7 used by CMD0 / CMD8 / CMD58. */
static uint8_t crc7(const uint8_t *data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (uint8_t)((crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x09u) : (uint8_t)(crc << 1));
        }
    }
    return (uint8_t)((crc << 1) | 0x01u);
}

static uint8_t send_cmd(uint8_t cmd, uint32_t arg) {
    uint8_t buf[6];
    buf[0] = (uint8_t)(0x40u | (cmd & 0x3Fu));
    buf[1] = (uint8_t)((arg >> 24) & 0xFFu);
    buf[2] = (uint8_t)((arg >> 16) & 0xFFu);
    buf[3] = (uint8_t)((arg >> 8) & 0xFFu);
    buf[4] = (uint8_t)(arg & 0xFFu);
    buf[5] = crc7(buf, 5);

    /* Allow the card to release bus before issuing the next command. */
    (void)wait_not_busy(500);

    for (int i = 0; i < 6; ++i) {
        (void)spi_xfer(buf[i]);
    }

    /* Discard the optional stuff byte after CMD12 (per spec, the first byte
     * is a stuff byte). */
    if (cmd == CMD12) {
        (void)spi_xfer(0xFFu);
    }

    /* Poll for the R1 response (high bit clear).  Up to 8 byte-times is the
     * spec ceiling but allow a bit more for slow cards. */
    for (int i = 0; i < 16; ++i) {
        uint8_t r = spi_xfer(0xFFu);
        if ((r & 0x80u) == 0u) {
            return r;
        }
    }
    return 0xFFu;
}

static uint8_t send_acmd(uint8_t cmd, uint32_t arg) {
    (void)send_cmd(CMD55, 0);
    return send_cmd(cmd, arg);
}

static SdResult read_data_block(uint8_t *buf) {
    /* Wait for the data token. */
    absolute_time_t deadline = make_timeout_time_ms(200);
    while (!time_reached(deadline)) {
        uint8_t t = spi_xfer(0xFFu);
        if (t == DATA_TOKEN_BLOCK_READ_WRITE) {
            for (int i = 0; i < 512; ++i) {
                buf[i] = spi_xfer(0xFFu);
            }
            (void)spi_xfer(0xFFu);  /* CRC hi */
            (void)spi_xfer(0xFFu);  /* CRC lo */
            return SD_OK;
        }
        if (t != 0xFFu) {
            return SD_ERR_READ;
        }
    }
    return SD_ERR_TIMEOUT;
}

bool sd_card_present(void) {
#if MICRONES_SD_CD_DISABLED
    return true;
#else
    bool level = gpio_get(MICRONES_SD_PIN_CD);
#if MICRONES_SD_CD_ACTIVE_LOW
    return !level;
#else
    return level;
#endif
#endif
}

bool       sd_is_initialized(void) { return s_sd.initialized; }
SdCardType sd_card_type(void)       { return s_sd.type; }
uint32_t   sd_block_count(void)     { return s_sd.block_count; }

static SdResult parse_csd_block_count(const uint8_t csd[16]) {
    uint8_t version = (uint8_t)(csd[0] >> 6);
    if (version == 1u) {
        /* CSD v2: c_size is bits 69..48, capacity = (c_size + 1) * 512 KiB. */
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3Fu) << 16) |
                          ((uint32_t)csd[8] << 8) |
                          (uint32_t)csd[9];
        s_sd.block_count = (c_size + 1u) * 1024u;  /* 512KiB chunks / 512 = 1024 */
        return SD_OK;
    }
    /* CSD v1: standard capacity. */
    uint32_t c_size = ((uint32_t)(csd[6] & 0x03u) << 10) |
                      ((uint32_t)csd[7] << 2) |
                      ((uint32_t)csd[8] >> 6);
    uint8_t c_size_mult = (uint8_t)(((csd[9] & 0x03u) << 1) | (csd[10] >> 7));
    uint8_t read_bl_len = (uint8_t)(csd[5] & 0x0Fu);
    uint32_t blocknr = (c_size + 1u) << (c_size_mult + 2u);
    uint32_t block_len = 1u << read_bl_len;
    s_sd.block_count = blocknr * (block_len / 512u);
    return SD_OK;
}

static void sd_pins_init(void) {
    gpio_set_function(MICRONES_SD_PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(MICRONES_SD_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(MICRONES_SD_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_pull_up(MICRONES_SD_PIN_MISO);

    gpio_init(MICRONES_SD_PIN_CS);
    gpio_set_dir(MICRONES_SD_PIN_CS, GPIO_OUT);
    gpio_put(MICRONES_SD_PIN_CS, 1);

#if !MICRONES_SD_CD_DISABLED
    gpio_init(MICRONES_SD_PIN_CD);
    gpio_set_dir(MICRONES_SD_PIN_CD, GPIO_IN);
    gpio_pull_up(MICRONES_SD_PIN_CD);
#endif
}

void sd_deinit(void) {
    if (s_sd.initialized) {
        spi_deinit(sd_spi_block());
    }
    memset(&s_sd, 0, sizeof(s_sd));
}

SdResult sd_init(void) {
    sd_deinit();

    sd_pins_init();
    spi_init(sd_spi_block(), MICRONES_SD_INIT_BAUD_HZ);
    spi_set_format(sd_spi_block(), 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    if (!sd_card_present()) {
        return SD_ERR_NO_CARD;
    }

    /* Power-up sequence: with CS deasserted, send 80+ clocks at < 400 kHz. */
    cs_deassert();
    sleep_ms(2);
    spi_send_dummy(10);

    /* CMD0 must be sent with CS asserted. */
    cs_assert();
    SdResult result = SD_OK;
    bool got_idle = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint8_t r = send_cmd(CMD0, 0);
        if (r == R1_IDLE) {
            got_idle = true;
            break;
        }
        sleep_ms(10);
    }
    if (!got_idle) {
        result = SD_ERR_CMD0;
        goto fail;
    }

    /* CMD8: voltage check.  Argument 0x1AA = 2.7-3.6V, check pattern 0xAA. */
    uint8_t r1 = send_cmd(CMD8, 0x000001AAu);
    bool v2_card = false;
    if (r1 == R1_IDLE) {
        uint8_t echo[4];
        for (int i = 0; i < 4; ++i) echo[i] = spi_xfer(0xFFu);
        if (echo[2] == 0x01u && echo[3] == 0xAAu) {
            v2_card = true;
        } else {
            result = SD_ERR_CMD8;
            goto fail;
        }
    } else if ((r1 & R1_ILLEGAL_CMD) == 0u) {
        /* Some response other than idle/illegal — protocol error. */
        result = SD_ERR_CMD8;
        goto fail;
    }
    /* If illegal command, we have a v1 card; proceed. */

    /* ACMD41: send op cond.  Poll for ready (R1 = 0). */
    absolute_time_t deadline = make_timeout_time_ms(2000);
    bool ready = false;
    while (!time_reached(deadline)) {
        uint8_t r = send_acmd(ACMD41, v2_card ? 0x40000000u : 0);
        if (r == 0u) {
            ready = true;
            break;
        }
        sleep_ms(2);
    }
    if (!ready) {
        result = SD_ERR_INIT_TIMEOUT;
        goto fail;
    }

    /* CMD58: read OCR to detect SDHC. */
    bool sdhc = false;
    if (v2_card) {
        if (send_cmd(CMD58, 0) != 0u) {
            result = SD_ERR_OCR;
            goto fail;
        }
        uint8_t ocr[4];
        for (int i = 0; i < 4; ++i) ocr[i] = spi_xfer(0xFFu);
        sdhc = (ocr[0] & 0x40u) != 0u;
    }

    if (!sdhc) {
        /* Set 512-byte block length on SDSC cards.  SDHC ignores. */
        if (send_cmd(CMD16, 512) != 0u) {
            result = SD_ERR_OCR;
            goto fail;
        }
    }

    s_sd.type = sdhc ? SD_TYPE_SDHC : (v2_card ? SD_TYPE_SDV2 : SD_TYPE_SDV1);

    /* CMD9: SEND_CSD to learn capacity. */
    if (send_cmd(CMD9, 0) == 0u) {
        uint8_t csd[16];
        if (read_data_block(csd) == SD_OK) {
            (void)parse_csd_block_count(csd);
        }
    }

    cs_deassert();
    spi_send_dummy(1);

    /* Bump to run clock.  pico-sdk picks the closest divisor below the
     * requested baud — actual frequency may be lower (no error here). */
    spi_set_baudrate(sd_spi_block(), MICRONES_SD_RUN_BAUD_HZ);

    s_sd.initialized = true;
    return SD_OK;

fail:
    cs_deassert();
    spi_send_dummy(1);
    s_sd.initialized = false;
    return result;
}

static uint32_t lba_to_address(uint32_t lba) {
    /* SDSC cards are byte-addressed; SDHC are block-addressed. */
    return s_sd.type == SD_TYPE_SDHC ? lba : (lba * 512u);
}

SdResult sd_read_block(uint32_t lba, uint8_t *buf) {
    if (!s_sd.initialized) return SD_ERR_NOT_INITIALIZED;
    cs_assert();
    SdResult r;
    if (send_cmd(CMD17, lba_to_address(lba)) != 0u) {
        r = SD_ERR_READ;
    } else {
        r = read_data_block(buf);
    }
    cs_deassert();
    spi_send_dummy(1);
    return r;
}

SdResult sd_write_block(uint32_t lba, const uint8_t *buf) {
    if (!s_sd.initialized) return SD_ERR_NOT_INITIALIZED;
    cs_assert();
    SdResult r = SD_OK;
    if (send_cmd(CMD24, lba_to_address(lba)) != 0u) {
        r = SD_ERR_WRITE;
        goto out;
    }
    /* One-byte gap before token. */
    (void)spi_xfer(0xFFu);
    (void)spi_xfer(DATA_TOKEN_BLOCK_READ_WRITE);
    for (int i = 0; i < 512; ++i) {
        (void)spi_xfer(buf[i]);
    }
    /* Dummy CRC. */
    (void)spi_xfer(0xFFu);
    (void)spi_xfer(0xFFu);

    /* Data response. */
    uint8_t resp = spi_xfer(0xFFu);
    if ((resp & DATA_RESPONSE_MASK) != DATA_RESPONSE_ACCEPTED) {
        r = SD_ERR_WRITE;
        goto out;
    }

    /* Wait for card to finish the program cycle. */
    if (!wait_not_busy(500)) {
        r = SD_ERR_TIMEOUT;
    }

out:
    cs_deassert();
    spi_send_dummy(1);
    return r;
}
