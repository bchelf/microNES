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
//  RM67162 QSPI command definitions (MIPI DCS subset)
//  This is the AMOLED driver chip on the Waveshare 1.91" board.
// ─────────────────────────────────────────────────────────────
#define LCD_SLPOUT   0x11   // Sleep out
#define LCD_DISPON   0x29   // Display on
#define LCD_CASET    0x2A   // Column address set
#define LCD_RASET    0x2B   // Row address set
#define LCD_RAMWR    0x2C   // Memory write
#define LCD_MADCTL   0x36   // Memory access control (rotation)
#define LCD_COLMOD   0x3A   // Colour mode
#define LCD_WRDISBV  0x51   // Write display brightness

// MADCTL bits – 90° CW landscape: MX=1, MV=1 → 0x60
#define MADCTL_MX   0x40
#define MADCTL_MV   0x20
#define MADCTL_LANDSCAPE  (MADCTL_MX | MADCTL_MV)

// Colour mode: 16-bit RGB565
#define COLMOD_RGB565   0x55

// ─────────────────────────────────────────────────────────────
//  RM67162 QSPI instruction bytes
//  0x02: write command/data in 1-bit SPI mode (cmd+addr 1-bit, data 1-bit)
//  0x32: write pixel data in quad mode  (cmd+addr 1-bit, data 4-bit)
//
//  Address field layout (24-bit, MSB first):
//    byte[0] = 0x00
//    byte[1] = DCS command (e.g. 0x2C for RAMWR)
//    byte[2] = 0x00
//  → addr = cmd << 8   (e.g. 0x2C << 8 = 0x002C00)
// ─────────────────────────────────────────────────────────────
#define QSPI_INST_WRITE_SINGLE  0x02
#define QSPI_INST_WRITE_QUAD    0x32

// DMA-capable scratch buffer in internal SRAM (used by display_fill,
// display_write_region and display_blit_region for chunked transfers).
// Sized for one full NES scanline (256 px × 2 bytes = 512 bytes).
#define ROW_BUF_PIXELS  256
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
    // RM67162 QSPI protocol: byte[1] of the 3-byte address carries the DCS command:
    //   byte[0] = 0x00  (addr[23:16])
    //   byte[1] = cmd   (addr[15:8])   ← cmd << 8
    //   byte[2] = 0x00  (addr[7:0])
    spi_transaction_ext_t t = {
        .base = {
            .flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR,
            .cmd   = QSPI_INST_WRITE_SINGLE,
            .addr  = (uint32_t)cmd << 8,
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
            .addr   = (uint32_t)LCD_RAMWR << 8,   // 0x002C00
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
    lcd_cmd(LCD_CASET, caset, 4);
    lcd_cmd(LCD_RASET, raset, 4);
}

// ─────────────────────────────────────────────────────────────
//  RM67162 QSPI initialisation sequence
//  Matches the rm67162_qspi_init[] table from the LilyGo/Waveshare reference.
// ─────────────────────────────────────────────────────────────
static void rm67162_init_sequence(void)
{
    // Hardware reset: pull low 20 ms, then release and let the panel stabilise
    gpio_set_level(BOARD_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    // Sleep out – RM67162 needs 120 ms before any further commands
    lcd_cmd(LCD_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    // 16-bit RGB565 colour mode
    uint8_t colmod = COLMOD_RGB565;
    lcd_cmd(LCD_COLMOD, &colmod, 1);

    // Landscape rotation 90° CW: MADCTL MX | MV = 0x60
    uint8_t madctl = MADCTL_LANDSCAPE;
    lcd_cmd(LCD_MADCTL, &madctl, 1);

    // Brightness: initialise to 0 before display-on
    uint8_t brt0 = 0x00;
    lcd_cmd(LCD_WRDISBV, &brt0, 1);

    // Display on – wait 120 ms
    lcd_cmd(LCD_DISPON, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    // Brightness: ramp to full (0xD0)
    uint8_t brt1 = 0xD0;
    lcd_cmd(LCD_WRDISBV, &brt1, 1);
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
        // Must hold one scanline (ROW_BUF_PIXELS × 2 bytes) plus overhead.
        .max_transfer_sz = ROW_BUF_PIXELS * 2 + 64,
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

    rm67162_init_sequence();
    ESP_LOGI(TAG, "RM67162 AMOLED initialised (%dx%d landscape)", DISPLAY_W, DISPLAY_H);
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

// Blit a pre-rendered buffer to the display one scanline at a time.
// Uses the same streaming path as the UI overlay (proven working) so each
// SPI transaction is exactly ROW_BUF_PIXELS × 2 bytes – well within any
// hardware limit.  memcpy into s_row_buf also guarantees DMA-safe source.
void display_blit_region(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         const uint16_t *buf)
{
    display_stream_begin(x, y, w, h);
    for (uint16_t row = 0; row < h; row++) {
        display_stream_row(buf + (uint32_t)row * w, w);
    }
    display_stream_end();
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
