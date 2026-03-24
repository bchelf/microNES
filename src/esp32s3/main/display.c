#include "display.h"
#include "board.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "display";

// ─────────────────────────────────────────────────────────────
//  SH8601 QSPI command definitions (MIPI DCS subset)
// ─────────────────────────────────────────────────────────────
#define SH8601_SWRESET  0x01
#define SH8601_SLPOUT   0x11
#define SH8601_NORON    0x13
#define SH8601_INVON    0x21   // inversion needed for AMOLED correct brightness
#define SH8601_DISPON   0x29
#define SH8601_CASET    0x2A   // column address set
#define SH8601_RASET    0x2B   // row address set
#define SH8601_RAMWR    0x2C   // memory write
#define SH8601_MADCTL   0x36   // memory access control (rotation)
#define SH8601_COLMOD   0x3A   // colour mode

// MADCTL bits
#define MADCTL_MY   0x80
#define MADCTL_MX   0x40
#define MADCTL_MV   0x20
#define MADCTL_ML   0x10
#define MADCTL_BGR  0x08
// 90° CW rotation: MX=1, MV=1 → 0x60
#define MADCTL_LANDSCAPE  (MADCTL_MX | MADCTL_MV)

// Colour mode: 16-bit RGB565
#define COLMOD_RGB565   0x55

// ─────────────────────────────────────────────────────────────
//  SPI QSPI instruction bytes (SH8601 protocol)
//  Instruction 0x02: write, cmd+addr 1-bit, data 1-bit
//  Instruction 0x32: write, cmd+addr 1-bit, data 4-bit (quad)
// ─────────────────────────────────────────────────────────────
#define QSPI_INST_WRITE_SINGLE  0x02
#define QSPI_INST_WRITE_QUAD    0x32

// DMA-capable scratch buffer in internal SRAM (used by display_fill and
// display_write_region for chunked transfers from non-DMA sources).
#define ROW_BUF_PIXELS  512
static DMA_ATTR uint8_t s_row_buf[ROW_BUF_PIXELS * 2];

static spi_device_handle_t s_spi = NULL;
static bool s_streaming = false;

// ─────────────────────────────────────────────────────────────
//  Low-level QSPI helpers
// ─────────────────────────────────────────────────────────────

// Send a DCS command with 0–4 bytes of parameters.
// cmd + params transferred in single-bit SPI mode.
static void lcd_cmd(uint8_t cmd, const uint8_t *params, size_t n_params)
{
    // Pack command into the 24-bit SPI address field.
    // SH8601 expects the DCS command byte in addr[23:16] (MSB first):
    //   addr[23:16] = cmd
    //   addr[15:8]  = 0x00
    //   addr[7:0]   = 0x00
    spi_transaction_ext_t t = {
        .base = {
            .flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR,
            .cmd   = QSPI_INST_WRITE_SINGLE,
            .addr  = (uint32_t)cmd << 16,
            .tx_buffer = (n_params > 0) ? params : NULL,
            .length    = n_params * 8,
        },
        .command_bits = 8,
        .address_bits = 24,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t));
}

// Begin a pixel-data write: send RAMWR in quad mode, keep CS active.
// Caller must follow up with lcd_pixel_chunk() calls and finish with
// lcd_pixel_end().
// ESP-IDF rule: spi_device_acquire_bus() MUST be called before any
// transaction that uses SPI_TRANS_CS_KEEP_ACTIVE, otherwise polling
// transactions cannot obtain the bus lock.
static void lcd_pixel_begin(void)
{
    ESP_ERROR_CHECK(spi_device_acquire_bus(s_spi, portMAX_DELAY));
    spi_transaction_ext_t t = {
        .base = {
            .flags  = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR
                    | SPI_TRANS_MODE_QIO | SPI_TRANS_CS_KEEP_ACTIVE,
            .cmd    = QSPI_INST_WRITE_QUAD,
            .addr   = (uint32_t)SH8601_RAMWR << 16,
            .tx_buffer = NULL,
            .length    = 0,
        },
        .command_bits = 8,
        .address_bits = 24,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t));
}

// Send a chunk of pixel data.  buf must be DMA-accessible.
// keep_cs: true  = CS stays active (intermediate chunk, bus stays acquired)
//          false = CS deasserts    (final chunk, bus is released)
static void lcd_pixel_chunk(const void *buf, size_t len_bytes, bool keep_cs)
{
    spi_transaction_ext_t t = {
        .base = {
            .flags  = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR
                    | SPI_TRANS_MODE_QIO
                    | (keep_cs ? SPI_TRANS_CS_KEEP_ACTIVE : 0),
            .cmd    = 0,
            .addr   = 0,
            .tx_buffer = buf,
            .length    = len_bytes * 8,
        },
        .command_bits = 0,
        .address_bits = 0,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t));
    if (!keep_cs) {
        spi_device_release_bus(s_spi);
    }
}

