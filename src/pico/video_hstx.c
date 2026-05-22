#include "video_hstx.h"
#include "hdmi_data_island.h"

#include "hardware/address_mapped.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
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

#define HSTX_CMD_RAW         (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

static int s_dmach_ping = -1;
static int s_dmach_pong = -1;

#define NES_SCALE 2u
#define NES_VIEW_X ((MODE_H_ACTIVE_PIXELS - (NES_FRAME_WIDTH * NES_SCALE)) / 2u)
#define FRAMEBUF_STORED_LINES NES_FRAME_HEIGHT
#define HSTX_PIXEL_CLOCK_HZ 125000000u

/* --- HDMI audio scheduler tunables ------------------------------------- */

/* 8 audio sample packets per data island × 25 lines = 200 packets/frame.
 * At 4 stereo samples per ASP, that's 800 frames/video-frame which matches
 * 48 kHz / 60 Hz exactly.
 */
#define HDMI_AUDIO_PACKETS_PER_LINE 8u
#define HDMI_AUDIO_LINES            25u
#define HDMI_AUDIO_FIRST_VBI_LINE   12u   /* V_BP starts at scanline 12. */
#define HDMI_AUDIO_LAST_VBI_LINE    (HDMI_AUDIO_FIRST_VBI_LINE + HDMI_AUDIO_LINES - 1u)

#define HDMI_CONTROL_PACKETS         4u   /* AVI + Audio IF + GCP + ACR */
#define HDMI_CONTROL_VBI_LINE        (HDMI_AUDIO_LAST_VBI_LINE + 1u)

#define HDMI_AUDIO_SAMPLE_RATE_HZ    48000u
#define HDMI_AUDIO_N_VALUE           6144u   /* 48 kHz, per HDMI 1.4 §7.2.2 */
/* CTS = TMDS_clock_Hz * N / (128 * fs). For 25 MHz pixel: 25e6 * 6144 /
 * (128 * 48000) = 25000. We send pixels at the system-divided rate; the
 * HSTX_CMD_TMDS pixels are emitted at 25 MHz exactly. */
#define HDMI_AUDIO_CTS_VALUE         25000u

#define HDMI_AUDIO_ISLAND_WORDS \
    (HDMI_DI_PREAMBLE_PIXELS + (2u * HDMI_DI_GUARDBAND_PIXELS) + \
     (HDMI_AUDIO_PACKETS_PER_LINE * HDMI_PACKET_RAW_WORDS))

#define HDMI_CONTROL_ISLAND_WORDS \
    (HDMI_DI_PREAMBLE_PIXELS + (2u * HDMI_DI_GUARDBAND_PIXELS) + \
     (HDMI_CONTROL_PACKETS * HDMI_PACKET_RAW_WORDS))

/* Pixel positions: the data island starts immediately after HSYNC pulse. */
#define HDMI_DI_POST_HSYNC_FILLER_PIXELS \
    (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS - HDMI_AUDIO_ISLAND_WORDS)

#define HDMI_CONTROL_POST_FILLER_PIXELS \
    (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS - HDMI_CONTROL_ISLAND_WORDS)

/* Layout of an audio VBI line cmd buffer:
 *   [RAW_REPEAT|HFP, sync_v1_h1,
 *    RAW_REPEAT|HSYNC, sync_v1_h0,
 *    RAW|island_words, ...island...,
 *    RAW_REPEAT|post_filler, sync_v1_h1,
 *    NOP]
 */
#define HDMI_AUDIO_LINE_WORDS \
    (2u + 2u + 1u + HDMI_AUDIO_ISLAND_WORDS + 2u + 1u)

#define HDMI_CONTROL_LINE_WORDS \
    (2u + 2u + 1u + HDMI_CONTROL_ISLAND_WORDS + 2u + 1u)

/* Offset of the first island RAW word inside an audio VBI line cmd buffer. */
#define HDMI_AUDIO_ISLAND_WORDS_OFFSET 5u

/* --- Existing VBI cmd lists (DVI-compatible, no data island) ------------ */

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

/*
 * Visible scanline cmd list. In DVI mode the HBP is just sync-off filler; in
 * HDMI mode we insert the video preamble (8 px) and video guard band (2 px)
 * immediately before the TMDS data period.
 *
 * Layout:
 *   [RAW_REPEAT|HFP, sync_v1_h1,
 *    RAW_REPEAT|HSYNC, sync_v1_h0,
 *    RAW_REPEAT|(HBP-10), sync_v1_h1,
 *    RAW_REPEAT|8, video_preamble,
 *    RAW_REPEAT|2, video_guardband,
 *    TMDS|640]
 */
