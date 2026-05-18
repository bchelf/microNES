/*
 * video_hstx.c — RP2350 HSTX DVI/HDMI driver for micrones.
 *
 * Architecture (mirrors pico-examples/hstx/dvi_out_hstx_encoder):
 *
 *   Two ping-pong "scanline" buffers each hold the FULL set of HSTX
 *   command words for one DVI line — including the HBP / active-pixel /
 *   HFP / HSYNC sub-sections inline.  At any moment one buffer is being
 *   DMA-streamed into the HSTX FIFO; the other is being refilled by the
 *   line-completion IRQ with whatever line is two ahead of the wire.
 *
 *      +-------------------+     +-------------------+
 *      |   line buffer A   |◀────|   ch_pixel (DMA)  |─▶ HSTX FIFO ─▶ pads
 *      |  327 × 32-bit     |     |   327 words/line  |    GP12..GP19
 *      +-------------------+     |   chain → ch_cmd  |
 *      +-------------------+     +-------------------+
 *      |   line buffer B   |              ▲
 *      |  327 × 32-bit     |              │
 *      +-------------------+      +---------------+
 *                                 |    ch_cmd     |  reads {addr,count}
 *                                 |  ring of 2    |  pairs from RAM and
 *                                 |  descriptors  |  retriggers ch_pixel
 *                                 +---------------+
 *
 *   ch_pixel's done IRQ fires once per line.  Inside the handler we:
 *     - bump the V counter
 *     - identify which line buffer just finished (ping vs pong)
 *     - rebuild that buffer for "current line + 2" (the line AFTER the
 *       one HSTX is streaming right now)
 *     - convert the corresponding NES scanline (with 2x H/V doubling and
 *       64-px black pillarbox) for ACTIVE lines
 *
 *   Per-line refill cost: ~1–2 µs at 252 MHz; total ~1 ms / frame.  This
 *   runs from an IRQ on core 0 but does not block the emulator's main
 *   loop because the bus_ctrl priority gives DMA preference.  HSTX never
 *   underruns provided the IRQ completes before the next line ends
 *   (~31.7 µs).
 *
 * NOTE on portability: this file uses RP2350-specific HSTX register names
 * (`hstx_ctrl_hw`, `hstx_fifo_hw`) and the DREQ_HSTX symbol.  Those are
 * present in pico-sdk ≥ 2.0.0 with PICO_BOARD=pico2.
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
#include "pico/multicore.h"
#include "pico/platform.h"
#include "pico/time.h"

#include <stdio.h>
#include <string.h>

#if MICRONES_SYS_CLK_MHZ != 252
#  warning "video_hstx expects MICRONES_SYS_CLK_MHZ=252 for clean 640x480@60 timing"
#endif

/* ---- Mode timing (640x480@60Hz, VESA / CEA-861) ---------------------- */
enum {
    HSTX_MODE_H_ACTIVE_PX = 640,
    HSTX_MODE_H_FRONT_PORCH = 16,
    HSTX_MODE_H_SYNC_WIDTH = 96,
    HSTX_MODE_H_BACK_PORCH = 48,
    HSTX_MODE_H_TOTAL = 800,

    HSTX_MODE_V_ACTIVE_LINES = 480,
    HSTX_MODE_V_FRONT_PORCH = 10,
    HSTX_MODE_V_SYNC_WIDTH = 2,
    HSTX_MODE_V_BACK_PORCH = 33,
    HSTX_MODE_V_TOTAL = 525,

    /* 640 = 64 (left pillarbox) + 512 (NES 2x) + 64 (right pillarbox).
     * Active picture is centred horizontally. */
    HSTX_PILLAR_LEFT_PX = 64,
    HSTX_NES_SCALED_WIDTH_PX = 512,
    HSTX_PILLAR_RIGHT_PX = 64,
};

_Static_assert(HSTX_MODE_H_ACTIVE_PX == HSTX_PILLAR_LEFT_PX +
                                            HSTX_NES_SCALED_WIDTH_PX +
                                            HSTX_PILLAR_RIGHT_PX,
               "640 = 64 + 512 + 64");

_Static_assert(HSTX_MODE_V_ACTIVE_LINES == NES_FRAME_HEIGHT * 2,
               "vertical 2x duplication");

/* ---- HSTX command word layout ----------------------------------------
 *
 * The HSTX peripheral reads 32-bit words from its FIFO.  The high 4 bits
 * select a "command" that controls how the lower 28 bits are interpreted.
 * Commands documented in the RP2350 datasheet § "HSTX".
 */
#define HSTX_CMD_RAW         (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

static inline uint32_t hstx_cmd(uint32_t cmd, uint32_t count) {
    return cmd | (count & 0xfffu);
}

/* TMDS sync-pair "control" symbols. */
#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