// Set the CASET/RASET write window in landscape coordinates.
static void lcd_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    uint8_t caset[4] = { x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF };
    uint8_t raset[4] = { y1 >> 8, y1 & 0xFF, y2 >> 8, y2 & 0xFF };
    lcd_cmd(SH8601_CASET, caset, 4);
    lcd_cmd(SH8601_RASET, raset, 4);
}

// ─────────────────────────────────────────────────────────────
//  SH8601 initialisation sequence
// ─────────────────────────────────────────────────────────────
static void sh8601_init_sequence(void)
{
    // Hardware reset
    gpio_set_level(BOARD_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    // Software reset
    lcd_cmd(SH8601_SWRESET, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    // Sleep out
    lcd_cmd(SH8601_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    // 16-bit RGB565 colour mode
    uint8_t colmod = COLMOD_RGB565;
    lcd_cmd(SH8601_COLMOD, &colmod, 1);

    // Landscape rotation: 90° CW
    uint8_t madctl = MADCTL_LANDSCAPE;
    lcd_cmd(SH8601_MADCTL, &madctl, 1);

    // Enable display inversion (typical for AMOLED panels)
    lcd_cmd(SH8601_INVON, NULL, 0);

    // Normal display mode
    lcd_cmd(SH8601_NORON, NULL, 0);

    // Set full-panel write window (landscape 536×240)
    lcd_set_window(0, 0, DISPLAY_W - 1, DISPLAY_H - 1);

    // Display on
    lcd_cmd(SH8601_DISPON, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
}

// ─────────────────────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────────────────────

bool display_init(void)
{
    // Configure reset GPIO
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << BOARD_LCD_RST),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));
    gpio_set_level(BOARD_LCD_RST, 1);

    // Initialise SPI bus with QSPI pins
    spi_bus_config_t buscfg = {
        .mosi_io_num     = BOARD_LCD_QSPI_D0,
        .miso_io_num     = BOARD_LCD_QSPI_D1,
        .sclk_io_num     = BOARD_LCD_QSPI_SCLK,
        .quadwp_io_num   = BOARD_LCD_QSPI_D2,
        .quadhd_io_num   = BOARD_LCD_QSPI_D3,
        .data4_io_num    = -1,
        .data5_io_num    = -1,
        .data6_io_num    = -1,
        .data7_io_num    = -1,
        // Allow transfers up to one full NES frame (256×240 × 2 bytes) plus overhead
        .max_transfer_sz = (256 * 240 * 2) + 64,
        .flags           = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD,
    };

    esp_err_t ret = spi_bus_initialize(BOARD_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Add the SH8601 device.
    // command_bits=8 and address_bits=24 are the per-device defaults;
    // individual transactions override these via SPI_TRANS_VARIABLE_*.
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = BOARD_LCD_SPI_CLK_HZ,
        .mode           = 0,  // SPI mode 0
        .spics_io_num   = BOARD_LCD_QSPI_CS,
        .queue_size     = 4,
        .command_bits   = 8,
        .address_bits   = 24,
        .flags          = SPI_DEVICE_HALFDUPLEX,
        .pre_cb         = NULL,
        .post_cb        = NULL,
    };

    ret = spi_bus_add_device(BOARD_LCD_SPI_HOST, &devcfg, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(ret));
        spi_bus_free(BOARD_LCD_SPI_HOST);
        return false;
    }

    sh8601_init_sequence();
    ESP_LOGI(TAG, "SH8601 AMOLED initialised (%dx%d landscape)", DISPLAY_W, DISPLAY_H);
    return true;
}

void display_fill(uint16_t rgb565_be)
{
    // Fill row buffer with the given colour
    for (int i = 0; i < ROW_BUF_PIXELS; i++) {
        s_row_buf[i * 2]     = rgb565_be >> 8;
        s_row_buf[i * 2 + 1] = rgb565_be & 0xFF;
    }

    lcd_set_window(0, 0, DISPLAY_W - 1, DISPLAY_H - 1);
    lcd_pixel_begin();

    // DISPLAY_W × DISPLAY_H pixels; send in ROW_BUF_PIXELS-pixel chunks
    uint32_t total_pixels = (uint32_t)DISPLAY_W * DISPLAY_H;
    uint32_t sent = 0;
    while (sent < total_pixels) {
        uint32_t chunk = total_pixels - sent;
        if (chunk > ROW_BUF_PIXELS) chunk = ROW_BUF_PIXELS;
        bool last = (sent + chunk >= total_pixels);
        lcd_pixel_chunk(s_row_buf, chunk * 2, !last);
        sent += chunk;
    }
}

