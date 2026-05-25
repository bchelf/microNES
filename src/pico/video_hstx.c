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
#include "pico/multicore.h"
#include "pico/time.h"

#include <math.h>
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
#define MODE_H_TOTAL_PIXELS ( \
    MODE_H_FRONT_PORCH + MODE_H_SYNC_WIDTH + \
    MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS \
)

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

#define HSTX_INTERNAL_CLKDIV 5u
#define HDMI_HSTX_CLOCK_HZ   157500000u
#define HDMI_PIXEL_CLOCK_HZ  (HDMI_HSTX_CLOCK_HZ / HSTX_INTERNAL_CLKDIV)

/* --- HDMI audio scheduler tunables ------------------------------------- */

/* Match pico_hdmi's 480p layout: one data-island packet per scanline,
 * placed inside the 96-pixel HSYNC pulse. The packet scheduler is driven by
 * pixel clock, so the audio rate remains 48 kHz even at our 25.000 MHz mode.
 */
#define HDMI_AUDIO_QUEUE_SIZE        256u
#define HDMI_SILENCE_PACKET_SLOTS   48u  /* 48 packets × 4 samples = 192 frames */
#define HDMI_TEST_TONE_PACKET_SLOTS  300u /* 1200 samples = 11 cycles at 440 Hz */
#define HDMI_CONTROL_PACKET_LINES    4u   /* AVI + Audio IF + GCP + ACR */
#define HDMI_CONTROL_VBI_LINE        12u  /* first V_BP line after VSYNC */

#define HDMI_AUDIO_SAMPLE_RATE_HZ    48000u
#define HDMI_AUDIO_N_VALUE           6144u   /* 48 kHz, per HDMI 1.4 §7.2.2 */
#define HDMI_AUDIO_CTS_VALUE         31500u  /* for 31.5 MHz pixel clock */

#define HDMI_ONE_PACKET_ISLAND_WORDS \
    ((2u * HDMI_DI_GUARDBAND_PIXELS) + HDMI_PACKET_RAW_WORDS)
#define HDMI_SYNC_AFTER_DI_PIXELS \
    (MODE_H_SYNC_WIDTH - HDMI_DI_PREAMBLE_PIXELS - HDMI_ONE_PACKET_ISLAND_WORDS)
#define HDMI_AUDIO_SAMPLES_PER_LINE_FP \
    ((uint32_t)((((uint64_t)HDMI_AUDIO_SAMPLE_RATE_HZ * MODE_H_TOTAL_PIXELS) << 16u) / HDMI_PIXEL_CLOCK_HZ))

#define HDMI_VBLANK_DI_LINE_WORDS \
    (2u + 1u + 2u + 1u + 1u + HDMI_ONE_PACKET_ISLAND_WORDS + 1u + 2u + 1u + 2u + 1u)

#define HDMI_ACTIVE_DI_LINE_WORDS \
    (2u + 1u + 2u + 1u + 1u + HDMI_ONE_PACKET_ISLAND_WORDS + 1u + 2u + 1u + \
     2u + 1u + 2u + 1u + 2u + 1u)

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
 * Visible scanline cmd list. Two compile-time switches bisect HDMI bring-up:
 *   MICRONES_HDMI_VIDEO_PREAMBLE  (default 1)
 *   MICRONES_HDMI_DATA_ISLANDS    (default 1)
 *   MICRONES_HDMI_AUDIO_PACKETS   (default 1)
 */
#ifndef MICRONES_HDMI_STDIO_USB_ENABLED
#define MICRONES_HDMI_STDIO_USB_ENABLED 0
#endif

#ifndef MICRONES_HDMI_VIDEO_PREAMBLE
#define MICRONES_HDMI_VIDEO_PREAMBLE 1
#endif
#ifndef MICRONES_HDMI_DATA_ISLANDS
#define MICRONES_HDMI_DATA_ISLANDS 1
#endif
/* Toggle for the per-VBI-line PCM audio sample islands. When OFF, the
 * control island (AVI InfoFrame + Audio InfoFrame + GCP + ACR) is still
 * emitted on its scanline, but no PCM audio reaches the sink. Useful to
 * isolate "control packets ok / audio packets bad" failures. */
#ifndef MICRONES_HDMI_AUDIO_PACKETS
#define MICRONES_HDMI_AUDIO_PACKETS 1
#endif
#ifndef MICRONES_HDMI_TEST_TONE
#define MICRONES_HDMI_TEST_TONE 0
#endif
#ifndef MICRONES_HDMI_AUDIO_CORE1
#define MICRONES_HDMI_AUDIO_CORE1 1
#endif