/* 30-bit RAW symbols for the three TMDS lanes.  Lane 0 carries the sync
 * bits in DVI; lanes 1 and 2 stay at the "no signal" control symbol. */
/* 640x480@60 uses negative H/V sync polarity.  During blanking, inactive
 * syncs are encoded as V=1,H=1; asserted sync pulses drive the relevant bit
 * low.  Lanes 1 and 2 always carry CTRL_00 in DVI control periods. */
#define HSTX_SYM_NO_SYNC ((TMDS_CTRL_00 << 20) | (TMDS_CTRL_00 << 10) | TMDS_CTRL_11)
#define HSTX_SYM_HSYNC   ((TMDS_CTRL_00 << 20) | (TMDS_CTRL_00 << 10) | TMDS_CTRL_10)
#define HSTX_SYM_VSYNC   ((TMDS_CTRL_00 << 20) | (TMDS_CTRL_00 << 10) | TMDS_CTRL_01)
#define HSTX_SYM_VHSYNC  ((TMDS_CTRL_00 << 20) | (TMDS_CTRL_00 << 10) | TMDS_CTRL_00)

/* ---- Per-line buffer layout ------------------------------------------
 *
 * One buffer holds the entire DMA stream for one DVI line:
 *
 *   index  contents
 *   ───── ──────────────────────────────────────────────────────────────
 *   0      RAW_REPEAT(48)  — HBP duration, then symbol
 *   1      HBP symbol (no_sync, vsync, etc. depending on V region)
 *   2      TMDS(640)       — active picture command
 *   3..322 320 × packed RGB565 pairs (640 pixels)  — for active lines;
 *          for blanking lines this region holds RAW_REPEAT(640)+symbol +
 *          padding NOPs to keep the buffer length uniform
 *   323    RAW_REPEAT(16)  — HFP
 *   324    HFP symbol
 *   325    RAW_REPEAT(96)  — HSYNC
 *   326    HSYNC symbol
 *
 * Total: 327 words per buffer.  Two buffers (ping-pong) = 654 words = 2616
 * bytes RAM.
 */
#define HSTX_LINE_WORDS     327u
#define HSTX_PIX_REGION_OFF 3u           /* index where 320-word pixel data begins */
#define HSTX_PIX_REGION_LEN 320u         /* 640 px / 2 px per word */

static uint32_t s_line_buf[2][HSTX_LINE_WORDS] __attribute__((aligned(8)));

/* The "present" buffer is the indexed NES image the emulator most recently
 * handed us.  The line-fill ISR converts this on-the-fly into the active
 * line buffer. */
static uint8_t s_present_indexed[NES_FRAME_WIDTH * NES_FRAME_HEIGHT];
static volatile bool s_present_valid = false;

/* RGB565 little-endian palette (HSTX consumes native words).
 *
 * Placed in SRAM (`__not_in_flash`) so the line-fill ISR never stalls on an
 * XIP cache miss reading the palette.  Const-correctness is sacrificed for
 * latency determinism — pico-sdk's `__not_in_flash` decoration forces the
 * variable into a .data section in RAM. */
#define RGB565_LE(r, g, b)                                                 \
    ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))

static const uint16_t __not_in_flash("hstx_palette")
    k_nes_palette_rgb565_le[64] = {
    RGB565_LE( 84,  84,  84), RGB565_LE(  0,  30, 116), RGB565_LE(  8,  16, 144), RGB565_LE( 48,   0, 136),
    RGB565_LE( 68,   0, 100), RGB565_LE( 92,   0,  48), RGB565_LE( 84,   4,   0), RGB565_LE( 60,  24,   0),
    RGB565_LE( 32,  42,   0), RGB565_LE(  8,  58,   0), RGB565_LE(  0,  64,   0), RGB565_LE(  0,  60,   0),
    RGB565_LE(  0,  50,  60), RGB565_LE(  0,   0,   0), RGB565_LE(  0,   0,   0), RGB565_LE(  0,   0,   0),

    RGB565_LE(152, 150, 152), RGB565_LE(  8,  76, 196), RGB565_LE( 48,  50, 236), RGB565_LE( 92,  30, 228),
    RGB565_LE(136,  20, 176), RGB565_LE(160,  20, 100), RGB565_LE(152,  34,  32), RGB565_LE(120,  60,   0),
    RGB565_LE( 84,  90,   0), RGB565_LE( 40, 114,   0), RGB565_LE(  8, 124,   0), RGB565_LE(  0, 118,  40),
    RGB565_LE(  0, 102, 120), RGB565_LE(  0,   0,   0), RGB565_LE(  0,   0,   0), RGB565_LE(  0,   0,   0),

    RGB565_LE(236, 238, 236), RGB565_LE( 76, 154, 236), RGB565_LE(120, 124, 236), RGB565_LE(176,  98, 236),
    RGB565_LE(228,  84, 236), RGB565_LE(236,  88, 180), RGB565_LE(236, 106, 100), RGB565_LE(212, 136,  32),
    RGB565_LE(160, 170,   0), RGB565_LE(116, 196,   0), RGB565_LE( 76, 208,  32), RGB565_LE( 56, 204, 108),
    RGB565_LE( 56, 180, 204), RGB565_LE( 60,  60,  60), RGB565_LE(  0,   0,   0), RGB565_LE(  0,   0,   0),

    RGB565_LE(236, 238, 236), RGB565_LE(168, 204, 236), RGB565_LE(188, 188, 236), RGB565_LE(212, 178, 236),
    RGB565_LE(236, 174, 236), RGB565_LE(236, 174, 212), RGB565_LE(236, 180, 176), RGB565_LE(228, 196, 144),
    RGB565_LE(204, 210, 120), RGB565_LE(180, 222, 120), RGB565_LE(168, 226, 144), RGB565_LE(152, 226, 180),
    RGB565_LE(160, 214, 228), RGB565_LE(160, 162, 160), RGB565_LE(  0,   0,   0), RGB565_LE(  0,   0,   0),
};

