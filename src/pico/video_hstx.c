#include "video_hstx.h"

#include "hardware/address_mapped.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "pico/time.h"

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

#define MODE_V_TOTAL_LINES ( \
    MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + \
    MODE_V_BACK_PORCH + MODE_V_ACTIVE_LINES \
)

#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

static int s_dmach_ping = -1;
static int s_dmach_pong = -1;

#define NES_SCALE 2u
#define NES_VIEW_X ((MODE_H_ACTIVE_PIXELS - (NES_FRAME_WIDTH * NES_SCALE)) / 2u)
#define FRAMEBUF_STORED_LINES NES_FRAME_HEIGHT
#define HSTX_PIXEL_CLOCK_HZ 125000000u

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
    s_framebuf[MODE_H_ACTIVE_PIXELS * FRAMEBUF_STORED_LINES];

static bool s_dma_pong;
static uint32_t s_v_scanline = 2u;
static bool s_vactive_cmdlist_posted;
static bool s_started;
static bool s_irq_configured;
static bool s_borders_dirty;
static VideoHstxStats s_stats;
static char s_last_error[64];

static const uint8_t k_nes_palette_rgb[64][3] = {
    { 0x7c, 0x7c, 0x7c }, { 0x00, 0x00, 0xfc }, { 0x00, 0x00, 0xbc }, { 0x44, 0x28, 0xbc },
    { 0x94, 0x00, 0x84 }, { 0xa8, 0x00, 0x20 }, { 0xa8, 0x10, 0x00 }, { 0x88, 0x14, 0x00 },
    { 0x50, 0x30, 0x00 }, { 0x00, 0x78, 0x00 }, { 0x00, 0x68, 0x00 }, { 0x00, 0x58, 0x00 },
    { 0x00, 0x40, 0x58 }, { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00 },

    { 0xbc, 0xbc, 0xbc }, { 0x00, 0x78, 0xf8 }, { 0x00, 0x58, 0xf8 }, { 0x68, 0x44, 0xfc },
    { 0xd8, 0x00, 0xcc }, { 0xe4, 0x00, 0x58 }, { 0xf8, 0x38, 0x00 }, { 0xe4, 0x5c, 0x10 },
    { 0xac, 0x7c, 0x00 }, { 0x00, 0xb8, 0x00 }, { 0x00, 0xa8, 0x00 }, { 0x00, 0xa8, 0x44 },
    { 0x00, 0x88, 0x88 }, { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00 },

    { 0xf8, 0xf8, 0xf8 }, { 0x3c, 0xbc, 0xfc }, { 0x68, 0x88, 0xfc }, { 0x98, 0x78, 0xf8 },
    { 0xf8, 0x78, 0xf8 }, { 0xf8, 0x58, 0x98 }, { 0xf8, 0x78, 0x58 }, { 0xfc, 0xa0, 0x44 },
    { 0xf8, 0xb8, 0x00 }, { 0xb8, 0xf8, 0x18 }, { 0x58, 0xd8, 0x54 }, { 0x58, 0xf8, 0x98 },
    { 0x00, 0xe8, 0xd8 }, { 0x78, 0x78, 0x78 }, { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00 },

    { 0xfc, 0xfc, 0xfc }, { 0xa4, 0xe4, 0xfc }, { 0xb8, 0xb8, 0xf8 }, { 0xd8, 0xb8, 0xf8 },
    { 0xf8, 0xb8, 0xf8 }, { 0xf8, 0xa4, 0xc0 }, { 0xf0, 0xd0, 0xb0 }, { 0xfc, 0xe0, 0xa8 },
    { 0xf8, 0xd8, 0x78 }, { 0xd8, 0xf8, 0x78 }, { 0xb8, 0xf8, 0xb8 }, { 0xb8, 0xf8, 0xd8 },
    { 0x00, 0xfc, 0xfc }, { 0xf8, 0xd8, 0xf8 }, { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00 },
};

static inline uint8_t rgb332(uint8_t r, uint8_t g, uint8_t b) {
    return (uint8_t)((r & 0xe0u) | ((g & 0xe0u) >> 3) | ((b & 0xc0u) >> 6));
}

static const uint8_t k_palette_rgb332[64] = {
    0x6d, 0x03, 0x02, 0x46, 0x82, 0xa0, 0xa0, 0x80,
    0x44, 0x0c, 0x0c, 0x08, 0x09, 0x00, 0x00, 0x00,
    0xb6, 0x0f, 0x0b, 0x6b, 0xc3, 0xe1, 0xe4, 0xe8,
    0xac, 0x14, 0x14, 0x15, 0x12, 0x00, 0x00, 0x00,
    0xff, 0x37, 0x73, 0x8f, 0xef, 0xea, 0xed, 0xf5,
    0xf4, 0xbc, 0x59, 0x5e, 0x1f, 0x6d, 0x00, 0x00,
    0xff, 0xbf, 0xb7, 0xd7, 0xf7, 0xf7, 0xfa, 0xfe,
    0xf9, 0xdd, 0xbe, 0xbf, 0x1f, 0xfb, 0x00, 0x00,
};

