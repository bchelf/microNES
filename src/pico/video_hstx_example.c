/*
 * video_hstx_example.c — HDMI/HSTX bring-up path.
 *
 * This intentionally mirrors pico-examples/hstx/dvi_out_hstx_encoder for
 * test-pattern builds.  Keep it boring: if this locks but video_hstx.c does
 * not, the issue is in the custom line-buffer/DMA path, not board wiring.
 */

#include "video_hstx.h"
#include "clock_config.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/resets.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"

#include <stdio.h>
#include <string.h>

#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

#define MODE_H_FRONT_PORCH   16u
#define MODE_H_SYNC_WIDTH    96u
#define MODE_H_BACK_PORCH    48u
#define MODE_H_ACTIVE_PIXELS 640u

#define MODE_V_FRONT_PORCH   10u
#define MODE_V_SYNC_WIDTH    2u
#define MODE_V_BACK_PORCH    33u
#define MODE_V_ACTIVE_LINES  480u

#define MODE_V_TOTAL_LINES \
    (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH + MODE_V_ACTIVE_LINES)

#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

static uint32_t s_vblank_line_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V1_H1,
    HSTX_CMD_NOP
};

static uint32_t s_vblank_line_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V0_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V0_H1,
    HSTX_CMD_NOP
};

static uint32_t s_vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS
};

static uint8_t __attribute__((aligned(4)))
    s_framebuf[MODE_H_ACTIVE_PIXELS * MODE_V_ACTIVE_LINES];

#define DMACH_PING 0u
#define DMACH_PONG 1u

static bool s_dma_pong;
static uint32_t s_v_scanline = 2u;
static bool s_vactive_cmdlist_posted;
static VideoHstxStats s_stats;
static char s_last_error[32];

static const uint8_t k_nes_palette_rgb[64][3] = {
    { 84,  84,  84}, {  0,  30, 116}, {  8,  16, 144}, { 48,   0, 136},
    { 68,   0, 100}, { 92,   0,  48}, { 84,   4,   0}, { 60,  24,   0},
    { 32,  42,   0}, {  8,  58,   0}, {  0,  64,   0}, {  0,  60,   0},
    {  0,  50,  60}, {  0,   0,   0}, {  0,   0,   0}, {  0,   0,   0},
    {152, 150, 152}, {  8,  76, 196}, { 48,  50, 236}, { 92,  30, 228},
    {136,  20, 176}, {160,  20, 100}, {152,  34,  32}, {120,  60,   0},
    { 84,  90,   0}, { 40, 114,   0}, {  8, 124,   0}, {  0, 118,  40},
    {  0, 102, 120}, {  0,   0,   0}, {  0,   0,   0}, {  0,   0,   0},
    {236, 238, 236}, { 76, 154, 236}, {120, 124, 236}, {176,  98, 236},
    {228,  84, 236}, {236,  88, 180}, {236, 106, 100}, {212, 136,  32},
    {160, 170,   0}, {116, 196,   0}, { 76, 208,  32}, { 56, 204, 108},
    { 56, 180, 204}, { 60,  60,  60}, {  0,   0,   0}, {  0,   0,   0},
    {236, 238, 236}, {168, 204, 236}, {188, 188, 236}, {212, 178, 236},
    {236, 174, 236}, {236, 174, 212}, {236, 180, 176}, {228, 196, 144},
    {204, 210, 120}, {180, 222, 120}, {168, 226, 144}, {152, 226, 180},
    {160, 214, 228}, {160, 162, 160}, {  0,   0,   0}, {  0,   0,   0},
};

static inline uint8_t rgb332(uint8_t r, uint8_t g, uint8_t b) {
    return (uint8_t)(((r & 0xc0u) >> 6) |
                     ((g & 0xe0u) >> 3) |
                     ((b & 0xe0u) >> 0));
}