/* ---- DMA chain --------------------------------------------------------
 *
 * Two channels are configured as symmetric pixel-DMA ping/pong channels.  Each
 * streams one complete HSTX line buffer into the FIFO, then chains to the
 * other channel.  The line-completion IRQ rearms the channel that just
 * finished while the other channel is already streaming the next line.
 */
static int s_dma_ch[2] = { -1, -1 };

/* Line counter advanced on every line-completion IRQ.
 *
 * Convention: at the moment the IRQ fires, HSTX has JUST finished one line
 * and channel chaining has already triggered the other DMA channel for the
 * next line.  So we need to refill the just-finished buffer with the line
 * that is "current+1" (i.e. two lines ahead of what the wire shows). */
static volatile uint32_t s_line_being_streamed = 0u; /* line index in V frame */

static VideoHstxStats s_stats;
static char s_last_error[96];

static void hstx_set_error(const char *msg) {
    snprintf(s_last_error, sizeof(s_last_error), "%s", msg);
}

/* Full diagnostic block — printed once at init and again every 5 s from
 * the core-1 diagnostic thread (see below).
 *
 * Why core 1: USB-CDC printf is slow and its TX queue (~256 bytes) drops
 * characters when overrun.  Printing 13+ lines from an IRQ context fills
 * the queue faster than USB can drain it, so most lines get dropped.
 * Running the same printf on core 1 in a regular thread context lets
 * USB drain naturally between bursts — no character loss. */
static void hstx_dump_diagnostics_full(const char *prefix) {
    printf("%s sys_clk_hz     = %lu\n",
           prefix, (unsigned long)clock_get_hz(clk_sys));
    printf("%s clk_hstx_hz    = %lu (expected %lu = sys_clk / 2)\n",
           prefix,
           (unsigned long)clock_get_hz(clk_hstx),
           (unsigned long)(clock_get_hz(clk_sys) / 2u));
    printf("%s CSR            = 0x%08lx\n",
           prefix, (unsigned long)hstx_ctrl_hw->csr);
    printf("%s expand_shift   = 0x%08lx\n",
           prefix, (unsigned long)hstx_ctrl_hw->expand_shift);
    printf("%s expand_tmds    = 0x%08lx\n",
           prefix, (unsigned long)hstx_ctrl_hw->expand_tmds);
    for (uint32_t i = 0; i < 8u; ++i) {
        printf("%s bit[%lu]         = 0x%08lx\n",
               prefix, (unsigned long)i,
               (unsigned long)hstx_ctrl_hw->bit[i]);
    }
    printf("%s dma channels   = pixel0=%d pixel1=%d\n",
           prefix, s_dma_ch[0], s_dma_ch[1]);
    printf("%s line streaming = %lu  (advances => ISR alive)\n",
           prefix, (unsigned long)s_line_being_streamed);
    printf("%s frames         = %llu\n",
           prefix, (unsigned long long)s_stats.frames_presented);
}

/* Core-1 diagnostic thread.  Sleeps 5 s, dumps the full register state,
 * repeats.  Runs in a regular thread context so USB-CDC has room to
 * drain between bursts — no truncation. */
static void hstx_diag_core1_entry(void) {
    /* Small initial delay so the very first dump from this thread
     * doesn't collide with the init-time dump still draining out. */
    sleep_ms(1000);
    while (true) {
        sleep_ms(5000);
        hstx_dump_diagnostics_full("[hstx][5s]");
    }
}

const char *video_hstx_last_error(void) {
    return s_last_error;
}

