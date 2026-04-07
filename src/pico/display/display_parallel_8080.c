#include "display_parallel_8080.h"

#include "display_config.h"
#include "display_transport.h"
#include "parallel_tft.pio.h"

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/time.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#if defined(MICRONES_DISPLAY_BACKEND_PARALLEL_8080)

enum {
    TFT_CMD_CASET = 0x2A,
    TFT_CMD_PASET = 0x2B,
    TFT_CMD_RAMWR = 0x2C,
};

static char s_last_error[128];
static int s_program_offset = -1;
static int s_dma_chan = -1;
static bool s_dma_active = false;

static inline PIO parallel_tft_pio(void) {
    return MICRONES_TFT_PARALLEL_PIO;
}

static inline uint parallel_tft_sm(void) {
    return MICRONES_TFT_PARALLEL_SM;
}

static void parallel_tft_set_error(const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, args);
    va_end(args);
}

static inline void parallel_tft_select(void) {
    if (MICRONES_TFT_PIN_CS == MICRONES_TFT_PIN_UNUSED) {
        return;
    }
    gpio_put(MICRONES_TFT_PIN_CS, 0);
}

static inline void parallel_tft_deselect(void) {
    if (MICRONES_TFT_PIN_CS == MICRONES_TFT_PIN_UNUSED) {
        return;
    }
#if MICRONES_TFT_PARALLEL_KEEP_CS_ASSERTED
    gpio_put(MICRONES_TFT_PIN_CS, 0);
#else
    gpio_put(MICRONES_TFT_PIN_CS, 1);
#endif
}

bool parallel_tft_init(void) {
    PIO pio = parallel_tft_pio();
    uint sm = parallel_tft_sm();
    pio_sm_config config;
    dma_channel_config dma_cfg;

    s_last_error[0] = '\0';

    gpio_init(MICRONES_TFT_PIN_RS);
    gpio_set_dir(MICRONES_TFT_PIN_RS, GPIO_OUT);
    gpio_put(MICRONES_TFT_PIN_RS, 1);

    if (MICRONES_TFT_PIN_CS != MICRONES_TFT_PIN_UNUSED) {
        gpio_init(MICRONES_TFT_PIN_CS);
        gpio_set_dir(MICRONES_TFT_PIN_CS, GPIO_OUT);
        parallel_tft_deselect();
    }

    gpio_init(MICRONES_TFT_PIN_RST);
    gpio_set_dir(MICRONES_TFT_PIN_RST, GPIO_OUT);
    gpio_put(MICRONES_TFT_PIN_RST, 1);

    if (MICRONES_TFT_PIN_RD != MICRONES_TFT_PIN_UNUSED) {
        gpio_init(MICRONES_TFT_PIN_RD);
        gpio_set_dir(MICRONES_TFT_PIN_RD, GPIO_OUT);
        gpio_put(MICRONES_TFT_PIN_RD, 1);
    }

    if (MICRONES_TFT_PIN_BL != MICRONES_TFT_PIN_UNUSED) {
        gpio_init(MICRONES_TFT_PIN_BL);
        gpio_set_dir(MICRONES_TFT_PIN_BL, GPIO_OUT);
        gpio_put(MICRONES_TFT_PIN_BL, 1);
    }

    for (uint pin = MICRONES_TFT_PIN_D0_BASE; pin < MICRONES_TFT_PIN_D0_BASE + 8u; ++pin) {
        pio_gpio_init(pio, pin);
    }
    pio_gpio_init(pio, MICRONES_TFT_PIN_WR);

    if (s_program_offset < 0) {
        s_program_offset = (int)pio_add_program(pio, &parallel_tft_program);
    }

    config = parallel_tft_program_get_default_config((uint)s_program_offset);
    sm_config_set_out_pins(&config, MICRONES_TFT_PIN_D0_BASE, 8u);
    sm_config_set_sideset_pins(&config, MICRONES_TFT_PIN_WR);
    /*
     * threshold=8: the SM autopulls after every 8-bit OUT, consuming one byte
     * per 32-bit FIFO word.  This matches pio_sm_put_blocking single-byte writes
     * from write_command / write_data_blocking, and DMA_SIZE_8 pixel transfers
     * that the DMA controller zero-extends to 32 bits per entry.
     */
    sm_config_set_out_shift(&config, true, true, 8u);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&config, MICRONES_TFT_PARALLEL_CLKDIV);

    pio_sm_set_consecutive_pindirs(pio, sm, MICRONES_TFT_PIN_D0_BASE, 8u, true);
    pio_sm_set_consecutive_pindirs(pio, sm, MICRONES_TFT_PIN_WR, 1u, true);
    pio_sm_init(pio, sm, (uint)s_program_offset, &config);
    pio_sm_set_enabled(pio, sm, true);

    s_dma_chan = dma_claim_unused_channel(true);
    dma_cfg = dma_channel_get_default_config((uint)s_dma_chan);
    /*
     * DMA_SIZE_8: each DMA transaction writes one byte (zero-extended to 32 bits
     * by the AHB bus) into the PIO TX FIFO.  With threshold=8, the SM emits that
     * byte on the next OUT and autopulls for the next entry.  This lets
     * write_pixels_dma point the DMA directly at the pixel byte buffer — no
     * expansion loop, no staging buffer.  RP2350 AHB narrow writes to FIFO
     * registers work the same way as 8-bit SPI DMA in the pico-sdk.
     */
    channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_8);
    channel_config_set_dreq(&dma_cfg, pio_get_dreq(pio, sm, true));
    channel_config_set_read_increment(&dma_cfg, true);
    channel_config_set_write_increment(&dma_cfg, false);
    dma_channel_configure((uint)s_dma_chan, &dma_cfg, &pio->txf[sm], NULL, 0u, false);

    sleep_ms(5);
    gpio_put(MICRONES_TFT_PIN_RST, 0);
    sleep_ms(20);
    gpio_put(MICRONES_TFT_PIN_RST, 1);
    sleep_ms(150);
    parallel_tft_select();

    printf("tft: transport=%s clkdiv=%.2f data_base=%u wr=%u rs=%u cs=%u rst=%u rd=%u\n",
           "parallel_8080",
           (double)MICRONES_TFT_PARALLEL_CLKDIV,
           MICRONES_TFT_PIN_D0_BASE,
           MICRONES_TFT_PIN_WR,
           MICRONES_TFT_PIN_RS,
           MICRONES_TFT_PIN_CS,
           MICRONES_TFT_PIN_RST,
           MICRONES_TFT_PIN_RD);
    return true;
}