static uint32_t s_vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH - HDMI_VIDEO_PREAMBLE_PIXELS - HDMI_VIDEO_GUARDBAND_PIXELS),
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | HDMI_VIDEO_PREAMBLE_PIXELS,
    0u, /* patched in init to hdmi_video_preamble_word */
    HSTX_CMD_RAW_REPEAT | HDMI_VIDEO_GUARDBAND_PIXELS,
    0u, /* patched in init to hdmi_video_guardband_word */
    HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS,
};

static uint8_t __attribute__((aligned(4)))
    s_framebuf[MODE_H_ACTIVE_PIXELS * FRAMEBUF_STORED_LINES];

/* --- HDMI data island cmd buffers --------------------------------------- */

static uint32_t __attribute__((aligned(4)))
    s_hdmi_audio_line_buf[2][HDMI_AUDIO_LINES][HDMI_AUDIO_LINE_WORDS];

static uint32_t __attribute__((aligned(4)))
    s_hdmi_control_line_buf[HDMI_CONTROL_LINE_WORDS];

static volatile uint8_t s_hdmi_audio_active_buf;

/* PCM ring drained when assembling audio sample packets. Stereo frames. */
#define HDMI_PCM_RING_FRAMES 2048u  /* must be a power of two */
static int16_t s_hdmi_pcm_ring[HDMI_PCM_RING_FRAMES * 2u];
static volatile uint32_t s_hdmi_pcm_head;  /* producer index, in stereo frames */
static volatile uint32_t s_hdmi_pcm_tail;  /* consumer index */
static uint32_t s_hdmi_audio_frame_no;     /* IEC-60958 frame counter */
static volatile uint32_t s_hdmi_audio_underruns;
static volatile uint32_t s_hdmi_audio_overruns;

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

/* --- HDMI data island helpers ------------------------------------------ */

static void hdmi_init_audio_line_template(uint32_t buf[HDMI_AUDIO_LINE_WORDS]) {
    uint32_t *p = buf;
    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH;
    *p++ = SYNC_V1_H1;
    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH;
    *p++ = SYNC_V1_H0;
    *p++ = HSTX_CMD_RAW | HDMI_AUDIO_ISLAND_WORDS;
    /* Island RAW words begin here; filled per frame. */
    for (uint32_t i = 0u; i < HDMI_AUDIO_ISLAND_WORDS; ++i) {
        *p++ = 0u;
    }
    *p++ = HSTX_CMD_RAW_REPEAT | HDMI_DI_POST_HSYNC_FILLER_PIXELS;
    *p++ = SYNC_V1_H1;
    *p++ = HSTX_CMD_NOP;
}

static void hdmi_init_control_line_template(void) {
    uint32_t *p = s_hdmi_control_line_buf;
    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH;
    *p++ = SYNC_V1_H1;
    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH;
    *p++ = SYNC_V1_H0;
    *p++ = HSTX_CMD_RAW | HDMI_CONTROL_ISLAND_WORDS;
    for (uint32_t i = 0u; i < HDMI_CONTROL_ISLAND_WORDS; ++i) {
        *p++ = 0u;
    }
    *p++ = HSTX_CMD_RAW_REPEAT | HDMI_CONTROL_POST_FILLER_PIXELS;
    *p++ = SYNC_V1_H1;
    *p++ = HSTX_CMD_NOP;
}

static void hdmi_refill_control_island(void) {
    HdmiPacket packets[HDMI_CONTROL_PACKETS];
    hdmi_pkt_make_avi_infoframe(&packets[0]);
    hdmi_pkt_make_audio_infoframe(&packets[1]);
    hdmi_pkt_make_general_control(&packets[2], 0, 1);
    hdmi_pkt_make_acr(&packets[3], HDMI_AUDIO_N_VALUE, HDMI_AUDIO_CTS_VALUE);

    uint32_t *island = &s_hdmi_control_line_buf[HDMI_AUDIO_ISLAND_WORDS_OFFSET];
    hdmi_di_emit_block(packets, HDMI_CONTROL_PACKETS,
                       /* hsync_active */ 0u, /* vsync_active */ 0u, island);
}

static inline uint32_t hdmi_pcm_available_frames(void) {
    uint32_t head = s_hdmi_pcm_head;
    uint32_t tail = s_hdmi_pcm_tail;
    return (head - tail) & (HDMI_PCM_RING_FRAMES - 1u);
}