/* ---- Line buffer construction ----------------------------------------
 *
 * Build one full line into `buf`, for V-line index `line` (0..524).  Three
 * shapes:
 *   - Active picture (480 lines): pixel data from NES framebuffer.
 *   - V-sync (2 lines): VSYNC asserted across HBP/active/HFP, V+H during
 *     HSYNC region.
 *   - V-blank (front porch, back porch — 43 lines total): blanking.
 */

static void __not_in_flash_func(hstx_build_blank_line)(uint32_t *buf,
                                                       uint32_t sym_no_active,
                                                       uint32_t sym_hsync) {
    /* Header: HBP duration */
    buf[0] = hstx_cmd(HSTX_CMD_RAW_REPEAT, HSTX_MODE_H_BACK_PORCH);
    buf[1] = sym_no_active;
    /* Active region: same blanking symbol for the whole picture area. */
    buf[2] = hstx_cmd(HSTX_CMD_RAW_REPEAT, HSTX_MODE_H_ACTIVE_PX);
    buf[3] = sym_no_active;
    /* Pad the rest of the pixel region with NOPs so the buffer length is
     * uniform across line types.  HSTX_CMD_NOP advances by `count` cycles
     * but does not assert any TMDS data. */
    for (uint32_t i = HSTX_PIX_REGION_OFF + 2u;
         i < HSTX_PIX_REGION_OFF + HSTX_PIX_REGION_LEN; ++i) {
        buf[i] = hstx_cmd(HSTX_CMD_NOP, 0);
    }
    /* Trailer: HFP, HSYNC */
    uint32_t t = HSTX_PIX_REGION_OFF + HSTX_PIX_REGION_LEN;
    buf[t + 0] = hstx_cmd(HSTX_CMD_RAW_REPEAT, HSTX_MODE_H_FRONT_PORCH);
    buf[t + 1] = sym_no_active;
    buf[t + 2] = hstx_cmd(HSTX_CMD_RAW_REPEAT, HSTX_MODE_H_SYNC_WIDTH);
    buf[t + 3] = sym_hsync;
}

static void __not_in_flash_func(hstx_build_active_line_pixels)(uint32_t *buf,
                                                                uint32_t nes_y) {
    /* Header: HBP, then the TMDS-encode-N command for 640 pixels. */
    buf[0] = hstx_cmd(HSTX_CMD_RAW_REPEAT, HSTX_MODE_H_BACK_PORCH);
    buf[1] = HSTX_SYM_NO_SYNC;
    buf[2] = hstx_cmd(HSTX_CMD_TMDS, HSTX_MODE_H_ACTIVE_PX);

    /* Pixel region: 320 × 32-bit packed RGB565 pairs.
     *   word[0]      = (px[1] << 16) | px[0]
     *   word[1]      = (px[3] << 16) | px[2]
     *
     * The expand_shift config below tells HSTX to consume two 16-bit slots
     * per FIFO word (low half first), matching this packing.
     *
     * Each NES pixel is duplicated horizontally (2x scale) and the 64-px
     * pillarbox columns on each side are black. */
    uint32_t *out = &buf[HSTX_PIX_REGION_OFF];

    /* Left pillarbox: 64 black pixels = 32 packed words. */
    for (uint32_t i = 0; i < HSTX_PILLAR_LEFT_PX / 2u; ++i) {
        out[i] = 0u;
    }
    out += HSTX_PILLAR_LEFT_PX / 2u;

    if (nes_y < NES_FRAME_HEIGHT) {
        const uint8_t *src = &s_present_indexed[nes_y * NES_FRAME_WIDTH];
        /* 256 source pixels → 256 packed words (each word = same colour
         * doubled for the 2x scale: low half = pixel[i], high half =
         * pixel[i] = same value, so HSTX emits two identical adjacent
         * pixels). */
        for (uint32_t i = 0; i < NES_FRAME_WIDTH; i += 4) {
            uint16_t c0 = k_nes_palette_rgb565_le[src[i + 0] & 0x3fu];
            uint16_t c1 = k_nes_palette_rgb565_le[src[i + 1] & 0x3fu];
            uint16_t c2 = k_nes_palette_rgb565_le[src[i + 2] & 0x3fu];
            uint16_t c3 = k_nes_palette_rgb565_le[src[i + 3] & 0x3fu];
            out[0] = ((uint32_t)c0 << 16) | c0;
            out[1] = ((uint32_t)c1 << 16) | c1;
            out[2] = ((uint32_t)c2 << 16) | c2;
            out[3] = ((uint32_t)c3 << 16) | c3;
            out += 4;
        }
    } else {
        /* Out-of-range — paint black (shouldn't happen). */
        for (uint32_t i = 0; i < NES_FRAME_WIDTH; ++i) {
            out[i] = 0u;
        }
        out += NES_FRAME_WIDTH;
    }

    /* Right pillarbox: 64 black pixels = 32 packed words. */
    for (uint32_t i = 0; i < HSTX_PILLAR_RIGHT_PX / 2u; ++i) {
        out[i] = 0u;
    }

    /* Trailer: HFP, HSYNC. */
    uint32_t t = HSTX_PIX_REGION_OFF + HSTX_PIX_REGION_LEN;
    buf[t + 0] = hstx_cmd(HSTX_CMD_RAW_REPEAT, HSTX_MODE_H_FRONT_PORCH);
    buf[t + 1] = HSTX_SYM_NO_SYNC;
    buf[t + 2] = hstx_cmd(HSTX_CMD_RAW_REPEAT, HSTX_MODE_H_SYNC_WIDTH);
    buf[t + 3] = HSTX_SYM_HSYNC;
}