const char *parallel_tft_last_error(void) {
    return s_last_error;
}

void parallel_tft_wait_for_completion(void) {
    if (!s_dma_active) {
        return;
    }
    dma_channel_wait_for_finish_blocking((uint)s_dma_chan);
    s_dma_active = false;
}

/*
 * Wait until the PIO state machine has physically shifted out all bytes.
 * FDEBUG TXSTALL is set when the SM tries to execute 'out' or 'pull' with an
 * empty TX FIFO — meaning all previously pushed bytes have been clocked onto
 * the bus.  Must be called before changing RS so we don't flip RS while a
 * command byte is still being shifted out.
 */
static void parallel_tft_flush_pio(void) {
    PIO pio = parallel_tft_pio();
    uint sm = parallel_tft_sm();
    uint32_t stall_mask = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);

    pio->fdebug = stall_mask;   /* clear the stall flag */
    while (!(pio->fdebug & stall_mask)) {
        tight_loop_contents();
    }
}

void parallel_tft_write_command(uint8_t cmd) {
    parallel_tft_wait_for_completion();
    parallel_tft_flush_pio();
    parallel_tft_select();
    gpio_put(MICRONES_TFT_PIN_RS, 0);
    pio_sm_put_blocking(parallel_tft_pio(), parallel_tft_sm(), (uint32_t)cmd);
}

void parallel_tft_write_data_blocking(const uint8_t *data, size_t len) {
    PIO pio = parallel_tft_pio();
    uint sm = parallel_tft_sm();

    parallel_tft_wait_for_completion();
    parallel_tft_flush_pio();
    parallel_tft_select();
    gpio_put(MICRONES_TFT_PIN_RS, 1);
    for (size_t i = 0u; i < len; ++i) {
        pio_sm_put_blocking(pio, sm, (uint32_t)data[i]);
    }
}