static void hdmi_pull_stereo_samples(int16_t out[8], uint32_t want_frames,
                                     uint32_t *got_frames) {
    uint32_t tail = s_hdmi_pcm_tail;
    uint32_t avail = hdmi_pcm_available_frames();
    uint32_t take = want_frames;
    if (take > avail) {
        take = avail;
    }
    for (uint32_t i = 0u; i < take; ++i) {
        uint32_t idx = (tail + i) & (HDMI_PCM_RING_FRAMES - 1u);
        out[i * 2u + 0u] = s_hdmi_pcm_ring[idx * 2u + 0u];
        out[i * 2u + 1u] = s_hdmi_pcm_ring[idx * 2u + 1u];
    }
    /* If under-running, pad with the last sample (or zero if buffer empty). */
    if (take < want_frames) {
        int16_t pad_l = take ? out[(take - 1u) * 2u + 0u] : 0;
        int16_t pad_r = take ? out[(take - 1u) * 2u + 1u] : 0;
        for (uint32_t i = take; i < want_frames; ++i) {
            out[i * 2u + 0u] = pad_l;
            out[i * 2u + 1u] = pad_r;
        }
        ++s_hdmi_audio_underruns;
    }
    s_hdmi_pcm_tail = (tail + take) & (HDMI_PCM_RING_FRAMES - 1u);
    *got_frames = take;
}

static void hdmi_refill_audio_islands(uint32_t buf_idx) {
    HdmiPacket packets[HDMI_AUDIO_PACKETS_PER_LINE];
    for (uint32_t line = 0u; line < HDMI_AUDIO_LINES; ++line) {
        for (uint32_t p = 0u; p < HDMI_AUDIO_PACKETS_PER_LINE; ++p) {
            int16_t samples[8];
            uint32_t got;
            hdmi_pull_stereo_samples(samples, 4u, &got);
            hdmi_pkt_make_audio_sample(&packets[p], samples,
                                       got ? got : 4u, &s_hdmi_audio_frame_no);
        }
        uint32_t *island =
            &s_hdmi_audio_line_buf[buf_idx][line][HDMI_AUDIO_ISLAND_WORDS_OFFSET];
        hdmi_di_emit_block(packets, HDMI_AUDIO_PACKETS_PER_LINE,
                           /* hsync_active */ 0u, /* vsync_active */ 0u, island);
    }
}

/* --- DMA / ISR --------------------------------------------------------- */