static void __not_in_flash_func(hstx_build_line)(uint32_t *buf,
                                                  uint32_t line) {
    /* V layout: [VFP 10][VSYNC 2][VBP 33][active 480] = 525 */
    if (line < HSTX_MODE_V_FRONT_PORCH) {
        hstx_build_blank_line(buf, HSTX_SYM_NO_SYNC, HSTX_SYM_HSYNC);
        return;
    }
    if (line < HSTX_MODE_V_FRONT_PORCH + HSTX_MODE_V_SYNC_WIDTH) {
        hstx_build_blank_line(buf, HSTX_SYM_VSYNC, HSTX_SYM_VHSYNC);
        return;
    }
    if (line < HSTX_MODE_V_FRONT_PORCH + HSTX_MODE_V_SYNC_WIDTH +
                   HSTX_MODE_V_BACK_PORCH) {
        hstx_build_blank_line(buf, HSTX_SYM_NO_SYNC, HSTX_SYM_HSYNC);
        return;
    }
    /* Active picture. */
    uint32_t v_blank_lines = HSTX_MODE_V_FRONT_PORCH + HSTX_MODE_V_SYNC_WIDTH +
                             HSTX_MODE_V_BACK_PORCH;
    uint32_t active_line = line - v_blank_lines;       /* 0..479 */
    uint32_t nes_y = active_line >> 1;                 /* /2 = vertical 2x */
    if (s_present_valid) {
        hstx_build_active_line_pixels(buf, nes_y);
    } else {
        hstx_build_blank_line(buf, HSTX_SYM_NO_SYNC, HSTX_SYM_HSYNC);
    }
}

/* DMA-completion ISR — runs once per DVI line.
 *
 * Placed in RAM (__not_in_flash_func) so an XIP cache miss can never delay
 * the handler past the 31.7 µs per-line budget.  At ~250 MHz a flash miss
 * costs hundreds of cycles plus a worst-case ~10 µs XIP read, which alone
 * would consume a third of the budget. */
static void __not_in_flash_func(hstx_dma_irq_handler)(void) {
    uint32_t pending = dma_hw->ints0;
    int finished_idx = -1;

    if (s_dma_ch[0] >= 0 && (pending & (1u << (uint32_t)s_dma_ch[0]))) {
        finished_idx = 0;
    } else if (s_dma_ch[1] >= 0 && (pending & (1u << (uint32_t)s_dma_ch[1]))) {
        finished_idx = 1;
    } else {
        return;
    }

    dma_hw->ints0 = 1u << (uint32_t)s_dma_ch[finished_idx];

    /* Advance the V counter — what HSTX is streaming RIGHT NOW. */
    uint32_t next_line = s_line_being_streamed + 1u;
    if (next_line >= HSTX_MODE_V_TOTAL) {
        next_line = 0u;
        s_stats.frames_presented++;
    }
    s_line_being_streamed = next_line;

    /* Refill the just-finished buffer with the line AFTER the one HSTX
     * is now streaming.  i.e. (next_line + 1) mod V_TOTAL. */
    uint32_t fill_line = next_line + 1u;
    if (fill_line >= HSTX_MODE_V_TOTAL) {
        fill_line = 0u;
    }

#if MICRONES_ENABLE_PERF_LOG
    uint64_t t0 = time_us_64();
    hstx_build_line(s_line_buf[finished_idx], fill_line);
    uint64_t dt = time_us_64() - t0;
    s_stats.lines_filled++;
    s_stats.fill_us_total += dt;
    if (dt > s_stats.fill_us_max_per_line) {
        s_stats.fill_us_max_per_line = dt;
    }
#else
    hstx_build_line(s_line_buf[finished_idx], fill_line);
    s_stats.lines_filled++;
#endif

    dma_channel_set_read_addr((uint)s_dma_ch[finished_idx],
                              s_line_buf[finished_idx], false);
    dma_channel_set_trans_count((uint)s_dma_ch[finished_idx],
                                HSTX_LINE_WORDS, false);
}