#if MICRONES_HDMI_VIDEO_PREAMBLE
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
#else
static uint32_t s_vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_NOP,
    HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS,
};
#endif

static uint8_t __attribute__((aligned(4)))
    s_framebuf[MODE_H_ACTIVE_PIXELS * FRAMEBUF_STORED_LINES];

static volatile uint32_t s_hdmi_scanline_stalls;
static volatile uint32_t s_hdmi_scanline_submitted;
static volatile uint32_t s_hdmi_scanline_expanded;
static volatile uint32_t s_hdmi_scanline_max_level;
static volatile uint32_t s_hdmi_frame_submitted;
static volatile uint32_t s_hdmi_frame_expanded;

/* --- HDMI data island cmd buffers --------------------------------------- */

#if MICRONES_HDMI_DATA_ISLANDS

static uint32_t __attribute__((aligned(4)))
    s_hdmi_audio_queue[HDMI_AUDIO_QUEUE_SIZE][HDMI_ONE_PACKET_ISLAND_WORDS];

static uint32_t __attribute__((aligned(4)))
    s_hdmi_null_island[HDMI_ONE_PACKET_ISLAND_WORDS];

static uint32_t __attribute__((aligned(4)))
    s_hdmi_silence_islands[HDMI_SILENCE_PACKET_SLOTS][HDMI_ONE_PACKET_ISLAND_WORDS];

#if MICRONES_HDMI_TEST_TONE
static uint32_t __attribute__((aligned(4)))
    s_hdmi_test_tone_islands[HDMI_TEST_TONE_PACKET_SLOTS][HDMI_ONE_PACKET_ISLAND_WORDS];
#endif

static uint32_t __attribute__((aligned(4)))
    s_hdmi_control_line_buf[HDMI_CONTROL_PACKET_LINES][HDMI_VBLANK_DI_LINE_WORDS];

static uint32_t __attribute__((aligned(4)))
    s_hdmi_vblank_di_line_buf[2][HDMI_VBLANK_DI_LINE_WORDS];

static uint32_t __attribute__((aligned(4)))
    s_hdmi_active_di_line_buf[2][HDMI_ACTIVE_DI_LINE_WORDS];

static volatile uint32_t s_hdmi_audio_refills;
static volatile uint32_t s_hdmi_audio_missed_swaps;
static volatile uint32_t s_hdmi_audio_queue_head;
static volatile uint32_t s_hdmi_audio_queue_tail;
static uint32_t s_hdmi_audio_packet_cursor;
static uint32_t s_hdmi_silence_packet_cursor;
#if MICRONES_HDMI_TEST_TONE
static uint32_t s_hdmi_test_tone_packet_cursor;
#endif
static uint32_t s_hdmi_audio_sample_accum_fp;

/* PCM ring drained when assembling audio sample packets. Stereo frames. */
#define HDMI_PCM_RING_FRAMES 2048u  /* must be a power of two */
static int16_t s_hdmi_pcm_ring[HDMI_PCM_RING_FRAMES * 2u];
static volatile uint32_t s_hdmi_pcm_head;  /* producer index, in stereo frames */
static volatile uint32_t s_hdmi_pcm_tail;  /* consumer index */
static uint32_t s_hdmi_audio_frame_no;     /* IEC-60958 frame counter */
#if MICRONES_HDMI_TEST_TONE
static uint32_t s_hdmi_test_tone_phase;
#endif
static volatile uint32_t s_hdmi_audio_underruns;
static volatile uint32_t s_hdmi_audio_overruns;
static spin_lock_t *s_hdmi_pcm_lock;
static volatile bool s_hdmi_audio_worker_running;

#endif /* MICRONES_HDMI_DATA_ISLANDS */

static bool s_dma_pong;
static uint32_t s_v_scanline = 2u;
static bool s_vactive_cmdlist_posted;
static bool s_started;
static bool s_irq_configured;

static uint32_t s_diag_sys_hz;
static uint32_t s_diag_hstx_hz;
static uint32_t s_diag_pixel_hz;
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

static uint8_t k_palette_rgb332[64];
static uint16_t k_palette_dup_rgb332[64];

static void init_palette_rgb332(void) {
    for (uint32_t i = 0u; i < 64u; ++i) {
        k_palette_rgb332[i] = rgb332(k_nes_palette_rgb[i][0],
                                     k_nes_palette_rgb[i][1],
                                     k_nes_palette_rgb[i][2]);
        k_palette_dup_rgb332[i] = (uint16_t)k_palette_rgb332[i] |
                                  ((uint16_t)k_palette_rgb332[i] << 8u);
    }
}