static void hstx_configure_next_scanline(dma_channel_hw_t *ch) {
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
        uint32_t active_y = s_v_scanline - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES);
        uint32_t stored_y = active_y / NES_SCALE;
        ch->read_addr = (uintptr_t)&s_framebuf[stored_y * MODE_H_ACTIVE_PIXELS];
        ch->transfer_count = MODE_H_ACTIVE_PIXELS / sizeof(uint32_t);
        s_vactive_cmdlist_posted = false;
    }

    if (!s_vactive_cmdlist_posted) {
        s_v_scanline = (s_v_scanline + 1u) % MODE_V_TOTAL_LINES;
        if (s_v_scanline == 0u) {
            ++s_stats.frames_presented;
        }
    }
}

static void __scratch_x("") hstx_dma_irq(void) {
    /* Without chaining, exactly one channel completes per ISR.  The OTHER
     * channel was pre-configured by the previous ISR and is ready to go.
     *
     * Pipeline: start the pre-configured channel immediately (minimises the
     * HSTX FIFO gap), then configure the just-completed channel for the
     * scanline AFTER the one that's now running.
     *
     * If the ISR was blocked (e.g. by printf), the DMA was simply idle — no
     * stale data replayed, no cascade. */
    uint32_t completed = s_dma_pong ? (uint32_t)s_dmach_pong : (uint32_t)s_dmach_ping;
    uint32_t next      = s_dma_pong ? (uint32_t)s_dmach_ping : (uint32_t)s_dmach_pong;

    dma_hw->intr = 1u << completed;

    dma_channel_start(next);

    s_dma_pong = !s_dma_pong;

    hstx_configure_next_scanline(&dma_hw->ch[completed]);
}

static void hstx_configure_peripheral(void) {
    const uint32_t sys_hz = clock_get_hz(clk_sys);

    clock_configure(clk_hstx, 0,
                    CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS,
                    sys_hz, HSTX_PIXEL_CLOCK_HZ);

    hstx_ctrl_hw->expand_tmds =
        2  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
        0  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
        2  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
        29 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
        1  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
        26 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    hstx_ctrl_hw->expand_shift =
        4 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        8 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    hstx_ctrl_hw->csr = 0u;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    hstx_ctrl_hw->bit[0] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    hstx_ctrl_hw->bit[1] = HSTX_CTRL_BIT0_CLK_BITS;
    for (uint32_t lane = 0u; lane < 3u; ++lane) {
        static const int lane_to_output_bit[3] = {2, 4, 6};
        uint32_t bit = (uint32_t)lane_to_output_bit[lane];
        uint32_t lane_data_sel_bits =
            (lane * 10u) << HSTX_CTRL_BIT0_SEL_P_LSB |
            (lane * 10u + 1u) << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[bit] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
        hstx_ctrl_hw->bit[bit + 1u] = lane_data_sel_bits;
    }
}

