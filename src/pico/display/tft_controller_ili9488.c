#include "tft_controller.h"

enum {
    TFT_CMD_SWRESET = 0x01,
    TFT_CMD_SLPOUT = 0x11,
    TFT_CMD_DISPON = 0x29,
    TFT_CMD_MADCTL = 0x36,
    TFT_CMD_COLMOD = 0x3A,
};

static void ili9488_init(const TftTransportOps *transport) {
    /*
     * ILI9488 controller-specific init.
     *
     * Inference from the ILI9488 datasheet for MCU/DBI mode:
     *   - 0x3A = 0x55 selects RGB565 (DBI=101) for parallel MCU transfers
     *   - 0x36 = 0x28 gives landscape with BGR ordering, matching the current
     *     framebuffer byte order and viewport assumptions
     *
     * If this panel still misbehaves, the next likely adjustment is MADCTL,
     * not the transport.
     */
    static const uint8_t colmod = 0x55;
    static const uint8_t madctl = 0x28; /* MV|BGR: 480x320 landscape */
    static const TftControllerInitCommand sequence[] = {
        { TFT_CMD_SWRESET, NULL, 0u, 150u },
        { TFT_CMD_SLPOUT,  NULL, 0u, 120u },
        { TFT_CMD_COLMOD, &colmod, 1u, 20u },
        { TFT_CMD_MADCTL, &madctl, 1u, 0u },
        { TFT_CMD_DISPON, NULL, 0u, 120u },
    };

    tft_controller_run_init_sequence(transport, sequence, sizeof(sequence) / sizeof(sequence[0]));
}

#if defined(MICRONES_DISPLAY_CONTROLLER_ILI9488)
static const TftController k_controller = {
    .name = "ili9488",
    .width = 480u,
    .height = 320u,
    .viewport_x = 112u,
    .viewport_y = 40u,
    .init = ili9488_init,
};

const TftController *tft_controller_get(void) {
    return &k_controller;
}
#endif