#define MICRONES_DMB() __asm volatile ("dmb ish" ::: "memory")
#define MICRONES_SEV() __asm volatile ("sev" ::: "memory")

static void hdmi_expand_scanline_to_framebuf(const uint8_t *src, uint16_t y) {
    if (src == NULL || y >= NES_FRAME_HEIGHT) {
        return;
    }

    uint8_t *dst = &s_framebuf[(uint32_t)y * MODE_H_ACTIVE_PIXELS];
    if (s_borders_dirty) {
        memset(dst, 0, NES_VIEW_X);
        memset(dst + NES_VIEW_X + (NES_FRAME_WIDTH * NES_SCALE), 0,
               MODE_H_ACTIVE_PIXELS - NES_VIEW_X - (NES_FRAME_WIDTH * NES_SCALE));
    }

    uint8_t *out = dst + NES_VIEW_X;
    uint32_t *out32 = (uint32_t *)(void *)out;
    for (uint32_t x = 0u; x < NES_FRAME_WIDTH; x += 2u) {
        *out32++ = (uint32_t)k_palette_dup_rgb332[src[x] & 0x3fu] |
                   ((uint32_t)k_palette_dup_rgb332[src[x + 1u] & 0x3fu] << 16u);
    }

    if (y == NES_FRAME_HEIGHT - 1u) {
        s_borders_dirty = false;
    }
    ++s_hdmi_scanline_expanded;
}

static inline uint32_t hdmi_scanline_queue_level(void) {
    return 0u;
}

/* --- HDMI data island helpers ------------------------------------------ */

#if MICRONES_HDMI_DATA_ISLANDS

static const uint32_t k_hdmi_di_preamble_v1_h0 =
    TMDS_CTRL_10 | ((uint32_t)TMDS_CTRL_01 << 10) |
    ((uint32_t)TMDS_CTRL_01 << 20);

static void hdmi_build_vblank_di_line(uint32_t *buf,
                                      const uint32_t island[HDMI_ONE_PACKET_ISLAND_WORDS]) {
    uint32_t *p = buf;
    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH;
    *p++ = SYNC_V1_H1;
    *p++ = HSTX_CMD_NOP;
    *p++ = HSTX_CMD_RAW_REPEAT | HDMI_DI_PREAMBLE_PIXELS;
    *p++ = k_hdmi_di_preamble_v1_h0;
    *p++ = HSTX_CMD_NOP;
    *p++ = HSTX_CMD_RAW | HDMI_ONE_PACKET_ISLAND_WORDS;
    memcpy(p, island, HDMI_ONE_PACKET_ISLAND_WORDS * sizeof(*p));
    p += HDMI_ONE_PACKET_ISLAND_WORDS;
    *p++ = HSTX_CMD_NOP;
    *p++ = HSTX_CMD_RAW_REPEAT | HDMI_SYNC_AFTER_DI_PIXELS;
    *p++ = SYNC_V1_H0;
    *p++ = HSTX_CMD_NOP;
    *p++ = HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS);
    *p++ = SYNC_V1_H1;
    *p++ = HSTX_CMD_NOP;
}

static void hdmi_build_active_di_line(uint32_t *buf,
                                      const uint32_t island[HDMI_ONE_PACKET_ISLAND_WORDS]) {
    uint32_t *p = buf;
    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH;
    *p++ = SYNC_V1_H1;
    *p++ = HSTX_CMD_NOP;
    *p++ = HSTX_CMD_RAW_REPEAT | HDMI_DI_PREAMBLE_PIXELS;
    *p++ = k_hdmi_di_preamble_v1_h0;
    *p++ = HSTX_CMD_NOP;
    *p++ = HSTX_CMD_RAW | HDMI_ONE_PACKET_ISLAND_WORDS;
    memcpy(p, island, HDMI_ONE_PACKET_ISLAND_WORDS * sizeof(*p));
    p += HDMI_ONE_PACKET_ISLAND_WORDS;
    *p++ = HSTX_CMD_NOP;
    *p++ = HSTX_CMD_RAW_REPEAT | HDMI_SYNC_AFTER_DI_PIXELS;
    *p++ = SYNC_V1_H0;
    *p++ = HSTX_CMD_NOP;
    *p++ = HSTX_CMD_RAW_REPEAT |
           (MODE_H_BACK_PORCH - HDMI_VIDEO_PREAMBLE_PIXELS - HDMI_VIDEO_GUARDBAND_PIXELS);
    *p++ = SYNC_V1_H1;
    *p++ = HSTX_CMD_NOP;
    *p++ = HSTX_CMD_RAW_REPEAT | HDMI_VIDEO_PREAMBLE_PIXELS;
    *p++ = hdmi_video_preamble_word;
    *p++ = HSTX_CMD_NOP;
    *p++ = HSTX_CMD_RAW_REPEAT | HDMI_VIDEO_GUARDBAND_PIXELS;
    *p++ = hdmi_video_guardband_word;
    *p++ = HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS;
}