static void __scratch_x("") hstx_dma_irq(void) {
    uint32_t ch_num = s_dma_pong ? (uint32_t)s_dmach_pong : (uint32_t)s_dmach_ping;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;
    s_dma_pong = !s_dma_pong;

    uint8_t audio_buf = s_hdmi_audio_active_buf;

    if (s_v_scanline >= MODE_V_FRONT_PORCH &&
        s_v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH) {
        ch->read_addr = (uintptr_t)s_vblank_line_vsync_on;
        ch->transfer_count = count_of(s_vblank_line_vsync_on);
    } else if (s_v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH) {
        if (s_v_scanline >= HDMI_AUDIO_FIRST_VBI_LINE &&
            s_v_scanline <= HDMI_AUDIO_LAST_VBI_LINE) {
            uint32_t line_idx = s_v_scanline - HDMI_AUDIO_FIRST_VBI_LINE;
            ch->read_addr = (uintptr_t)&s_hdmi_audio_line_buf[audio_buf][line_idx][0];
            ch->transfer_count = HDMI_AUDIO_LINE_WORDS;
        } else if (s_v_scanline == HDMI_CONTROL_VBI_LINE) {
            ch->read_addr = (uintptr_t)s_hdmi_control_line_buf;
            ch->transfer_count = HDMI_CONTROL_LINE_WORDS;
        } else {
            ch->read_addr = (uintptr_t)s_vblank_line_vsync_off;
            ch->transfer_count = count_of(s_vblank_line_vsync_off);
        }
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
    dma_channel_config c = dma_channel_get_default_config((uint)s_dmach_ping);
    channel_config_set_chain_to(&c, (uint)s_dmach_pong);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure((uint)s_dmach_ping, &c,
                          &hstx_fifo_hw->fifo,
                          s_vblank_line_vsync_off,
                          count_of(s_vblank_line_vsync_off),
                          false);

    c = dma_channel_get_default_config((uint)s_dmach_pong);
    channel_config_set_chain_to(&c, (uint)s_dmach_ping);
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

    /* Patch the per-pixel HDMI preamble/guard-band words into the active-line
     * cmd list. We use array indices rather than struct offsets to keep this
     * narrow and obvious. */
    s_vactive_line[7] = hdmi_video_preamble_word;
    s_vactive_line[9] = hdmi_video_guardband_word;

    /* Build VBI line templates and pre-populate islands with NULL packets so
     * the channel is well-formed even before audio samples arrive. */
    for (uint32_t b = 0u; b < 2u; ++b) {
        for (uint32_t line = 0u; line < HDMI_AUDIO_LINES; ++line) {
            hdmi_init_audio_line_template(&s_hdmi_audio_line_buf[b][line][0]);
        }
    }
    hdmi_init_control_line_template();
    s_hdmi_audio_active_buf = 0u;
    s_hdmi_audio_frame_no = 0u;
    s_hdmi_pcm_head = 0u;
    s_hdmi_pcm_tail = 0u;
    s_hdmi_audio_underruns = 0u;
    s_hdmi_audio_overruns = 0u;

    hdmi_refill_control_island();

    /* Fill both audio buffers with NULL packets so the receiver sees a
     * coherent data-island stream from the first frame. */
    HdmiPacket null_pkts[HDMI_AUDIO_PACKETS_PER_LINE];
    for (uint32_t i = 0u; i < HDMI_AUDIO_PACKETS_PER_LINE; ++i) {
        hdmi_pkt_make_null(&null_pkts[i]);
    }
    for (uint32_t b = 0u; b < 2u; ++b) {
        for (uint32_t line = 0u; line < HDMI_AUDIO_LINES; ++line) {
            uint32_t *island =
                &s_hdmi_audio_line_buf[b][line][HDMI_AUDIO_ISLAND_WORDS_OFFSET];
            hdmi_di_emit_block(null_pkts, HDMI_AUDIO_PACKETS_PER_LINE,
                               0u, 0u, island);
        }
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

    /* Refill the inactive audio data-island buffer and atomically swap.
     * This happens once per NES frame, which is close to once per display
     * frame; the receiver tolerates the small jitter via its audio FIFO. */
    uint8_t back = s_hdmi_audio_active_buf ^ 1u;
    hdmi_refill_audio_islands(back);
    s_hdmi_audio_active_buf = back;

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

/* --- HDMI audio backend entry points ----------------------------------- */

void video_hstx_hdmi_audio_init(uint32_t sample_rate) {
    (void)sample_rate;
    /* The data-island scheduler is fixed for 48 kHz; we ignore the requested
     * rate and rely on the audio backend wrapper to declare 48 kHz. */
    s_hdmi_pcm_head = 0u;
    s_hdmi_pcm_tail = 0u;
    s_hdmi_audio_frame_no = 0u;
    s_hdmi_audio_underruns = 0u;
    s_hdmi_audio_overruns = 0u;
}

size_t video_hstx_hdmi_audio_push(const int16_t *interleaved_stereo,
                                  size_t nframes) {
    if (interleaved_stereo == NULL || nframes == 0u) {
        return 0u;
    }
    size_t written = 0u;
    for (size_t i = 0u; i < nframes; ++i) {
        uint32_t head = s_hdmi_pcm_head;
        uint32_t next = (head + 1u) & (HDMI_PCM_RING_FRAMES - 1u);
        if (next == s_hdmi_pcm_tail) {
            /* Ring full: evict the oldest sample, matching the MAX98357
             * backend's circular-overwrite policy so audio keeps flowing. */
            uint32_t save = save_and_disable_interrupts();
            s_hdmi_pcm_tail = (s_hdmi_pcm_tail + 1u) & (HDMI_PCM_RING_FRAMES - 1u);
            restore_interrupts(save);
            ++s_hdmi_audio_overruns;
        }
        s_hdmi_pcm_ring[head * 2u + 0u] = interleaved_stereo[i * 2u + 0u];
        s_hdmi_pcm_ring[head * 2u + 1u] = interleaved_stereo[i * 2u + 1u];
        s_hdmi_pcm_head = next;
        ++written;
    }
    return written;
}

uint32_t video_hstx_hdmi_audio_underruns(void) {
    return s_hdmi_audio_underruns;
}

uint32_t video_hstx_hdmi_audio_overruns(void) {
    return s_hdmi_audio_overruns;
}

uint32_t video_hstx_hdmi_audio_buffer_level(void) {
    return hdmi_pcm_available_frames();
}