static void __scratch_x("") hstx_example_dma_irq(void) {
    uint32_t ch_num = s_dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;
    s_dma_pong = !s_dma_pong;

    if (s_v_scanline >= MODE_V_FRONT_PORCH &&
        s_v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH) {
        ch->read_addr = (uintptr_t)s_vblank_line_vsync_on;
        ch->transfer_count = count_of(s_vblank_line_vsync_on);
    } else if (s_v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH) {
        ch->read_addr = (uintptr_t)s_vblank_line_vsync_off;
        ch->transfer_count = count_of(s_vblank_line_vsync_off);
    } else if (!s_vactive_cmdlist_posted) {
        ch->read_addr = (uintptr_t)s_vactive_line;
        ch->transfer_count = count_of(s_vactive_line);
        s_vactive_cmdlist_posted = true;
    } else {
        uint32_t active_y = s_v_scanline -
            (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES);
        ch->read_addr = (uintptr_t)&s_framebuf[active_y * MODE_H_ACTIVE_PIXELS];
        ch->transfer_count = MODE_H_ACTIVE_PIXELS / sizeof(uint32_t);
        s_vactive_cmdlist_posted = false;
    }

    if (!s_vactive_cmdlist_posted) {
        s_v_scanline++;
        if (s_v_scanline >= MODE_V_TOTAL_LINES) {
            s_v_scanline = 0u;
            s_stats.frames_presented++;
        }
    }
}

static void hstx_example_configure_peripheral(void) {
    reset_unreset_block_num_wait_blocking(RESET_HSTX);

    const uint32_t sys_hz = MICRONES_PLL_VCO_HZ /
                            (MICRONES_PLL_DIV1 * MICRONES_PLL_DIV2);
    clock_configure(clk_hstx, 0,
                    CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS,
                    sys_hz, sys_hz / 2u);

    hstx_ctrl_hw->expand_tmds =
        (2u << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB) |
        (0u << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB) |
        (2u << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB) |
        (29u << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB) |
        (1u << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB) |
        (26u << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB);

    hstx_ctrl_hw->expand_shift =
        (4u << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB) |
        (8u << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB) |
        (1u << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB) |
        (0u << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB);

    hstx_ctrl_hw->csr = 0u;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        (5u << HSTX_CTRL_CSR_CLKDIV_LSB) |
        (5u << HSTX_CTRL_CSR_N_SHIFTS_LSB) |
        (2u << HSTX_CTRL_CSR_SHIFT_LSB) |
        HSTX_CTRL_CSR_EN_BITS;

    hstx_ctrl_hw->bit[0] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    hstx_ctrl_hw->bit[1] = HSTX_CTRL_BIT0_CLK_BITS;
    for (uint32_t lane = 0; lane < 3u; ++lane) {
        static const int lane_to_output_bit[3] = {2, 4, 6};
        uint32_t bit = (uint32_t)lane_to_output_bit[lane];
        uint32_t lane_data_sel_bits =
            (lane * 10u) << HSTX_CTRL_BIT0_SEL_P_LSB |
            (lane * 10u + 1u) << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[bit] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
        hstx_ctrl_hw->bit[bit + 1u] = lane_data_sel_bits;
    }
}

static void hstx_example_configure_dma(void) {
    dma_channel_config c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PING, &c,
                          &hstx_fifo_hw->fifo,
                          s_vblank_line_vsync_off,
                          count_of(s_vblank_line_vsync_off),
                          false);

    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PONG, &c,
                          &hstx_fifo_hw->fifo,
                          s_vblank_line_vsync_off,
                          count_of(s_vblank_line_vsync_off),
                          false);

    dma_hw->intr = (1u << DMACH_PING) | (1u << DMACH_PONG);
    dma_hw->inte0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, hstx_example_dma_irq);
    irq_set_enabled(DMA_IRQ_0, true);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS |
                            BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    dma_channel_start(DMACH_PING);
}

