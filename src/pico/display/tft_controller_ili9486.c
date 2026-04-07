#include "tft_controller.h"

enum {
    TFT_CMD_SWRESET = 0x01,
    TFT_CMD_SLPOUT = 0x11,
    TFT_CMD_DISPON = 0x29,
    TFT_CMD_MADCTL = 0x36,
    TFT_CMD_COLMOD = 0x3A,
};

static void ili9486_init(const TftTransportOps *transport) {
    /*
     * ILI9486 init for common 480x320 Arduino-shield parallel panels.
     *
     * Assumptions:
     *   - 8-bit 8080-I parallel interface
     *   - 16-bit/pixel RGB565 (COLMOD 0x55)
     *   - landscape orientation, BGR pixel order (MADCTL MV|BGR = 0x28)
     *
     * Power control and VCOM values are conservative defaults from the ILI9486
     * datasheet.  Interface Mode Control (0xB0) forces the 8080 parallel bus and
     * disables DE mode, which is needed on many shield boards.  Display Function
     * Control (0xB6) sets the standard scan direction for a 480x320 panel.
     *
     * If purple/color tint persists after bus fixes, toggle MADCTL BGR (bit 3):
     *   0x28 = MV|BGR (landscape, BGR order) — default
     *   0x20 = MV only (landscape, RGB order) — try if colors are red/blue swapped
     */
    static const uint8_t ifmode[]   = {0x00};        /* 0xB0: 8080 parallel, no DE */
    static const uint8_t frmctrl[]  = {0xB0, 0x11};  /* 0xB1: 60 Hz frame rate */
    static const uint8_t dispfunc[] = {0x00, 0x42};  /* 0xB6: display function control */
    static const uint8_t pwctrl1[]  = {0x17, 0x15};  /* 0xC0: GVDD=4.40V, VCI1=2.5V */
    static const uint8_t pwctrl2[]  = {0x41};        /* 0xC1: step-up factor */
    static const uint8_t vmctrl[]   = {0x00, 0x12, 0x80}; /* 0xC5: VCOM control */
    static const uint8_t colmod     = 0x55;          /* 0x3A: 16-bit RGB565 */
    static const uint8_t madctl     = 0xA8;          /* 0x36: MY|MV|BGR landscape, mirrored Y */

    static const TftControllerInitCommand sequence[] = {
        { TFT_CMD_SWRESET, NULL,      0u,                        150u },
        { TFT_CMD_SLPOUT,  NULL,      0u,                        120u },
        { 0xB0, ifmode,   sizeof(ifmode),                          0u },
        { 0xB1, frmctrl,  sizeof(frmctrl),                         0u },
        { 0xB6, dispfunc, sizeof(dispfunc),                        0u },
        { 0xC0, pwctrl1,  sizeof(pwctrl1),                         0u },
        { 0xC1, pwctrl2,  sizeof(pwctrl2),                         0u },
        { 0xC5, vmctrl,   sizeof(vmctrl),                          0u },
        { TFT_CMD_COLMOD, &colmod, 1u,                             0u },
        { TFT_CMD_MADCTL, &madctl, 1u,                             0u },
        { TFT_CMD_DISPON, NULL,    0u,                             0u },
    };

    tft_controller_run_init_sequence(transport, sequence, sizeof(sequence) / sizeof(sequence[0]));
}

#if defined(MICRONES_DISPLAY_CONTROLLER_ILI9486)
static const TftController k_controller = {
    .name = "ili9486",
    .width = 480u,
    .height = 320u,
    .viewport_x = 112u,
    .viewport_y = 40u,
    .init = ili9486_init,
};

const TftController *tft_controller_get(void) {
    return &k_controller;
}
#endif