static void hdmi_refill_control_islands(void) {
    HdmiPacket packet;
    uint32_t island[HDMI_ONE_PACKET_ISLAND_WORDS];

    hdmi_pkt_make_avi_infoframe(&packet);
    hdmi_di_emit_island(&packet, 1u, 1u, 0u, island);
    hdmi_build_vblank_di_line(s_hdmi_control_line_buf[0], island);

    hdmi_pkt_make_audio_infoframe(&packet);
    hdmi_di_emit_island(&packet, 1u, 1u, 0u, island);
    hdmi_build_vblank_di_line(s_hdmi_control_line_buf[1], island);

    hdmi_pkt_make_general_control(&packet, 0, 1);
    hdmi_di_emit_island(&packet, 1u, 1u, 0u, island);
    hdmi_build_vblank_di_line(s_hdmi_control_line_buf[2], island);

    hdmi_pkt_make_acr(&packet, HDMI_AUDIO_N_VALUE, HDMI_AUDIO_CTS_VALUE);
    hdmi_di_emit_island(&packet, 1u, 1u, 0u, island);
    hdmi_build_vblank_di_line(s_hdmi_control_line_buf[3], island);
}

static inline uint32_t hdmi_pcm_available_frames(void) {
    uint32_t head = s_hdmi_pcm_head;
    uint32_t tail = s_hdmi_pcm_tail;
    return (head - tail) & (HDMI_PCM_RING_FRAMES - 1u);
}

static inline uint32_t hdmi_audio_queue_level(void) {
    uint32_t head = s_hdmi_audio_queue_head;
    uint32_t tail = s_hdmi_audio_queue_tail;
    return (head >= tail) ? (head - tail) : (HDMI_AUDIO_QUEUE_SIZE + head - tail);
}

static inline uint32_t hdmi_audio_queue_next(uint32_t index) {
    ++index;
    return (index >= HDMI_AUDIO_QUEUE_SIZE) ? 0u : index;
}

static void hdmi_pull_stereo_samples(int16_t out[8], uint32_t want_frames,
                                     uint32_t *got_frames) {
    uint32_t save = spin_lock_blocking(s_hdmi_pcm_lock);
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
    spin_unlock(s_hdmi_pcm_lock, save);
    *got_frames = take;
}

#if MICRONES_HDMI_TEST_TONE
static void hdmi_refill_test_tone_islands(void) {
    uint32_t saved_frame_no = s_hdmi_audio_frame_no;

    s_hdmi_audio_frame_no = 0u;
    for (uint32_t slot = 0u; slot < HDMI_TEST_TONE_PACKET_SLOTS; ++slot) {
        HdmiPacket packet;
        int16_t samples[8];
        for (uint32_t i = 0u; i < 4u; ++i) {
            uint32_t sample_idx = (slot * 4u) + i;
            float angle = ((float)sample_idx * 11.0f * 2.0f * 3.14159265358979323846f) / 1200.0f;
            int16_t s = (int16_t)(sinf(angle) * 10000.0f);
            samples[i * 2u + 0u] = s;
            samples[i * 2u + 1u] = s;
        }
        hdmi_pkt_make_audio_sample(&packet, samples, 4u, &s_hdmi_audio_frame_no);
        hdmi_di_emit_island(&packet, 1u, 1u, 0u, s_hdmi_test_tone_islands[slot]);
    }
    s_hdmi_audio_frame_no = saved_frame_no;
}
#endif

static void hdmi_build_silence_islands(void) {
    int16_t samples[8] = {0};
    uint32_t saved_frame_no = s_hdmi_audio_frame_no;

    s_hdmi_audio_frame_no = 0u;
    for (uint32_t slot = 0u; slot < HDMI_SILENCE_PACKET_SLOTS; ++slot) {
        HdmiPacket packet;
        hdmi_pkt_make_audio_sample(&packet, samples, 4u, &s_hdmi_audio_frame_no);
        hdmi_di_emit_island(&packet, 1u, 1u, 0u, s_hdmi_silence_islands[slot]);
    }
    s_hdmi_audio_frame_no = saved_frame_no;
}

