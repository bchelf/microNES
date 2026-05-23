#include "display_config.h"
#include "display_transport.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/time.h"

#include <stdarg.h>
#include <stdio.h>

#if defined(MICRONES_DISPLAY_BACKEND_SPI_ILI9341)

static char s_last_error[128];

static inline spi_inst_t *display_spi(void) {
    return spi1;
}

static void spi_tft_set_error(const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, args);
    va_end(args);
}

static inline void spi_tft_select(void) {
    gpio_put(MICRONES_TFT_PIN_CS, 0);
}

static inline void spi_tft_deselect(void) {
    gpio_put(MICRONES_TFT_PIN_CS, 1);
}

static bool spi_tft_init(void) {
    const uint32_t baud = spi_init(display_spi(), MICRONES_TFT_SPI_BAUD_HZ);

    s_last_error[0] = '\0';

    gpio_init(MICRONES_TFT_PIN_CS);
    gpio_set_dir(MICRONES_TFT_PIN_CS, GPIO_OUT);
    spi_tft_deselect();

    gpio_init(MICRONES_TFT_PIN_RS);
    gpio_set_dir(MICRONES_TFT_PIN_RS, GPIO_OUT);
    gpio_put(MICRONES_TFT_PIN_RS, 1);

    gpio_init(MICRONES_TFT_PIN_RST);
    gpio_set_dir(MICRONES_TFT_PIN_RST, GPIO_OUT);
    gpio_put(MICRONES_TFT_PIN_RST, 1);

    gpio_init(MICRONES_TFT_PIN_BL);
    gpio_set_dir(MICRONES_TFT_PIN_BL, GPIO_OUT);
    gpio_put(MICRONES_TFT_PIN_BL, 0);

    gpio_set_function(MICRONES_TFT_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(MICRONES_TFT_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(MICRONES_TFT_PIN_MISO, GPIO_FUNC_SPI);
    spi_set_format(display_spi(), 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    sleep_ms(5);
    gpio_put(MICRONES_TFT_PIN_RST, 0);
    sleep_ms(20);
    gpio_put(MICRONES_TFT_PIN_RST, 1);
    sleep_ms(150);
    gpio_put(MICRONES_TFT_PIN_BL, 1);

    if (baud == 0u) {
        spi_tft_set_error("spi_init returned 0");
        return false;
    }

    return true;
}

static const char *spi_tft_last_error(void) {
    return s_last_error;
}

static void spi_tft_write_command(uint8_t cmd) {
    gpio_put(MICRONES_TFT_PIN_RS, 0);
    spi_tft_select();
    spi_write_blocking(display_spi(), &cmd, 1);
    spi_tft_deselect();
}

static void spi_tft_write_data_blocking(const uint8_t *data, size_t len) {
    if (len == 0u) {
        return;
    }
    gpio_put(MICRONES_TFT_PIN_RS, 1);
    spi_tft_select();
    spi_write_blocking(display_spi(), data, len);
    spi_tft_deselect();
}

static void spi_tft_begin_pixels(void) {
    gpio_put(MICRONES_TFT_PIN_RS, 1);
    spi_tft_select();
}

static void spi_tft_write_pixels_blocking(const uint8_t *data, size_t len) {
    if (len == 0u) {
        return;
    }
    spi_write_blocking(display_spi(), data, len);
}

static void spi_tft_write_pixels_dma(const uint8_t *data, size_t len) {
    spi_tft_write_pixels_blocking(data, len);
}

static void spi_tft_wait_idle(void) {
}

static void spi_tft_end_pixels(void) {
    spi_tft_deselect();
}

static const TftTransportOps k_spi_ops = {
    .name = "spi_ili9341",
    .init = spi_tft_init,
    .last_error = spi_tft_last_error,
    .write_command = spi_tft_write_command,
    .write_data_blocking = spi_tft_write_data_blocking,
    .begin_pixels = spi_tft_begin_pixels,
    .write_pixels_blocking = spi_tft_write_pixels_blocking,
    .write_pixels_dma = spi_tft_write_pixels_dma,
    .wait_idle = spi_tft_wait_idle,
    .end_pixels = spi_tft_end_pixels,
};

const TftTransportOps *tft_display_transport_get(void) {
    return &k_spi_ops;
}

#endif