static void hstx_configure_dma(void) {
    const uint32_t mask = (1u << s_dmach_ping) | (1u << s_dmach_pong);

    /* No chaining: each channel chains to itself (disabled).  The ISR
     * explicitly starts the next channel after configuring it, so a delayed
     * ISR causes a stall instead of stale-data replay cascades. */
    dma_channel_config c = dma_channel_get_default_config((uint)s_dmach_ping);
    channel_config_set_chain_to(&c, (uint)s_dmach_ping);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure((uint)s_dmach_ping, &c,
                          &hstx_fifo_hw->fifo,
                          s_vblank_line_vsync_off,
                          count_of(s_vblank_line_vsync_off),
                          false);

    c = dma_channel_get_default_config((uint)s_dmach_pong);
    channel_config_set_chain_to(&c, (uint)s_dmach_pong);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure((uint)s_dmach_pong, &c,
                          &hstx_fifo_hw->fifo,
                          s_vblank_line_vsync_off,
                          count_of(s_vblank_line_vsync_off),
                          false);

    dma_hw->intr = mask;
    dma_hw->inte0 |= mask;
    if (!s_irq_configured) {
        irq_set_exclusive_handler(DMA_IRQ_0, hstx_dma_irq);
        s_irq_configured = true;
    }
    irq_set_priority(DMA_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_priority(USBCTRL_IRQ, 0xc0);
    irq_set_enabled(DMA_IRQ_0, true);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS |
                            BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
}

bool video_hstx_init(void) {
    snprintf(s_last_error, sizeof(s_last_error), "%s", "");
    memset(&s_stats, 0, sizeof(s_stats));
    memset(s_framebuf, 0, sizeof(s_framebuf));
    s_dma_pong = false;
    s_v_scanline = 2u;
    s_vactive_cmdlist_posted = false;
    s_started = false;
    s_irq_configured = false;
    s_borders_dirty = true;

    if (s_dmach_ping < 0) {
        s_dmach_ping = dma_claim_unused_channel(true);
    }
    if (s_dmach_pong < 0) {
        s_dmach_pong = dma_claim_unused_channel(true);
    }

    hstx_configure_peripheral();
    for (uint32_t gpio = 12u; gpio <= 19u; ++gpio) {
        gpio_set_function(gpio, GPIO_FUNC_HSTX);
    }
    hstx_configure_dma();
    return true;
}

const char *video_hstx_last_error(void) {
    return s_last_error;
}

void video_hstx_start(void) {
    if (s_started) {
        return;
    }
    s_dma_pong = false;
    s_v_scanline = 2u;
    s_vactive_cmdlist_posted = false;
    hstx_configure_peripheral();
    hstx_configure_dma();
    s_started = true;
    dma_channel_start((uint)s_dmach_ping);
}

void video_hstx_stop(void) {
    const uint32_t mask = (1u << s_dmach_ping) | (1u << s_dmach_pong);

    if (!s_started) {
        return;
    }

    irq_set_enabled(DMA_IRQ_0, false);
    dma_hw->inte0 &= ~mask;

    /* RP2350-E5: clear EN bits before aborting chained channels, otherwise
     * an abort can retrigger the partner and leave the pair half-running. */
    hw_clear_bits(&dma_hw->ch[s_dmach_ping].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_clear_bits(&dma_hw->ch[s_dmach_pong].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_channel_abort((uint)s_dmach_ping);
    dma_channel_abort((uint)s_dmach_pong);
    dma_hw->intr = mask;
    hstx_ctrl_hw->csr = 0u;

    s_dma_pong = false;
    s_v_scanline = 2u;
    s_vactive_cmdlist_posted = false;
    s_started = false;
}

void video_hstx_present_frame(const NesFrameBuffer *frame) {
    uint64_t start_us;

    if (frame == NULL) {
        return;
    }

    start_us = time_us_64();

    if (s_borders_dirty) {
        for (uint32_t y = 0u; y < NES_FRAME_HEIGHT; ++y) {
            uint8_t *dst = &s_framebuf[y * MODE_H_ACTIVE_PIXELS];
            memset(dst, 0, NES_VIEW_X);
            memset(dst + NES_VIEW_X + (NES_FRAME_WIDTH * NES_SCALE), 0,
                   MODE_H_ACTIVE_PIXELS - NES_VIEW_X - (NES_FRAME_WIDTH * NES_SCALE));
        }
        s_borders_dirty = false;
    }

    for (uint32_t y = 0u; y < NES_FRAME_HEIGHT; ++y) {
        uint8_t *dst = &s_framebuf[y * MODE_H_ACTIVE_PIXELS];
        const uint8_t *src = nes_framebuffer_scanline_const(frame, (uint16_t)y);

        uint8_t *out = dst + NES_VIEW_X;
        for (uint32_t x = 0u; x < NES_FRAME_WIDTH; ++x) {
            uint8_t c = k_palette_rgb332[src[x] & 0x3fu];
            ((uint16_t *)out)[x] = (uint16_t)c | ((uint16_t)c << 8);
        }
    }

    uint64_t elapsed = time_us_64() - start_us;
    s_stats.present_us_total += elapsed;
    if (elapsed > s_stats.present_us_max) {
        s_stats.present_us_max = elapsed;
    }
}

void video_hstx_draw_test_pattern(void) {
    static const uint8_t colors[8][3] = {
        {255, 255, 255}, {255, 255,   0}, {  0, 255, 255}, {  0, 255,   0},
        {255,   0, 255}, {255,   0,   0}, {  0,   0, 255}, {  0,   0,   0},
    };

    for (uint32_t y = 0u; y < FRAMEBUF_STORED_LINES; ++y) {
        uint32_t display_y = y * NES_SCALE;
        for (uint32_t x = 0u; x < MODE_H_ACTIVE_PIXELS; ++x) {
            uint8_t c;
            if (x == 0u || x == MODE_H_ACTIVE_PIXELS - 1u ||
                y == 0u || y == FRAMEBUF_STORED_LINES - 1u) {
                c = rgb332(255, 255, 255);
            } else if (display_y < 320u) {
                const uint8_t *rgb = colors[(x * 8u) / MODE_H_ACTIVE_PIXELS];
                c = rgb332(rgb[0], rgb[1], rgb[2]);
            } else {
                uint8_t level = ((x / 16u) ^ (display_y / 16u)) & 1u ? 255u : 0u;
                c = rgb332(level, level, level);
            }
            s_framebuf[y * MODE_H_ACTIVE_PIXELS + x] = c;
        }
    }
    s_borders_dirty = true;
}

void video_hstx_get_stats(VideoHstxStats *stats_out) {
    if (stats_out == NULL) {
        return;
    }
    *stats_out = s_stats;
    stats_out->scanline = s_v_scanline;
    stats_out->started = s_started;
}
