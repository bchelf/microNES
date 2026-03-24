#include "touch.h"
#include "board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "touch";

// ─────────────────────────────────────────────────────────────
//  FT3168 register map
// ─────────────────────────────────────────────────────────────
#define FT_REG_TD_STATUS  0x02   // Number of touch points (lower nibble)
#define FT_REG_P1_XH      0x03   // Touch point 1 X high (bits[3:0])
#define FT_REG_P1_XL      0x04   // Touch point 1 X low
#define FT_REG_P1_YH      0x05   // Touch point 1 Y high (bits[3:0])
#define FT_REG_P1_YL      0x06   // Touch point 1 Y low
// Each subsequent touch point occupies 6 bytes: 0x09, 0x0F, 0x15, 0x1B
static const uint8_t ft_point_base[TOUCH_MAX_POINTS] = {0x03, 0x09, 0x0F, 0x15, 0x1B};

static i2c_master_bus_handle_t  s_bus    = NULL;
static i2c_master_dev_handle_t  s_dev    = NULL;

static bool i2c_read_reg(uint8_t reg, uint8_t *buf, size_t len)
{
    // Use write-then-read combined transfer to avoid a repeated-start issue
    // on some FT series controllers.
    esp_err_t ret = i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 100);
    return (ret == ESP_OK);
}

bool touch_init(void)
{
    // Note: BOARD_TP_RST (GPIO17) is the shared AMOLED/panel reset line.
    // The display driver already toggled it during display_init(); doing it
    // again here would reset the display.  The FT3168 starts up automatically
    // after the shared reset, so we just wait for it to be I2C-ready.
    vTaskDelay(pdMS_TO_TICKS(10));

    // Configure the FT3168 interrupt pin as input with internal pull-up.
    // The FT3168 drives INT low while a touch event is pending and floats it
    // (open-drain) otherwise.  We check this pin before every I2C read to
    // avoid redundant transactions – and to prevent NACKs that occur when the
    // controller is in its low-power idle state (INT is high / not asserted).
    gpio_config_t int_cfg = {
        .pin_bit_mask     = (1ULL << BOARD_TP_INT),
        .mode             = GPIO_MODE_INPUT,
        .pull_up_en       = GPIO_PULLUP_ENABLE,
        .pull_down_en     = GPIO_PULLDOWN_DISABLE,
        .intr_type        = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&int_cfg));

    // Initialise I2C master bus
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port      = BOARD_I2C_PORT,
        .sda_io_num    = BOARD_I2C_SDA,
        .scl_io_num    = BOARD_I2C_SCL,
        .clk_source    = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Add FT3168 device
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BOARD_TP_I2C_ADDR,
        .scl_speed_hz    = BOARD_I2C_FREQ_HZ,
    };
    ret = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "FT3168 touch initialised");
    return true;
}

void touch_read(TouchData *out)
{
    memset(out, 0, sizeof(*out));

    // Fast path: FT3168 keeps INT low while at least one finger is on-screen.
    // When INT is high the controller is idle – skip the I2C transaction
    // entirely.  This avoids ~0.3 ms of bus overhead every frame and prevents
    // the NACK errors that occur when the FT3168 is in low-power idle mode.
    if (gpio_get_level(BOARD_TP_INT) != 0) return;

    uint8_t status;
    if (!i2c_read_reg(FT_REG_TD_STATUS, &status, 1)) return;

    uint8_t count = status & 0x0F;
    if (count > TOUCH_MAX_POINTS) count = TOUCH_MAX_POINTS;
    out->count = count;

    for (uint8_t i = 0; i < count; i++) {
        uint8_t base = ft_point_base[i];
        uint8_t raw[4];
        if (!i2c_read_reg(base, raw, 4)) {
            out->points[i].valid = false;
            continue;
        }
        // raw[0] = XH (bits[3:0] = x[11:8])
        // raw[1] = XL (x[7:0])
        // raw[2] = YH (bits[3:0] = y[11:8])
        // raw[3] = YL (y[7:0])
        uint16_t px = (uint16_t)((raw[0] & 0x0F) << 8) | raw[1];
        uint16_t py = (uint16_t)((raw[2] & 0x0F) << 8) | raw[3];

        // Convert from native portrait (240×536) to landscape (536×240).
        // The display is rotated 90° CW via MADCTL, so:
        //   landscape_x = portrait_y
        //   landscape_y = (BOARD_LCD_NATIVE_W - 1) - portrait_x
        // If touches appear mirrored, swap to:
        //   landscape_x = (BOARD_LCD_NATIVE_H - 1) - portrait_y
        //   landscape_y = portrait_x
        out->points[i].x     = py;
        out->points[i].y     = (uint16_t)(BOARD_LCD_NATIVE_W - 1) - px;
        out->points[i].valid = true;
    }
}