void display_write_region(uint16_t x, uint16_t y,
                          uint16_t w, uint16_t h,
                          const uint16_t *data)
{
    lcd_set_window(x, y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));
    lcd_pixel_begin();

    uint32_t total_pixels = (uint32_t)w * h;
    uint32_t sent = 0;
    const uint8_t *src = (const uint8_t *)data;

    while (sent < total_pixels) {
        uint32_t chunk = total_pixels - sent;
        if (chunk > ROW_BUF_PIXELS) chunk = ROW_BUF_PIXELS;
        // Copy into DMA-safe buffer (also handles PSRAM source)
        memcpy(s_row_buf, src + sent * 2, chunk * 2);
        bool last = (sent + chunk >= total_pixels);
        lcd_pixel_chunk(s_row_buf, chunk * 2, !last);
        sent += chunk;
    }
}

void display_write_row(uint16_t x, uint16_t row_y, uint16_t w,
                       const uint16_t *row)
{
    display_write_region(x, row_y, w, 1, row);
}

// Send a pre-rendered, DMA-accessible buffer directly – no intermediate copy.
// buf must reside in internal SRAM (always the case when PSRAM is disabled).
// This is the fast path for full-frame NES blits (one SPI transaction).
void display_blit_region(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         const uint16_t *buf)
{
    lcd_set_window(x, y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));
    ESP_ERROR_CHECK(spi_device_acquire_bus(s_spi, portMAX_DELAY));

    // RAMWR header in quad mode, CS stays active
    spi_transaction_ext_t hdr = {
        .base = {
            .flags  = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR
                    | SPI_TRANS_MODE_QIO | SPI_TRANS_CS_KEEP_ACTIVE,
            .cmd    = QSPI_INST_WRITE_QUAD,
            .addr   = (uint32_t)SH8601_RAMWR << 16,
            .tx_buffer = NULL,
            .length    = 0,
        },
        .command_bits = 8,
        .address_bits = 24,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, (spi_transaction_t *)&hdr));

    // Pixel data – single large DMA transaction, CS deasserts at end
    spi_transaction_ext_t px = {
        .base = {
            .flags  = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR
                    | SPI_TRANS_MODE_QIO,
            .cmd    = 0,
            .addr   = 0,
            .tx_buffer = buf,
            .length    = (uint32_t)w * h * 16,  // bits
        },
        .command_bits = 0,
        .address_bits = 0,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, (spi_transaction_t *)&px));
    spi_device_release_bus(s_spi);
}

// ── Streaming API ──────────────────────────────────────────────
static uint32_t s_stream_total_px;
static uint32_t s_stream_sent_px;
static uint16_t s_stream_w;

void display_stream_begin(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    s_stream_total_px = (uint32_t)w * h;
    s_stream_sent_px  = 0;
    s_stream_w        = w;
    s_streaming       = true;
    lcd_set_window(x, y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));
    lcd_pixel_begin();
}

void display_stream_row(const uint16_t *row, uint16_t w)
{
    if (!s_streaming) return;
    // row width should equal s_stream_w, but we trust the caller
    uint32_t remaining = s_stream_total_px - s_stream_sent_px;
    uint32_t pixels = (w < remaining) ? w : remaining;
    memcpy(s_row_buf, row, pixels * 2);
    s_stream_sent_px += pixels;
    bool last = (s_stream_sent_px >= s_stream_total_px);
    lcd_pixel_chunk(s_row_buf, pixels * 2, !last);
    if (last) s_streaming = false;
}

void display_stream_end(void)
{
    // Called before all rows were pushed: deassert CS and release the bus.
    if (s_streaming) {
        // A transaction without CS_KEEP_ACTIVE deasserts CS cleanly.
        // bus is still acquired so the transaction goes through fine.
        spi_transaction_ext_t t = {
            .base = {
                .flags  = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR
                        | SPI_TRANS_MODE_QIO,
                .cmd    = 0,
                .addr   = 0,
                .tx_buffer = s_row_buf,  // must be non-NULL for DMA path
                .length    = 8,          // 1 dummy byte – enough to deassert CS
            },
            .command_bits = 0,
            .address_bits = 0,
        };
        spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t);
        spi_device_release_bus(s_spi);
        s_streaming = false;
    }
}