/* ---- Peripheral configuration ---------------------------------------- */

static void hstx_configure_peripheral(void) {
    /* CRITICAL: on RP2350 the HSTX block boots in RESET state.  Register
     * writes to it are silently dropped until we explicitly unreset.
     * Symptom of skipping this step: GPIOs configured to GPIO_FUNC_HSTX
     * sit at their pad defaults (one pin of each differential pair at
     * 3.3 V, the other at 0 V) — no TMDS output at all. */
    reset_unreset_block_num_wait_blocking(RESET_HSTX);

    const uint32_t sys_hz = MICRONES_PLL_VCO_HZ /
                            (MICRONES_PLL_DIV1 * MICRONES_PLL_DIV2);

    /* Drive clk_hstx at sys_clk / 2.  The HSTX CSR below then emits two bits
     * per HSTX clock, five shifts per 10-bit TMDS symbol:
     *   pixel_clk = (clk_hstx * 2) / 10 = sys_clk / 10 = 25.2 MHz. */
    clock_configure(clk_hstx, 0,
                    CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS,
                    sys_hz, sys_hz / 2u);

    /* Pin map (relative to GP12, the HSTX pin base) — matches the v0.1
     * board layout where the lower-numbered GP in each pair carries the
     * NEGATIVE signal of the differential pair:
     *
     *   offset 0 — GP12 — CK-   (HDMI pin 12)
     *   offset 1 — GP13 — CK+   (HDMI pin 10)
     *   offset 2 — GP14 — D0-   (HDMI pin 9,  Blue-)
     *   offset 3 — GP15 — D0+   (HDMI pin 7,  Blue+)
     *   offset 4 — GP16 — D1-   (HDMI pin 6,  Green-)
     *   offset 5 — GP17 — D1+   (HDMI pin 4,  Green+)
     *   offset 6 — GP18 — D2-   (HDMI pin 3,  Red-)
     *   offset 7 — GP19 — D2+   (HDMI pin 1,  Red+)
     *
     * Each pair drives the same TMDS lane but with opposite polarity.
     * The `INV` bit applied to the negative pin makes it carry the
     * complement of the positive pin's signal.
     *
     * NOTE: if you wire to a different breakout that puts CK+ on GP12
     * (e.g. Pico-DVI-Sock convention), swap INV onto the odd-numbered
     * `bit[N]` entries instead — i.e. move HSTX_CTRL_BIT0_INV_BITS up
     * one line in each pair below.
     */
    hstx_ctrl_hw->bit[0] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    hstx_ctrl_hw->bit[1] = HSTX_CTRL_BIT0_CLK_BITS;

    hstx_ctrl_hw->bit[2] = (0u << HSTX_CTRL_BIT0_SEL_P_LSB) |
                           (1u << HSTX_CTRL_BIT0_SEL_N_LSB) |
                           HSTX_CTRL_BIT0_INV_BITS;
    hstx_ctrl_hw->bit[3] = (0u << HSTX_CTRL_BIT0_SEL_P_LSB) |
                           (1u << HSTX_CTRL_BIT0_SEL_N_LSB);

    hstx_ctrl_hw->bit[4] = (10u << HSTX_CTRL_BIT0_SEL_P_LSB) |
                           (11u << HSTX_CTRL_BIT0_SEL_N_LSB) |
                           HSTX_CTRL_BIT0_INV_BITS;
    hstx_ctrl_hw->bit[5] = (10u << HSTX_CTRL_BIT0_SEL_P_LSB) |
                           (11u << HSTX_CTRL_BIT0_SEL_N_LSB);

    hstx_ctrl_hw->bit[6] = (20u << HSTX_CTRL_BIT0_SEL_P_LSB) |
                           (21u << HSTX_CTRL_BIT0_SEL_N_LSB) |
                           HSTX_CTRL_BIT0_INV_BITS;
    hstx_ctrl_hw->bit[7] = (20u << HSTX_CTRL_BIT0_SEL_P_LSB) |
                           (21u << HSTX_CTRL_BIT0_SEL_N_LSB);

    /* RGB565 layout (bits 15..0): RRRRR GGGGGG BBBBB.
     * The HSTX TMDS encoder reads each lane starting at bit 7 after the
     * configured right-rotate, so the rotations move B0/G0/R0 to bit 7.
     * `nbits` field is encoded as `bits-1`. */
    hstx_ctrl_hw->expand_tmds =
        (4u << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB) |
        (4u << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB) |
        (5u << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB) |
        (30u << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB) |
        (4u << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB) |
        (25u << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB);

    /* Two pixels per FIFO word, shift 16 bits between them. */
    hstx_ctrl_hw->expand_shift =
        (2u << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB) |
        (16u << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB) |
        (1u << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB) |
        (0u << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB);

    /* Main control:
     *   shift = 2  (DDR — 2 bits per HSTX cycle)
     *   n_shifts = 5 (10-bit TMDS symbol = 5 DDR pairs)
     *   clkdiv = 1 (HSTX_clk = sys_clk / 2 = 126 MHz @ sys_clk=252)
     *
     * RP2350 HSTX clkdiv field: HSTX_clk = sys_clk / (clkdiv + 1).
     * Setting clkdiv=1 gives /2 — the pixel rate is then
     *   pixel_clk = HSTX_clk × (DDR=2) / (TMDS_bits=10)
     *             = (sys_clk/2) × 2 / 10
     *             = sys_clk / 10
     *             = 25.2 MHz (with sys_clk = 252 MHz)
     */
    hstx_ctrl_hw->csr = 0u;
    hstx_ctrl_hw->csr =
        (5u << HSTX_CTRL_CSR_N_SHIFTS_LSB) |
        (2u << HSTX_CTRL_CSR_SHIFT_LSB) |
        (1u << HSTX_CTRL_CSR_CLKDIV_LSB) |
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        HSTX_CTRL_CSR_EN_BITS;
}