bool video_hstx_init(void) {
    snprintf(s_last_error, sizeof(s_last_error), "%s", "");
    memset(&s_stats, 0, sizeof(s_stats));
    memset(s_framebuf, 0, sizeof(s_framebuf));
    s_dma_pong = false;
    s_v_scanline = 2u;
    s_vactive_cmdlist_posted = false;

    printf("[hstx-example] init: sys=%lu hstx=%lu\n",
           (unsigned long)clock_get_hz(clk_sys),
           (unsigned long)clock_get_hz(clk_hstx));

    hstx_example_configure_peripheral();
    for (uint32_t gpio = 12u; gpio <= 19u; ++gpio) {
        gpio_set_function(gpio, GPIO_FUNC_HSTX);
    }
#if defined(MICRONES_PICO_VIDEO_MODE_TEST_PATTERN)
    video_hstx_draw_test_pattern();
#endif
    hstx_example_configure_dma();

    printf("[hstx-example] init complete: sys=%lu hstx=%lu csr=0x%08lx\n",
           (unsigned long)clock_get_hz(clk_sys),
           (unsigned long)clock_get_hz(clk_hstx),
           (unsigned long)hstx_ctrl_hw->csr);
    return true;
}

const char *video_hstx_last_error(void) {
    return s_last_error;
}

void video_hstx_present_frame(const NesFrameBuffer *frame) {
    if (frame == NULL) {
        return;
    }

    const uint32_t left = 64u;
    const uint32_t scaled_width = NES_FRAME_WIDTH * 2u;

    for (uint32_t y = 0; y < NES_FRAME_HEIGHT; ++y) {
        uint8_t *dst0 = &s_framebuf[(y * 2u) * MODE_H_ACTIVE_PIXELS];
        uint8_t *dst1 = dst0 + MODE_H_ACTIVE_PIXELS;
        const uint8_t *src = &frame->pixels[y * NES_FRAME_WIDTH];

        memset(dst0, 0, left);
        memset(dst1, 0, left);

        uint8_t *out0 = dst0 + left;
        uint8_t *out1 = dst1 + left;
        for (uint32_t x = 0; x < NES_FRAME_WIDTH; ++x) {
            const uint8_t *rgb = k_nes_palette_rgb[src[x] & 0x3fu];
            uint8_t c = rgb332(rgb[0], rgb[1], rgb[2]);
            out0[x * 2u + 0u] = c;
            out0[x * 2u + 1u] = c;
            out1[x * 2u + 0u] = c;
            out1[x * 2u + 1u] = c;
        }

        memset(dst0 + left + scaled_width, 0,
               MODE_H_ACTIVE_PIXELS - left - scaled_width);
        memset(dst1 + left + scaled_width, 0,
               MODE_H_ACTIVE_PIXELS - left - scaled_width);
    }
}

void video_hstx_draw_test_pattern(void) {
    static const uint8_t bars[8] = {
        0x30, 0x28, 0x2c, 0x2a, 0x24, 0x16, 0x21, 0x0f,
    };

    for (uint32_t y = 0; y < MODE_V_ACTIVE_LINES; ++y) {
        for (uint32_t x = 0; x < MODE_H_ACTIVE_PIXELS; ++x) {
            uint8_t c;
            if (x == 0 || x == MODE_H_ACTIVE_PIXELS - 1u ||
                y == 0 || y == MODE_V_ACTIVE_LINES - 1u) {
                const uint8_t *rgb = k_nes_palette_rgb[0x30];
                c = rgb332(rgb[0], rgb[1], rgb[2]);
            } else if (y < MODE_V_ACTIVE_LINES / 2u) {
                const uint8_t *rgb = k_nes_palette_rgb[bars[(x * 8u) / MODE_H_ACTIVE_PIXELS]];
                c = rgb332(rgb[0], rgb[1], rgb[2]);
            } else {
                bool white = (((x / 32u) ^ (y / 32u)) & 1u) != 0u;
                const uint8_t *rgb = k_nes_palette_rgb[white ? 0x30 : 0x0f];
                c = rgb332(rgb[0], rgb[1], rgb[2]);
            }
            s_framebuf[y * MODE_H_ACTIVE_PIXELS + x] = c;
        }
    }
}

void video_hstx_get_stats(VideoHstxStats *stats_out) {
    if (stats_out == NULL) return;
    *stats_out = s_stats;
}
