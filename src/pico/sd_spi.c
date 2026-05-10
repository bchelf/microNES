#include "sd_spi.h"

#include "sd_card_pins.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include <stdio.h>
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

/* Diagnostic snapshot of the most recent sd_init() attempt. */
typedef struct {
    bool     attempted;
    uint8_t  warmup[10];
    int      cmd0_attempts;
    uint8_t  cmd0_responses[8];
    uint8_t  probe_tx[4];
    uint8_t  probe_rx[4];
    SdResult last_result;
} SdInitDiag;

static SdInitDiag s_diag;

typedef struct {
    bool        initialized;
    SdCardType  type;
    uint32_t    block_count;
} SdState;

static SdState s_sd;

static spi_inst_t *sd_spi_block(void) {
    return MICRONES_SD_SPI_INDEX == 0u ? spi0 : spi1;
}

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

/* SD CRC7.  Polynomial G(x) = x^7 + x^3 + 1, encoded as 0x89.
 *
 * Per the SD physical layer spec example: at each bit, conditionally
 * XOR with the polynomial when bit 7 is set, then shift left.  Shifting
 * BEFORE the XOR (as an earlier broken version of this function did)
 * is *not* equivalent — it leaves bit 7 of the result toggled wrong.
 *
 * After processing all input bytes the CRC7 occupies bits 7..1 of the
 * register and bit 0 is 0; ORing 0x01 sets the SD command-frame stop
 * bit and yields the byte the card expects.
 *
 * Cross-check expected outputs (verify if you ever touch this):
 *   crc7({0x40,0x00,0x00,0x00,0x00})   == 0x95   (CMD0)
 *   crc7({0x48,0x00,0x00,0x01,0xAA})   == 0x87   (CMD8 voltage check) */
static uint8_t crc7(const uint8_t *data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x80u) {
                crc ^= 0x89u;
            }
            crc = (uint8_t)(crc << 1);
        }
    }
    return (uint8_t)(crc | 0x01u);
}