static void hstx_configure_pins(void) {
    for (uint gpio = 12; gpio <= 19; ++gpio) {
        gpio_set_function(gpio, GPIO_FUNC_HSTX);
        /* 252 Mbps over short PCB traces — 8 mA drive is a reasonable
         * default; tune this for your board's trace impedance. */
        gpio_set_drive_strength(gpio, GPIO_DRIVE_STRENGTH_8MA);
    }
}

static void hstx_configure_dma(void) {
    s_dma_ch[0] = dma_claim_unused_channel(true);
    s_dma_ch[1] = dma_claim_unused_channel(true);

    for (uint32_t i = 0; i < 2u; ++i) {
        dma_channel_config c = dma_channel_get_default_config((uint)s_dma_ch[i]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, DREQ_HSTX);
        channel_config_set_chain_to(&c, (uint)s_dma_ch[i ^ 1u]);
        channel_config_set_irq_quiet(&c, false);
        channel_config_set_high_priority(&c, true);
        dma_channel_configure((uint)s_dma_ch[i], &c,
                              &hstx_fifo_hw->fifo,
                              s_line_buf[i],
                              HSTX_LINE_WORDS,
                              false);
    }

    /* Bus priority: pixel DMA must always win arbitration so HSTX never
     * underruns.  Granting the entire DMA controller priority over the
     * CPUs is the standard pattern for this peripheral. */
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_R_BITS |
                            BUSCTRL_BUS_PRIORITY_DMA_W_BITS;

    /* IRQ on every line completion. */
    dma_channel_set_irq0_enabled((uint)s_dma_ch[0], true);
    dma_channel_set_irq0_enabled((uint)s_dma_ch[1], true);
    irq_set_exclusive_handler(DMA_IRQ_0, hstx_dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);
}

bool video_hstx_init(void) {
    hstx_set_error("");

    printf("[hstx] init: starting\n");

    memset(s_line_buf, 0, sizeof(s_line_buf));
    memset(s_present_indexed, 0, sizeof(s_present_indexed));
    s_present_valid = false;
    s_line_being_streamed = 0u;
    memset(&s_stats, 0, sizeof(s_stats));

    /* Pre-build the first two lines (V0 and V1) so the chain has valid
     * data when it kicks off. */
    hstx_build_line(s_line_buf[0], 0);
    hstx_build_line(s_line_buf[1], 1);

    hstx_configure_peripheral();
    hstx_configure_pins();
    hstx_configure_dma();

    dma_hw->ints0 = (1u << (uint32_t)s_dma_ch[0]) |
                    (1u << (uint32_t)s_dma_ch[1]);
    dma_channel_start((uint)s_dma_ch[0]);

    /* Dump the full state once after init completes... */
    hstx_dump_diagnostics_full("[hstx][init]");

    /* ...and then again every 5 s from a core-1 thread.  Running it on
     * core 1 avoids USB-CDC truncation we'd hit if we printed from a
     * timer IRQ on core 0.  Safe to use core 1 here because the HDMI
     * backend doesn't otherwise need it (HSTX is fully DMA-driven). */
    multicore_launch_core1(hstx_diag_core1_entry);

    printf("[hstx] init: complete (DMA chain running, diag every 5s on core 1)\n");

    return true;
}