static inline void hdmi_audio_scheduler_tick(void) {
    s_hdmi_audio_sample_accum_fp += HDMI_AUDIO_SAMPLES_PER_LINE_FP;
}

static const uint32_t *hdmi_next_audio_island(void) {
    if (s_hdmi_audio_sample_accum_fp < (4u << 16u)) {
        return s_hdmi_null_island;
    }
    s_hdmi_audio_sample_accum_fp -= (4u << 16u);

    uint32_t cursor = s_hdmi_audio_packet_cursor++;
#if MICRONES_HDMI_TEST_TONE
    (void)cursor;
    uint32_t tone_cursor = s_hdmi_test_tone_packet_cursor++;
    if (s_hdmi_test_tone_packet_cursor >= HDMI_TEST_TONE_PACKET_SLOTS) {
        s_hdmi_test_tone_packet_cursor = 0u;
    }
    return s_hdmi_test_tone_islands[tone_cursor];
#else
    (void)cursor;
    uint32_t tail = s_hdmi_audio_queue_tail;
    if (tail != s_hdmi_audio_queue_head) {
        const uint32_t *island = s_hdmi_audio_queue[tail];
        s_hdmi_audio_queue_tail = hdmi_audio_queue_next(tail);
        return island;
    }
    ++s_hdmi_audio_missed_swaps;
    uint32_t silence_cursor = s_hdmi_silence_packet_cursor++;
    if (s_hdmi_silence_packet_cursor >= HDMI_SILENCE_PACKET_SLOTS) {
        s_hdmi_silence_packet_cursor = 0u;
    }
    return s_hdmi_silence_islands[silence_cursor];
#endif
}

static bool hdmi_audio_encode_one_packet(void) {
#if MICRONES_HDMI_TEST_TONE
    return false;
#else
    if (hdmi_audio_queue_level() >= (HDMI_AUDIO_QUEUE_SIZE - 16u) ||
        hdmi_pcm_available_frames() < 4u) {
        return false;
    }

    uint32_t head = s_hdmi_audio_queue_head;
    uint32_t next = hdmi_audio_queue_next(head);
    if (next == s_hdmi_audio_queue_tail) {
        return false;
    }

    HdmiPacket packet;
    int16_t samples[8];
    uint32_t got;
    hdmi_pull_stereo_samples(samples, 4u, &got);
    (void)got;

    hdmi_pkt_make_audio_sample(&packet, samples, 4u, &s_hdmi_audio_frame_no);
    hdmi_di_emit_island(&packet, 1u, 1u, 0u, s_hdmi_audio_queue[head]);
    s_hdmi_audio_queue_head = next;
    ++s_hdmi_audio_refills;
    return true;
#endif
}

static void __not_in_flash_func(video_hstx_audio_core1_entry)(void) {
    multicore_lockout_victim_init();
    while (s_hdmi_audio_worker_running) {
        if (hdmi_audio_encode_one_packet()) {
            continue;
        }
        tight_loop_contents();
    }
}

#endif /* MICRONES_HDMI_DATA_ISLANDS */

/* --- DMA / ISR --------------------------------------------------------- */