static uint8_t send_cmd(uint8_t cmd, uint32_t arg) {
    uint8_t buf[6];
    buf[0] = (uint8_t)(0x40u | (cmd & 0x3Fu));
    buf[1] = (uint8_t)((arg >> 24) & 0xFFu);
    buf[2] = (uint8_t)((arg >> 16) & 0xFFu);
    buf[3] = (uint8_t)((arg >> 8) & 0xFFu);
    buf[4] = (uint8_t)(arg & 0xFFu);
    buf[5] = crc7(buf, 5);

    (void)wait_not_busy(500);

    for (int i = 0; i < 6; ++i) {
        (void)spi_xfer(buf[i]);
    }

    if (cmd == CMD12) {
        (void)spi_xfer(0xFFu);
    }

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

/* Wait for the SD data token, then read exactly `len` bytes into buf
 * followed by the 2 CRC bytes (which we discard).  `len` is 512 for
 * CMD17/18 block reads and 16 for CMD9 CSD reads — getting this wrong
 * silently overruns the caller's buffer. */
static SdResult read_data_block(uint8_t *buf, size_t len) {
    absolute_time_t deadline = make_timeout_time_ms(200);
    while (!time_reached(deadline)) {
        uint8_t t = spi_xfer(0xFFu);
        if (t == DATA_TOKEN_BLOCK_READ_WRITE) {
            for (size_t i = 0; i < len; ++i) {
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
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3Fu) << 16) |
                          ((uint32_t)csd[8] << 8) |
                          (uint32_t)csd[9];
        s_sd.block_count = (c_size + 1u) * 1024u;
        return SD_OK;
    }
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
    /* Hardware-SPI signals: ask the IO mux to route them to SPI. */
    gpio_set_function(MICRONES_SD_PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(MICRONES_SD_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(MICRONES_SD_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_pull_up(MICRONES_SD_PIN_MISO);

    /* CS we drive ourselves.  Idle high. */
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

    memset(&s_diag, 0, sizeof(s_diag));
    s_diag.attempted = true;
    s_diag.last_result = SD_ERR_NOT_INITIALIZED;
    memset(s_diag.warmup, 0xFFu, sizeof(s_diag.warmup));
    memset(s_diag.cmd0_responses, 0xFFu, sizeof(s_diag.cmd0_responses));

    sd_pins_init();
    spi_init(sd_spi_block(), MICRONES_SD_INIT_BAUD_HZ);
    spi_set_format(sd_spi_block(), 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    if (!sd_card_present()) {
        s_diag.last_result = SD_ERR_NO_CARD;
        return SD_ERR_NO_CARD;
    }

    /* Power-up sequence: CS high, send 80+ clocks at < 400 kHz with MOSI
     * held high. */
    cs_deassert();
    sleep_ms(2);

    /* Loopback verification probe (FF/00/A5/55).  CS still high so a
     * real card is deselected and won't be confused.  In a hardware
     * loopback (MOSI jumpered to MISO) all four bytes echo. */
    {
        static const uint8_t probe[4] = { 0xFFu, 0x00u, 0xA5u, 0x55u };
        for (int i = 0; i < 4; ++i) {
            s_diag.probe_tx[i] = probe[i];
            s_diag.probe_rx[i] = spi_xfer(probe[i]);
        }
    }

    for (int i = 0; i < 10; ++i) {
        s_diag.warmup[i] = spi_xfer(0xFFu);
    }

    /* CMD0 must be sent with CS asserted. */
    cs_assert();
    SdResult result = SD_OK;
    bool got_idle = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint8_t r = send_cmd(CMD0, 0);
        if (s_diag.cmd0_attempts < (int)(sizeof(s_diag.cmd0_responses)
                                         / sizeof(s_diag.cmd0_responses[0]))) {
            s_diag.cmd0_responses[s_diag.cmd0_attempts] = r;
        }
        ++s_diag.cmd0_attempts;
        if (r == R1_IDLE) {
            got_idle = true;
            break;
        }
        sleep_ms(10);
    }
    if (!got_idle) {
        s_diag.last_result = SD_ERR_CMD0;
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
        result = SD_ERR_CMD8;
        goto fail;
    }

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
        if (send_cmd(CMD16, 512) != 0u) {
            result = SD_ERR_OCR;
            goto fail;
        }
    }

    s_sd.type = sdhc ? SD_TYPE_SDHC : (v2_card ? SD_TYPE_SDV2 : SD_TYPE_SDV1);

    /* CMD9: SEND_CSD. */
    if (send_cmd(CMD9, 0) == 0u) {
        uint8_t csd[16];
        if (read_data_block(csd, sizeof(csd)) == SD_OK) {
            (void)parse_csd_block_count(csd);
        }
    }

    cs_deassert();
    spi_send_dummy(1);

    /* Bump to run baud now that init handshake is done. */
    spi_set_baudrate(sd_spi_block(), MICRONES_SD_RUN_BAUD_HZ);

    s_sd.initialized = true;
    s_diag.last_result = SD_OK;
    return SD_OK;

fail:
    cs_deassert();
    spi_send_dummy(1);
    s_sd.initialized = false;
    s_diag.last_result = result;
    return result;
}

void sd_print_init_diag(void) {
    if (!s_diag.attempted) {
        printf("SD diag: sd_init() not yet called\n");
        return;
    }
    printf("SD diag: pins MISO=%u SCK=%u MOSI=%u CS=%u SPI=%u  result=%d\n",
           (unsigned)MICRONES_SD_PIN_MISO,
           (unsigned)MICRONES_SD_PIN_SCK,
           (unsigned)MICRONES_SD_PIN_MOSI,
           (unsigned)MICRONES_SD_PIN_CS,
           (unsigned)MICRONES_SD_SPI_INDEX,
           (int)s_diag.last_result);
    printf("SD diag: probe (TX->RX):");
    for (int i = 0; i < (int)(sizeof(s_diag.probe_tx) / sizeof(s_diag.probe_tx[0])); ++i) {
        printf(" %02X->%02X", s_diag.probe_tx[i], s_diag.probe_rx[i]);
    }
    printf("\n");
    printf("SD diag: warmup MISO");
    for (int i = 0; i < (int)(sizeof(s_diag.warmup) / sizeof(s_diag.warmup[0])); ++i) {
        printf(" %02X", s_diag.warmup[i]);
    }
    printf("\n");
    printf("SD diag: CMD0 attempts=%d responses", s_diag.cmd0_attempts);
    int n = s_diag.cmd0_attempts;
    int cap = (int)(sizeof(s_diag.cmd0_responses) / sizeof(s_diag.cmd0_responses[0]));
    if (n > cap) n = cap;
    for (int i = 0; i < n; ++i) {
        printf(" %02X", s_diag.cmd0_responses[i]);
    }
    printf("\n");
}

static void probe_one_pin(const char *name, unsigned pin) {
    gpio_set_function(pin, GPIO_FUNC_SIO);
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    for (int rep = 0; rep < 3; ++rep) {
        gpio_put(pin, 1);
        sleep_ms(500);
        bool h = gpio_get(pin);
        gpio_put(pin, 0);
        sleep_ms(500);
        bool l = gpio_get(pin);
        printf("SD probe: %s (GP%u) drive=1 read=%d, drive=0 read=%d\n",
               name, pin, h ? 1 : 0, l ? 1 : 0);
    }
    gpio_put(pin, 1);
}

void sd_run_gpio_probe(void) {
    sd_deinit();

    for (int i = 10; i > 0; --i) {
        printf("SD probe: starting in %d s...\n", i);
        sleep_ms(1000);
    }

    printf("SD probe: starting GPIO walk (9 s).  Multimeter / LED on each pin in turn.\n");
    probe_one_pin("CS",   MICRONES_SD_PIN_CS);
    probe_one_pin("SCK",  MICRONES_SD_PIN_SCK);
    probe_one_pin("MOSI", MICRONES_SD_PIN_MOSI);
    printf("SD probe: done\n");
}

static uint32_t lba_to_address(uint32_t lba) {
    return s_sd.type == SD_TYPE_SDHC ? lba : (lba * 512u);
}

SdResult sd_read_block(uint32_t lba, uint8_t *buf) {
    if (!s_sd.initialized) return SD_ERR_NOT_INITIALIZED;
    cs_assert();
    SdResult r;
    if (send_cmd(CMD17, lba_to_address(lba)) != 0u) {
        r = SD_ERR_READ;
    } else {
        r = read_data_block(buf, 512);
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
    (void)spi_xfer(0xFFu);
    (void)spi_xfer(DATA_TOKEN_BLOCK_READ_WRITE);
    for (int i = 0; i < 512; ++i) {
        (void)spi_xfer(buf[i]);
    }
    (void)spi_xfer(0xFFu);
    (void)spi_xfer(0xFFu);

    uint8_t resp = spi_xfer(0xFFu);
    if ((resp & DATA_RESPONSE_MASK) != DATA_RESPONSE_ACCEPTED) {
        r = SD_ERR_WRITE;
        goto out;
    }

    if (!wait_not_busy(500)) {
        r = SD_ERR_TIMEOUT;
    }

out:
    cs_deassert();
    spi_send_dummy(1);
    return r;
}