void __not_in_flash_func(video_hstx_present_frame)(const NesFrameBuffer *frame) {
    if (frame == NULL) return;

    /* The fill ISR reads s_present_indexed at LINE rate (every ~31 µs).
     * memcpy of 60 KB ≈ 50 µs at 252 MHz, which crosses 1–2 HDMI lines.
     *
     * We deliberately do NOT gate the ISR on a "valid" flag during the
     * copy.  A flag-gate would paint those 1–2 lines black on every
     * present, producing a visible flicker band at the tear point.  By
     * letting the ISR read the half-updated buffer instead, the worst
     * case is 1–2 lines of mixed old/new pixels — an ordinary tear, not
     * a flash, and far less perceptible.
     *
     * s_present_valid stays true for the lifetime of the link after the
     * first present.  Before the first present it gates blanking so the
     * boot screen is black instead of garbage. */
    memcpy(s_present_indexed, frame->pixels, sizeof(s_present_indexed));
    s_present_valid = true;
}

void __not_in_flash_func(video_hstx_draw_test_pattern)(void) {
    /*
     * Diagnostic test pattern designed to expose common bring-up failures:
     *
     *   ┌─────────────────────────────────────┐
     *   │ ░  WHT YEL CYN GRN MAG RED BLU BLK ░ │  <- top half: NES palette bars
     *   │ ░                                ░ │     (one bar per ~32 px column)
     *   │ ░  WHT YEL CYN GRN MAG RED BLU BLK ░ │
     *   │ ░ ═══════════════════════════════ ░ │  <- horizontal divider (white)
     *   │ ░ ██░░██░░██░░██░░██░░██░░██░░██░░░ │  <- bottom half: checkerboard
     *   │ ░ ░░██░░██░░██░░██░░██░░██░░██░░██░ │     of 8x8 NES pixels (16x16 HDMI
     *   │ ░ ██░░██░░██░░██░░██░░██░░██░░██░░░ │     pixels after 2x scaling)
     *   │ ░                                ░ │
     *   └─────────────────────────────────────┘
     *
     * The 1-px white BORDER around the entire 256x240 frame is the most
     * useful single diagnostic — if you don't see all four edges, the
     * sync/timing is wrong.  If only horizontal edges are missing, V
     * timing is off.  If only vertical edges, H timing.  If corners are
     * cut, scaling is wrong.
     *
     * The CHECKERBOARD in the bottom half makes pixel-perfect alignment
     * visible — uniform squares means 2x scaling is correct, distorted
     * squares mean the pixel-doubling code has a bug.
     *
     * The COLOUR BARS in the top half let you verify each TMDS lane is
     * decoding correctly.  Swapped lanes show up as wrong-colour bars in
     * a predictable pattern (yellow becomes cyan, etc.).
     */

    /* NES palette indices for clear, distinct test colours.  These are
     * the closest NES palette matches to white/primaries/secondaries —
     * not perfectly pure R/G/B because the NES palette doesn't have
     * those, but distinctive enough to spot lane swaps. */
    static const uint8_t k_bar_colours[8] = {
        0x30, /* white     — all three lanes maximum */
        0x28, /* yellow    — R + G high, B low */
        0x2C, /* cyan      — G + B high, R low */
        0x2A, /* green     — G high only */
        0x24, /* magenta   — R + B high, G low */
        0x16, /* red       — R high only */
        0x21, /* blue      — B high only */
        0x0F, /* black     — all three lanes minimum */
    };
    const uint8_t border_colour = 0x30; /* white */
    const uint8_t checker_a = 0x30;     /* white */
    const uint8_t checker_b = 0x0F;     /* black */

    const uint32_t mid_y = NES_FRAME_HEIGHT / 2u;
    const uint32_t checker_size = 8u; /* 8x8 NES pixels = 16x16 HDMI pixels */

    for (uint32_t y = 0; y < NES_FRAME_HEIGHT; ++y) {
        uint8_t *row = &s_present_indexed[y * NES_FRAME_WIDTH];
        for (uint32_t x = 0; x < NES_FRAME_WIDTH; ++x) {
            /* 1-pixel border on all four edges. */
            if (y == 0 || y == NES_FRAME_HEIGHT - 1u ||
                x == 0 || x == NES_FRAME_WIDTH - 1u) {
                row[x] = border_colour;
                continue;
            }

            /* Horizontal divider at the midline. */
            if (y == mid_y) {
                row[x] = border_colour;
                continue;
            }

            if (y < mid_y) {
                /* Top half: colour bars. */
                row[x] = k_bar_colours[(x * 8u) / NES_FRAME_WIDTH];
            } else {
                /* Bottom half: checkerboard. */
                uint32_t cell_x = x / checker_size;
                uint32_t cell_y = (y - mid_y - 1u) / checker_size;
                row[x] = ((cell_x ^ cell_y) & 1u) ? checker_a : checker_b;
            }
        }
    }
    s_present_valid = true;
}

void video_hstx_get_stats(VideoHstxStats *stats_out) {
    if (stats_out == NULL) return;
    *stats_out = s_stats;
}
