#include "tft_controller.h"

#include "pico/time.h"

enum {
    TFT_CMD_SWRESET = 0x01,
    TFT_CMD_SLPOUT = 0x11,
    TFT_CMD_DISPON = 0x29,
    TFT_CMD_CASET = 0x2A,
    TFT_CMD_PASET = 0x2B,
    TFT_CMD_RAMWR = 0x2C,
    TFT_CMD_MADCTL = 0x36,
    TFT_CMD_COLMOD = 0x3A,
};

static void ili9341_init(const TftTransportOps *transport) {
    static const uint8_t colmod = 0x55;
    static const uint8_t madctl = 0x28; /* MV|BGR: 320x240 landscape */
    static const TftControllerInitCommand sequence[] = {
        { TFT_CMD_SWRESET, NULL, 0u, 150u },
        { TFT_CMD_SLPOUT,  NULL, 0u, 120u },
        { TFT_CMD_COLMOD, &colmod, 1u, 0u },
        { TFT_CMD_MADCTL, &madctl, 1u, 0u },
        { TFT_CMD_DISPON, NULL, 0u, 120u },
    };

    tft_controller_run_init_sequence(transport, sequence, sizeof(sequence) / sizeof(sequence[0]));
}

void tft_controller_run_init_sequence(
    const TftTransportOps *transport,
    const TftControllerInitCommand *sequence,
    size_t count
) {
    for (size_t i = 0; i < count; ++i) {
        transport->write_command(sequence[i].cmd);
        if (sequence[i].len > 0u) {
            transport->write_data_blocking(sequence[i].data, sequence[i].len);
        }
        if (sequence[i].delay_ms > 0u) {
            sleep_ms(sequence[i].delay_ms);
        }
    }
}

void tft_controller_write_standard_window(
    const TftTransportOps *transport,
    uint16_t x0,
    uint16_t y0,
    uint16_t x1,
    uint16_t y1
) {
    uint8_t caset[4] = {
        (uint8_t)(x0 >> 8), (uint8_t)x0,
        (uint8_t)(x1 >> 8), (uint8_t)x1,
    };
    uint8_t paset[4] = {
        (uint8_t)(y0 >> 8), (uint8_t)y0,
        (uint8_t)(y1 >> 8), (uint8_t)y1,
    };

    transport->write_command(TFT_CMD_CASET);
    transport->write_data_blocking(caset, sizeof(caset));
    transport->write_command(TFT_CMD_PASET);
    transport->write_data_blocking(paset, sizeof(paset));
    transport->write_command(TFT_CMD_RAMWR);
}

#if defined(MICRONES_DISPLAY_CONTROLLER_ILI9341)
static const TftController k_controller = {
    .name = "ili9341",
    .width = 320u,
    .height = 240u,
    .viewport_x = 32u,
    .viewport_y = 0u,
    .init = ili9341_init,
};

const TftController *tft_controller_get(void) {
    return &k_controller;
}
#endif