void parallel_tft_begin_pixels(void) {
    parallel_tft_wait_for_completion();
    parallel_tft_flush_pio();
    parallel_tft_select();
    gpio_put(MICRONES_TFT_PIN_RS, 1);
}

void parallel_tft_write_pixels_blocking(const uint8_t *data, size_t len) {
    PIO pio = parallel_tft_pio();
    uint sm = parallel_tft_sm();

    parallel_tft_begin_pixels();
    for (size_t i = 0u; i < len; ++i) {
        pio_sm_put_blocking(pio, sm, (uint32_t)data[i]);
    }
}

void parallel_tft_write_pixels_dma(const uint8_t *data, size_t len) {
    /*
     * DMA_SIZE_8: each transfer writes one source byte to &pio->txf[sm].
     * The AHB bus zero-extends it to 32 bits; the PIO SM emits bits[7:0] per
     * threshold=8 autopull.  The source buffer is used directly — no expansion
     * loop or staging buffer.
     */
    parallel_tft_wait_for_completion();
    parallel_tft_select();
    gpio_put(MICRONES_TFT_PIN_RS, 1);
    if (len == 0u) {
        return;
    }

    dma_channel_set_read_addr((uint)s_dma_chan, data, false);
    dma_channel_set_trans_count((uint)s_dma_chan, len, false);
    s_dma_active = true;
    dma_channel_start((uint)s_dma_chan);
    parallel_tft_wait_for_completion();
}

void parallel_tft_end_pixels(void) {
    parallel_tft_wait_for_completion();
    parallel_tft_deselect();
}

void parallel_tft_set_window(int x0, int y0, int x1, int y1) {
    uint8_t caset[4] = {
        (uint8_t)((uint16_t)x0 >> 8), (uint8_t)x0,
        (uint8_t)((uint16_t)x1 >> 8), (uint8_t)x1,
    };
    uint8_t paset[4] = {
        (uint8_t)((uint16_t)y0 >> 8), (uint8_t)y0,
        (uint8_t)((uint16_t)y1 >> 8), (uint8_t)y1,
    };

    parallel_tft_write_command(TFT_CMD_CASET);
    parallel_tft_write_data_blocking(caset, sizeof(caset));
    parallel_tft_write_command(TFT_CMD_PASET);
    parallel_tft_write_data_blocking(paset, sizeof(paset));
    parallel_tft_write_command(TFT_CMD_RAMWR);
}

void parallel_tft_blit_rect_rgb565(int x, int y, int w, int h, const uint16_t *src, int stride) {
    if (w <= 0 || h <= 0 || src == NULL) {
        return;
    }

    parallel_tft_set_window(x, y, x + w - 1, y + h - 1);
    parallel_tft_begin_pixels();
    for (int row = 0; row < h; ++row) {
        const uint8_t *row_bytes = (const uint8_t *)(const void *)(src + row * stride);
        parallel_tft_write_pixels_dma(row_bytes, (size_t)w * sizeof(uint16_t));
        parallel_tft_wait_for_completion();
    }
    parallel_tft_end_pixels();
}

static bool parallel_tft_transport_init(void) {
    if (!parallel_tft_init()) {
        if (s_last_error[0] == '\0') {
            parallel_tft_set_error("parallel init failed");
        }
        return false;
    }
    return true;
}

static const TftTransportOps k_parallel_ops = {
    .name = "parallel_8080",
    .init = parallel_tft_transport_init,
    .last_error = parallel_tft_last_error,
    .write_command = parallel_tft_write_command,
    .write_data_blocking = parallel_tft_write_data_blocking,
    .begin_pixels = parallel_tft_begin_pixels,
    .write_pixels_blocking = parallel_tft_write_pixels_blocking,
    .write_pixels_dma = parallel_tft_write_pixels_dma,
    .wait_idle = parallel_tft_wait_for_completion,
    .end_pixels = parallel_tft_end_pixels,
};

const TftTransportOps *tft_display_transport_get(void) {
    return &k_parallel_ops;
}

#endif