static void __scratch_x("") hstx_dma_irq(void) {
    uint32_t ch_num = s_dma_pong ? (uint32_t)s_dmach_pong : (uint32_t)s_dmach_ping;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;
    s_dma_pong = !s_dma_pong;
    uint32_t cmd_buf_idx = (ch_num == (uint32_t)s_dmach_ping) ? 0u : 1u;

#if MICRONES_HDMI_DATA_ISLANDS && MICRONES_HDMI_AUDIO_PACKETS
    if (!s_vactive_cmdlist_posted) {
        hdmi_audio_scheduler_tick();
    }
#endif

    if (s_v_scanline >= MODE_V_FRONT_PORCH &&
        s_v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH) {
        ch->read_addr = (uintptr_t)s_vblank_line_vsync_on;
        ch->transfer_count = count_of(s_vblank_line_vsync_on);
    } else if (s_v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH) {
#if MICRONES_HDMI_DATA_ISLANDS
        if (s_v_scanline >= HDMI_CONTROL_VBI_LINE &&
            s_v_scanline < HDMI_CONTROL_VBI_LINE + HDMI_CONTROL_PACKET_LINES) {
            uint32_t line_idx = s_v_scanline - HDMI_CONTROL_VBI_LINE;
            ch->read_addr = (uintptr_t)s_hdmi_control_line_buf[line_idx];
            ch->transfer_count = HDMI_VBLANK_DI_LINE_WORDS;
        } else {
#if MICRONES_HDMI_AUDIO_PACKETS
            const uint32_t *island = hdmi_next_audio_island();
            hdmi_build_vblank_di_line(s_hdmi_vblank_di_line_buf[cmd_buf_idx], island);
            ch->read_addr = (uintptr_t)s_hdmi_vblank_di_line_buf[cmd_buf_idx];
            ch->transfer_count = HDMI_VBLANK_DI_LINE_WORDS;
#else
            ch->read_addr = (uintptr_t)s_vblank_line_vsync_off;
            ch->transfer_count = count_of(s_vblank_line_vsync_off);
#endif
        }
#else
        ch->read_addr = (uintptr_t)s_vblank_line_vsync_off;
        ch->transfer_count = count_of(s_vblank_line_vsync_off);
#endif
    } else if (!s_vactive_cmdlist_posted) {
#if MICRONES_HDMI_DATA_ISLANDS && MICRONES_HDMI_AUDIO_PACKETS
        const uint32_t *island = hdmi_next_audio_island();
        hdmi_build_active_di_line(s_hdmi_active_di_line_buf[cmd_buf_idx], island);
        ch->read_addr = (uintptr_t)s_hdmi_active_di_line_buf[cmd_buf_idx];
        ch->transfer_count = HDMI_ACTIVE_DI_LINE_WORDS;
#else
        ch->read_addr = (uintptr_t)s_vactive_line;
        ch->transfer_count = count_of(s_vactive_line);
#endif
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
#if MICRONES_HDMI_DATA_ISLANDS && MICRONES_HDMI_AUDIO_PACKETS
            s_hdmi_audio_packet_cursor = 0u;
#endif
        }
    }
}

static void hstx_configure_peripheral(void) {
    /* Run HSTX from clk_sys/2 at 315 MHz. That gives a clean 157.5 MHz
     * clk_hstx and, with HSTX CSR CLKDIV=5, a 31.5 MHz pixel clock without
     * touching pll_usb. */
    clock_configure(clk_hstx, 0,
                    CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS,
                    clock_get_hz(clk_sys), HDMI_HSTX_CLOCK_HZ);

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
        HSTX_INTERNAL_CLKDIV << HSTX_CTRL_CSR_CLKDIV_LSB |
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
    s_hdmi_scanline_stalls = 0u;
    s_hdmi_scanline_submitted = 0u;
    s_hdmi_scanline_expanded = 0u;
    s_hdmi_scanline_max_level = 0u;
    s_hdmi_frame_submitted = 0u;
    s_hdmi_frame_expanded = 0u;
    s_dma_pong = false;
    s_v_scanline = 2u;
    s_vactive_cmdlist_posted = false;
    s_started = false;
    s_irq_configured = false;
    s_borders_dirty = true;
    init_palette_rgb332();

    if (s_dmach_ping < 0) {
        s_dmach_ping = dma_claim_unused_channel(true);
    }
    if (s_dmach_pong < 0) {
        s_dmach_pong = dma_claim_unused_channel(true);
    }

#if MICRONES_HDMI_VIDEO_PREAMBLE
    /* Patch the per-pixel HDMI preamble/guard-band words into the active-line
     * cmd list. We use array indices rather than struct offsets to keep this
     * narrow and obvious. */
    s_vactive_line[7] = hdmi_video_preamble_word;
    s_vactive_line[9] = hdmi_video_guardband_word;
#endif

#if MICRONES_HDMI_DATA_ISLANDS
    if (s_hdmi_pcm_lock == NULL) {
        s_hdmi_pcm_lock = spin_lock_init(spin_lock_claim_unused(true));
    }
    s_hdmi_audio_refills = 0u;
    s_hdmi_audio_missed_swaps = 0u;
    s_hdmi_audio_queue_head = 0u;
    s_hdmi_audio_queue_tail = 0u;
    s_hdmi_audio_packet_cursor = 0u;
    s_hdmi_silence_packet_cursor = 0u;
#if MICRONES_HDMI_TEST_TONE
    s_hdmi_test_tone_packet_cursor = 0u;
#endif
    s_hdmi_audio_sample_accum_fp = 0u;
    s_hdmi_audio_frame_no = 0u;
#if MICRONES_HDMI_TEST_TONE
    s_hdmi_test_tone_phase = 0u;
#endif
    s_hdmi_pcm_head = 0u;
    s_hdmi_pcm_tail = 0u;
    s_hdmi_audio_underruns = 0u;
    s_hdmi_audio_overruns = 0u;
    s_hdmi_audio_worker_running = false;

    hdmi_refill_control_islands();

    /* Fill both audio buffers with NULL packets so the receiver sees a
     * coherent data-island stream from the first frame. */
    {
        HdmiPacket null_pkt;
        hdmi_pkt_make_null(&null_pkt);
        hdmi_di_emit_island(&null_pkt, 1u, 1u, 0u, s_hdmi_null_island);
        hdmi_build_silence_islands();
#if MICRONES_HDMI_TEST_TONE
        hdmi_refill_test_tone_islands();
#endif
    }
#endif /* MICRONES_HDMI_DATA_ISLANDS */

    hstx_configure_peripheral();

    /* Save clock info for periodic diagnostic (USB may not be ready at boot). */
    s_diag_sys_hz = clock_get_hz(clk_sys);
    s_diag_hstx_hz = clock_get_hz(clk_hstx);
    s_diag_pixel_hz = s_diag_hstx_hz / 5u;

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
    s_hdmi_scanline_max_level = 0u;
    hstx_configure_peripheral();
    hstx_configure_dma();
#if MICRONES_HDMI_DATA_ISLANDS && MICRONES_HDMI_AUDIO_PACKETS && \
    MICRONES_HDMI_AUDIO_CORE1 && !MICRONES_HDMI_TEST_TONE
    s_hdmi_audio_worker_running = true;
    multicore_launch_core1(video_hstx_audio_core1_entry);
#endif
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

#if MICRONES_HDMI_DATA_ISLANDS && MICRONES_HDMI_AUDIO_PACKETS && \
    MICRONES_HDMI_AUDIO_CORE1 && !MICRONES_HDMI_TEST_TONE
    if (s_hdmi_audio_worker_running) {
        s_hdmi_audio_worker_running = false;
        multicore_reset_core1();
    }
#endif

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

    for (uint32_t y = 0u; y < NES_FRAME_HEIGHT; ++y) {
        const uint8_t *src = nes_framebuffer_scanline_const(frame, (uint16_t)y);
        hdmi_expand_scanline_to_framebuf(src, (uint16_t)y);
    }

    uint64_t elapsed = time_us_64() - start_us;
    s_stats.present_us_total += elapsed;
    if (elapsed > s_stats.present_us_max) {
        s_stats.present_us_max = elapsed;
    }
}

void video_hstx_submit_scanline(const uint8_t *pixels, uint16_t y) {
    if (pixels == NULL || y >= NES_FRAME_HEIGHT) {
        return;
    }

    ++s_hdmi_scanline_submitted;
    hdmi_expand_scanline_to_framebuf(pixels, y);
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

void video_hstx_print_diag(void) {
    static uint32_t prev_refills;
    static uint32_t prev_missed;
    static uint32_t prev_vsubmitted;
    static uint32_t prev_vexpanded;
    static uint32_t prev_vstalls;
    uint32_t refills = 0u;
    uint32_t missed = 0u;
    uint32_t vsubmitted = s_hdmi_scanline_submitted;
    uint32_t vexpanded = s_hdmi_scanline_expanded;
    uint32_t vstalls = s_hdmi_scanline_stalls;
#if MICRONES_HDMI_DATA_ISLANDS
    refills = s_hdmi_audio_refills;
    missed = s_hdmi_audio_missed_swaps;
#endif

    printf("[hstx] sys=%lu hstx=%lu pixel=%lu CTS=%u N=%u tone=%u start=%u worker=%u vf=%llu pcm=%lu q=%lu vq=%lu vmax=%lu dref=%lu dmiss=%lu dvsub=%lu dvexp=%lu dvstall=%lu vfsub=%lu vfexp=%lu underruns=%lu overruns=%lu pkts=%lu\n",
           (unsigned long)s_diag_sys_hz,
           (unsigned long)s_diag_hstx_hz,
           (unsigned long)s_diag_pixel_hz,
           (unsigned)HDMI_AUDIO_CTS_VALUE,
           (unsigned)HDMI_AUDIO_N_VALUE,
           (unsigned)MICRONES_HDMI_TEST_TONE,
           (unsigned)s_started,
#if MICRONES_HDMI_DATA_ISLANDS
           (unsigned)s_hdmi_audio_worker_running,
#else
           0u,
#endif
           (unsigned long long)s_stats.frames_presented,
#if MICRONES_HDMI_DATA_ISLANDS
           (unsigned long)video_hstx_hdmi_audio_buffer_level(),
           (unsigned long)hdmi_audio_queue_level(),
           (unsigned long)hdmi_scanline_queue_level(),
           (unsigned long)s_hdmi_scanline_max_level,
           (unsigned long)(refills - prev_refills),
           (unsigned long)(missed - prev_missed),
           (unsigned long)(vsubmitted - prev_vsubmitted),
           (unsigned long)(vexpanded - prev_vexpanded),
           (unsigned long)(vstalls - prev_vstalls),
           (unsigned long)s_hdmi_frame_submitted,
           (unsigned long)s_hdmi_frame_expanded,
           (unsigned long)video_hstx_hdmi_audio_underruns(),
           (unsigned long)video_hstx_hdmi_audio_overruns(),
           (unsigned long)s_hdmi_audio_packet_cursor
#else
           0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL
#endif
    );
    prev_refills = refills;
    prev_missed = missed;
    prev_vsubmitted = vsubmitted;
    prev_vexpanded = vexpanded;
    prev_vstalls = vstalls;
}

/* --- HDMI audio backend entry points ----------------------------------- */

void video_hstx_hdmi_audio_service(void) {
#if MICRONES_HDMI_DATA_ISLANDS && MICRONES_HDMI_AUDIO_PACKETS
#if MICRONES_HDMI_TEST_TONE
    return;
#else
    if (s_hdmi_audio_worker_running) {
        return;
    }
    while (hdmi_audio_encode_one_packet()) {
    }
#endif
#endif
}

void video_hstx_hdmi_audio_init(uint32_t sample_rate) {
    (void)sample_rate;
#if MICRONES_HDMI_DATA_ISLANDS
    /* The data-island scheduler is fixed for 48 kHz; we ignore the requested
     * rate and rely on the audio backend wrapper to declare 48 kHz. */
    s_hdmi_pcm_head = 0u;
    s_hdmi_pcm_tail = 0u;
    s_hdmi_audio_refills = 0u;
    s_hdmi_audio_missed_swaps = 0u;
    s_hdmi_audio_queue_head = 0u;
    s_hdmi_audio_queue_tail = 0u;
    s_hdmi_audio_packet_cursor = 0u;
    s_hdmi_silence_packet_cursor = 0u;
#if MICRONES_HDMI_TEST_TONE
    s_hdmi_test_tone_packet_cursor = 0u;
#endif
    s_hdmi_audio_sample_accum_fp = 0u;
    s_hdmi_audio_frame_no = 0u;
#if MICRONES_HDMI_TEST_TONE
    s_hdmi_test_tone_phase = 0u;
#endif
    s_hdmi_audio_underruns = 0u;
    s_hdmi_audio_overruns = 0u;
#endif
}

size_t video_hstx_hdmi_audio_push(const int16_t *samples,
                                  size_t count) {
#if MICRONES_HDMI_DATA_ISLANDS
#if MICRONES_HDMI_TEST_TONE
    (void)samples;
    (void)count;
    return 0u;
#else
    if (samples == NULL || count == 0u) {
        return 0u;
    }
    size_t written = 0u;
    uint32_t save = spin_lock_blocking(s_hdmi_pcm_lock);
    for (size_t i = 0u; i < count; ++i) {
        uint32_t head = s_hdmi_pcm_head;
        uint32_t next = (head + 1u) & (HDMI_PCM_RING_FRAMES - 1u);
        if (next == s_hdmi_pcm_tail) {
            s_hdmi_pcm_tail = (s_hdmi_pcm_tail + 1u) & (HDMI_PCM_RING_FRAMES - 1u);
            ++s_hdmi_audio_overruns;
        }
        /* Backend contract is mono; duplicate to L and R. */
        s_hdmi_pcm_ring[head * 2u + 0u] = samples[i];
        s_hdmi_pcm_ring[head * 2u + 1u] = samples[i];
        s_hdmi_pcm_head = next;
        ++written;
    }
    spin_unlock(s_hdmi_pcm_lock, save);
    return written;
#endif
#else
    (void)samples;
    return count;
#endif
}

uint32_t video_hstx_hdmi_audio_underruns(void) {
#if MICRONES_HDMI_DATA_ISLANDS
    return s_hdmi_audio_underruns;
#else
    return 0u;
#endif
}

uint32_t video_hstx_hdmi_audio_overruns(void) {
#if MICRONES_HDMI_DATA_ISLANDS
    return s_hdmi_audio_overruns;
#else
    return 0u;
#endif
}

uint32_t video_hstx_hdmi_audio_buffer_level(void) {
#if MICRONES_HDMI_DATA_ISLANDS
    return hdmi_pcm_available_frames();
#else
    return 0u;
#endif
}
