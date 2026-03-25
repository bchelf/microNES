/*
 * video_ntsc.c  —  4-bit binary-weighted composite NTSC video output
 *
 * System clock : 315 MHz  (set by main() via set_sys_clock_pll before calling
 *                           video_ntsc_init)
 * Sample rate  : 315 MHz / 22 = 14.318182 MHz  (NTSC subcarrier × 4)
 * Frame rate   : 910 samp/line × 22 cyc/samp × 262 lines = 5,245,240 cyc/frame
 *                315,000,000 / 5,245,240 = 60.054 Hz  ✓
 *
 * Analytically derived DAC codes
 * (R0=1000Ω, R1=485Ω, R2=242Ω, R3=120Ω, R_series=75.5Ω, R_load=75Ω,
 *  no R_bias at summing node):
 *
 *   G_total = 1/1000 + 1/485 + 1/242 + 1/120 + 1/150.5 = 0.022172 S
 *   V_at_TV(code N) = 3.3 × Σ(bit_i × G_i) / G_total × 75/150.5
 *
 *   code  4 (0100): 0.306 V  →  blank_code = 4   (NTSC blank ≈ 0.286 V)
 *   code 13 (1101): 0.998 V  →  white_code = 13  (NTSC white ≈ 1.000 V)
 *   luma_scale = 9  →  dac = 4 + (luma × 9) / 7,  luma ∈ [0..7]
 *   effective LSB ≈ 74 mV/code
 *
 * Line structure (910 samples at 14.318 MHz = 63.56 µs):
 *   Sync tip         : samples   0–46   (47 samp, DAC=0, GP4=HIGH)
 *   Back porch       : samples  47–71   (25 samp, DAC=blank)
 *   Colorburst       : samples  72–111  (40 samp, 180° cosine ±2 around blank)
 *   Remaining porch  : samples 112–115  ( 4 samp, DAC=blank)
 *   Active video     : samples 116–873  (758 samp, luma+chroma)
 *   Front porch      : samples 874–909  (36 samp, DAC=blank)
 *   Padding          : 2 extra nibbles  (fill to 912 = 114 words × 8 nibbles)
 *   Total valid      : 910 samples  ✓
 *
 * DMA ping-pong: channels A and B, each 114 words (= one scanline buffer).
 *   A chains→B, B chains→A.
 *   Core 0 IRQ_0: lightweight re-arm for test-pattern / fallback mode.
 *   Core 1 IRQ_1: full render-and-re-arm for emulator mode.
 *
 * GP4 (sync clamp MOSFET): raised by IRQ1 handler at each line transition;
 *   lowered by Core 1 after busy_wait_at_least_cycles(1000) ≈ 3.18 µs
 *   (target = 47 × 22 = 1034 cycles; error < 1 sample = 22 cycles).
 */

#include "video_ntsc.h"
#include "video_ntsc.pio.h"
#include "core1_video.h"
#include "scanline_queue.h"

#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#include <math.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 * Colorburst pattern
 *
 * 180° cosine, amplitude ±2 DAC codes around blank (code 4):
 *   Phase 0 (0°)  : cos(0°+180°)  = −1 → 4−2 = 2
 *   Phase 1 (90°) : cos(90°+180°) =  0 → 4
 *   Phase 2 (180°): cos(180°+180°)= +1 → 4+2 = 6
 *   Phase 3 (270°): cos(270°+180°)=  0 → 4
 *
 * Index by (absolute_sample_index & 3).  Burst begins at sample 72;
 * 72 & 3 = 0 → first burst sample = code 2 (peak-negative). ✓
 * ========================================================================= */
static const uint8_t k_burst_pattern[4] = { 2, 4, 6, 4 };

/* =========================================================================
 * Chroma LUT  [hue 0-12][subcarrier phase 0-3]
 *
 * value = roundf(2.5 × cos(s × π/2 + (hue-1) × 30° × π/180))
 * ========================================================================= */
static int8_t chroma_lut[13][4];

void build_chroma_lut(void) {
    memset(chroma_lut[0], 0, sizeof(chroma_lut[0]));  /* hue 0 = greyscale */
    for (int hue = 1; hue < 13; hue++) {
        float phase_rad = (float)(hue - 1) * 30.0f * (float)M_PI / 180.0f;
        for (int s = 0; s < 4; s++) {
            float sc = (float)s * (float)M_PI / 2.0f;
            chroma_lut[hue][s] = (int8_t)roundf(2.5f * cosf(sc + phase_rad));
        }
    }
}

/* =========================================================================
 * Luma lookup table (stored from video_ntsc_precompute_palette)
 * ========================================================================= */
static const uint8_t *s_nes_luma      = NULL;
static int            s_luma_pal_size = 0;

void video_ntsc_precompute_palette(const uint8_t *palette_to_luma, int palette_size) {
    s_nes_luma      = palette_to_luma;
    s_luma_pal_size = palette_size;
}

/* =========================================================================
 * Scanline buffers — fixed ping-pong assignment:
 *   DMA channel 0 (A) always reads from scanline_buf[0]
 *   DMA channel 1 (B) always reads from scanline_buf[1]
 * ========================================================================= */
static uint32_t __attribute__((aligned(4))) scanline_buf[2][VIDEO_WORDS_PER_LINE];

/* =========================================================================
 * PIO / DMA
 * ========================================================================= */
static PIO  s_pio;
static uint s_sm;
static uint s_pio_offset;
static uint s_dma_chan[2];
static dma_channel_config s_dma_cfg[2];

/* =========================================================================
 * State shared between IRQ handler and Core 1 loop
 * ========================================================================= */
static volatile bool s_dma_irq1_pending = false;
static volatile int  s_idle_buf         = 0;

/* =========================================================================
 * Performance counters
 * ========================================================================= */
static volatile uint64_t s_frames_rendered    = 0;
static volatile uint64_t s_swap_wait_us_total = 0;
static volatile uint64_t s_swap_wait_us_max   = 0;

/* =========================================================================
 * Test-pattern flag (set by video_ntsc_build_test_pattern_frame)
 * ========================================================================= */
static bool s_test_pattern_filled = false;

/* =========================================================================
 * render_scanline_composite()
 *
 * Packs 910 NTSC samples + 2 padding nibbles into 114 uint32_t words.
 * 8 nibbles per word, MSB first: nibble 0 → bits[31:28], nibble 7 → bits[3:0].
 * ========================================================================= */
void render_scanline_composite(uint32_t *buf, const uint8_t *pixels, bool active) {
    uint32_t word      = 0;
    int      nib_count = 0;

#define EMIT(code)                                           \
    do {                                                     \
        word = (word << 4) | ((uint32_t)(code) & 0xFu);     \
        if (++nib_count == 8) {                              \
            *buf++ = word;                                   \
            word = 0; nib_count = 0;                         \
        }                                                    \
    } while (0)

    /* Sync (0-46): 47 samples at code 0 */
    for (uint i = 0; i < VIDEO_SYNC_SAMPLES; i++)
        EMIT(VIDEO_DAC_SYNC);

    /* Back porch before burst (47-71): 25 samples blank */
    for (uint i = VIDEO_SYNC_SAMPLES; i < VIDEO_BURST_START; i++)
        EMIT(VIDEO_DAC_BLANK);

    /* Colorburst (72-111): 40 samples, 180° cosine ±2 around blank */
    for (uint i = 0; i < VIDEO_BURST_SAMPLES; i++)
        EMIT(k_burst_pattern[(VIDEO_BURST_START + i) & 3u]);

    /* Remaining back porch (112-115): 4 samples blank */
    for (uint abs_s = VIDEO_BURST_START + VIDEO_BURST_SAMPLES;
         abs_s < VIDEO_ACTIVE_START; abs_s++)
        EMIT(VIDEO_DAC_BLANK);

    /* Active video (116-873): 758 samples */
    if (active && pixels != NULL && s_nes_luma != NULL) {
        /*
         * Map 256 NES pixels across 758 active samples using fixed-point
         * integer scaling.
         *   pixel_inc = (256 << 16) / 758 = 17644  (≈256/758, 16.16 fixed)
         */
        uint32_t       pixel_fp  = 0u;
        const uint32_t pixel_inc = (256u << 16u) / VIDEO_ACTIVE_SAMPLES;

        for (uint s = 0; s < VIDEO_ACTIVE_SAMPLES; s++) {
            uint pixel_idx = pixel_fp >> 16u;
            if (pixel_idx >= 256u) pixel_idx = 255u;
            pixel_fp += pixel_inc;

            uint8_t color = pixels[pixel_idx] & 0x3Fu;

            /* Luma → DAC code: dac = blank + (luma × 9) / 7 */
            int luma = (color < (uint8_t)s_luma_pal_size) ?
                       (int)s_nes_luma[color] : 0;
            int dac  = (int)VIDEO_DAC_BLANK + (luma * (int)VIDEO_LUMA_SCALE) / 7;

            /* Chroma offset from LUT */
            uint hue = (uint)color & 0x0Fu;
            if (hue > 12u) hue = 0u;
            dac += (int)chroma_lut[hue][(VIDEO_ACTIVE_START + s) & 3u];

            /* Clamp to 0-15 */
            if (dac < 0)  dac = 0;
            if (dac > 15) dac = 15;

            EMIT((uint)dac);
        }
    } else {
        for (uint s = 0; s < VIDEO_ACTIVE_SAMPLES; s++)
            EMIT(VIDEO_DAC_BLANK);
    }

    /* Front porch (874-909): 36 samples blank */
    for (uint i = 0; i < 36u; i++)
        EMIT(VIDEO_DAC_BLANK);

    /* Padding nibbles 910-911: fill last word to 114 × 8 = 912 nibbles */
    EMIT(VIDEO_DAC_BLANK);
    EMIT(VIDEO_DAC_BLANK);

#undef EMIT
}

/* =========================================================================
 * Private scanline renderers
 * ========================================================================= */

static void render_vsync_line(uint32_t *buf) {
    /* All-sync: DAC code 0 for entire line.  GP4 stays HIGH (Core 1). */
    memset(buf, 0, VIDEO_WORDS_PER_LINE * sizeof(uint32_t));
}

static void render_blank_line(uint32_t *buf) {
    render_scanline_composite(buf, NULL, false);
}

static void render_test_scanline(uint32_t *buf) {
    /* 8 luma bars across the active region — no Core 1 needed */
    static const uint8_t bar_codes[8] = { 13, 11, 9, 7, 6, 5, 4, 4 };

    uint32_t word      = 0;
    int      nib_count = 0;

#define EMIT_T(c) \
    do { \
        word = (word << 4) | ((uint32_t)(c) & 0xFu); \
        if (++nib_count == 8) { *buf++ = word; word = 0; nib_count = 0; } \
    } while (0)

    for (uint i = 0; i < VIDEO_SYNC_SAMPLES; i++)
        EMIT_T(VIDEO_DAC_SYNC);
    for (uint i = VIDEO_SYNC_SAMPLES; i < VIDEO_BURST_START; i++)
        EMIT_T(VIDEO_DAC_BLANK);
    for (uint i = 0; i < VIDEO_BURST_SAMPLES; i++)
        EMIT_T(k_burst_pattern[(VIDEO_BURST_START + i) & 3u]);
    for (uint abs_s = VIDEO_BURST_START + VIDEO_BURST_SAMPLES;
         abs_s < VIDEO_ACTIVE_START; abs_s++)
        EMIT_T(VIDEO_DAC_BLANK);
    for (uint s = 0; s < VIDEO_ACTIVE_SAMPLES; s++) {
        uint bar = (s * 8u) / VIDEO_ACTIVE_SAMPLES;
        EMIT_T(bar_codes[bar]);
    }
    for (uint i = 0; i < 38u; i++)  /* 36 front porch + 2 padding */
        EMIT_T(VIDEO_DAC_BLANK);

#undef EMIT_T
}

static void render_ntsc_line(int ntsc_line, uint32_t *buf, ScanlineQueue *queue) {
    if (ntsc_line < (int)VIDEO_VSYNC_LINES) {
        render_vsync_line(buf);
    } else if (ntsc_line < (int)(VIDEO_VSYNC_LINES + VIDEO_TOP_BLANK_LINES) ||
               ntsc_line >= (int)VIDEO_ACTIVE_END_LINE) {
        render_blank_line(buf);
    } else {
        int active_y = ntsc_line - (int)VIDEO_ACTIVE_START_LINE;
        ScanlineQueueSlot slot;
        scanline_queue_pop_blocking(queue, &slot);
        render_scanline_composite(buf, slot.pixels, true);
        if (active_y == (int)VIDEO_ACTIVE_LINES - 1) {
            s_frames_rendered++;
        }
    }
}

/* =========================================================================
 * DMA helpers
 * ========================================================================= */

static void dma_rearm(int buf_idx) {
    /*
     * Reset READ_ADDR and TRANS_COUNT without triggering.  The channel will
     * be triggered automatically when the other channel chains to it.
     */
    dma_channel_configure(
        s_dma_chan[buf_idx],
        &s_dma_cfg[buf_idx],
        &s_pio->txf[s_sm],       /* write addr: PIO TX FIFO        */
        scanline_buf[buf_idx],   /* read addr:  start of this buf  */
        VIDEO_WORDS_PER_LINE,    /* transfer count                 */
        false                    /* do not trigger                 */
    );
}

static void setup_dma(void) {
    s_dma_chan[0] = dma_claim_unused_channel(true);
    s_dma_chan[1] = dma_claim_unused_channel(true);

    uint dreq = pio_get_dreq(s_pio, s_sm, /*is_tx=*/true);

    /* ---- Channel A: buf[0] → PIO FIFO, chain → B ---- */
    s_dma_cfg[0] = dma_channel_get_default_config(s_dma_chan[0]);
    channel_config_set_transfer_data_size(&s_dma_cfg[0], DMA_SIZE_32);
    channel_config_set_read_increment(&s_dma_cfg[0],  true);
    channel_config_set_write_increment(&s_dma_cfg[0], false);
    channel_config_set_dreq(&s_dma_cfg[0], dreq);
    channel_config_set_chain_to(&s_dma_cfg[0], s_dma_chan[1]);
    channel_config_set_irq_quiet(&s_dma_cfg[0], false);

    dma_channel_configure(s_dma_chan[0], &s_dma_cfg[0],
        &s_pio->txf[s_sm], scanline_buf[0], VIDEO_WORDS_PER_LINE, false);

    /* ---- Channel B: buf[1] → PIO FIFO, chain → A ---- */
    s_dma_cfg[1] = dma_channel_get_default_config(s_dma_chan[1]);
    channel_config_set_transfer_data_size(&s_dma_cfg[1], DMA_SIZE_32);
    channel_config_set_read_increment(&s_dma_cfg[1],  true);
    channel_config_set_write_increment(&s_dma_cfg[1], false);
    channel_config_set_dreq(&s_dma_cfg[1], dreq);
    channel_config_set_chain_to(&s_dma_cfg[1], s_dma_chan[0]);
    channel_config_set_irq_quiet(&s_dma_cfg[1], false);

    dma_channel_configure(s_dma_chan[1], &s_dma_cfg[1],
        &s_pio->txf[s_sm], scanline_buf[1], VIDEO_WORDS_PER_LINE, false);

    /*
     * Enable both IRQ lines for both channels:
     *   DMA_IRQ_0 → Core 0 handler (lightweight test-pattern re-arm)
     *   DMA_IRQ_1 → Core 1 handler (full render loop)
     * Core 1 entry will disable IRQ_0 for these channels once it takes over.
     */
    uint32_t mask = (1u << s_dma_chan[0]) | (1u << s_dma_chan[1]);
    dma_set_irq0_channel_mask_enabled(mask, true);
    dma_set_irq1_channel_mask_enabled(mask, true);
}

/* =========================================================================
 * Core 0  DMA IRQ_0  handler  (test-pattern / fallback re-arm)
 * ========================================================================= */
static void __isr dma_irq0_handler(void) {
    uint32_t mask   = (1u << s_dma_chan[0]) | (1u << s_dma_chan[1]);
    uint32_t status = dma_hw->ints0 & mask;
    dma_hw->ints0   = status;   /* clear all triggered bits in one write */
    if (status & (1u << s_dma_chan[0])) dma_rearm(0);
    if (status & (1u << s_dma_chan[1])) dma_rearm(1);
}

/* =========================================================================
 * Core 1  DMA IRQ_1  handler  (emulator render loop)
 * ========================================================================= */
static void __isr dma_irq1_handler(void) {
    /*
     * The DMA chain has already started the next channel.  Raise GP4
     * immediately for the new line's horizontal sync pulse.
     * IRQ latency on Cortex-M33 ≈ 12 cycles; error < 1 sample (22 cycles).
     */
    gpio_put(MICRONES_VIDEO_SYNC_GPIO, 1);

    uint32_t mask   = (1u << s_dma_chan[0]) | (1u << s_dma_chan[1]);
    uint32_t status = dma_hw->ints1 & mask;
    dma_hw->ints1   = status;

    s_idle_buf         = (status & (1u << s_dma_chan[0])) ? 0 : 1;
    s_dma_irq1_pending = true;
    /* Core 1 wakes from __wfe() automatically on IRQ return. */
}

/* =========================================================================
 * Public init / control
 * ========================================================================= */

void video_ntsc_init(void) {
    build_chroma_lut();

    /* GP4: sync clamp gate — output, initially LOW (MOSFET off) */
    gpio_init(MICRONES_VIDEO_SYNC_GPIO);
    gpio_set_dir(MICRONES_VIDEO_SYNC_GPIO, GPIO_OUT);
    gpio_put(MICRONES_VIDEO_SYNC_GPIO, 0);

    /* Load PIO program and configure state machine */
    s_pio        = pio0;
    s_sm         = 0;
    s_pio_offset = pio_add_program(s_pio, &video_ntsc_program);
    video_ntsc_program_init(s_pio, s_sm, s_pio_offset, MICRONES_VIDEO_PIN_BASE);

    /* Claim and configure DMA channels */
    setup_dma();

    /* Register Core 0 IRQ_0 handler for the test-pattern / fallback path */
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq0_handler);
    irq_set_enabled(DMA_IRQ_0, true);
}

void video_ntsc_start(void) {
    /*
     * If video_ntsc_build_test_pattern_frame() was called, the buffers already
     * contain the test pattern; otherwise fill them with blank scanlines.
     */
    if (!s_test_pattern_filled) {
        render_blank_line(scanline_buf[0]);
        render_blank_line(scanline_buf[1]);
    }

    pio_sm_set_enabled(s_pio, s_sm, true);
    gpio_put(MICRONES_VIDEO_SYNC_GPIO, 0);   /* GP4 LOW for test-pattern mode */
    dma_channel_start(s_dma_chan[0]);        /* A starts, chains to B, B to A… */
}

void video_ntsc_build_test_pattern_frame(void) {
    render_test_scanline(scanline_buf[0]);
    render_test_scanline(scanline_buf[1]);
    s_test_pattern_filled = true;
}

/* =========================================================================
 * Compatibility stubs  (called by existing core1_video.c — no-ops here)
 * ========================================================================= */

void video_ntsc_begin_frame(void) {}
void video_ntsc_present(void)     {}

void video_ntsc_write_visible_scanline_mono(
    int y, const uint8_t *p, int n)
{ (void)y; (void)p; (void)n; }

void video_ntsc_write_visible_scanline_luma(
    int y, const uint8_t *p, int n)
{ (void)y; (void)p; (void)n; }

void video_ntsc_write_visible_scanline_indexed_luma(
    int y, const uint8_t *p, int n, const uint8_t *lut, int lsz)
{ (void)y; (void)p; (void)n; (void)lut; (void)lsz; }

/* =========================================================================
 * Performance counters
 * ========================================================================= */

void video_ntsc_perf_get(MicronesVideoNtscPerfStats *out) {
    if (!out) return;
    out->begin_frame_calls  = 0;
    out->present_calls      = 0;
    out->swap_wait_events   = s_frames_rendered;
    out->swap_wait_us_total = s_swap_wait_us_total;
    out->swap_wait_us_max   = s_swap_wait_us_max;
}

/* =========================================================================
 * video_ntsc_core1_entry()
 *
 * Called via  multicore_launch_core1(video_ntsc_core1_entry).
 * video_ntsc_init() and video_ntsc_start() must have been called on Core 0
 * and the scanline queue must have been initialised.
 *
 * Responsibilities:
 *   1. Take over DMA from Core 0 (disable IRQ_0, enable IRQ_1 on Core 1).
 *   2. Re-seed both scanline buffers with vsync data.
 *   3. Restart PIO and DMA cleanly.
 *   4. Loop: on each DMA IRQ, manage GP4 timing, render next NTSC line,
 *      re-arm the completed DMA channel.
 *
 * Pipeline: while DMA outputs line N from buf[K], Core 1 renders line N+1
 * into buf[1-K].  Render budget at 315 MHz: ~20,000 cycles/scanline;
 * active-line render takes ~12,000-15,000 cycles (well within budget).
 * ========================================================================= */

void video_ntsc_core1_entry(void) {
    uint32_t ch_mask = (1u << s_dma_chan[0]) | (1u << s_dma_chan[1]);

    /*
     * Hand DMA IRQ ownership to Core 1:
     *   • Disable IRQ_0 for our channels (Core 0 handler stops firing).
     *   • Register and enable IRQ_1 on Core 1's NVIC.
     */
    dma_set_irq0_channel_mask_enabled(ch_mask, false);

    irq_set_exclusive_handler(DMA_IRQ_1, dma_irq1_handler);
    irq_set_enabled(DMA_IRQ_1, true);

    ScanlineQueue *queue = core1_video_get_queue();

    /*
     * Clean restart: stop PIO, drain FIFOs, abort DMA, pre-render vsync
     * data into both buffers, then restart PIO and DMA together.
     */
    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_clear_fifos(s_pio, s_sm);
    pio_sm_restart(s_pio, s_sm);
    pio_sm_exec(s_pio, s_sm, pio_encode_jmp(s_pio_offset));

    dma_channel_abort(s_dma_chan[0]);
    dma_channel_abort(s_dma_chan[1]);

    /* Render lines 0 and 1 (both vsync: all code 0) */
    render_vsync_line(scanline_buf[0]);
    render_vsync_line(scanline_buf[1]);

    /* Re-arm both channels with fresh vsync data */
    dma_rearm(0);
    dma_rearm(1);

    /* Clear any stale IRQ_1 flags */
    dma_hw->ints1      = ch_mask;
    s_dma_irq1_pending = false;

    /* Start: GP4 HIGH for line 0 sync, then start channel A */
    pio_sm_set_enabled(s_pio, s_sm, true);
    gpio_put(MICRONES_VIDEO_SYNC_GPIO, 1);
    dma_channel_start(s_dma_chan[0]);

    /*
     * Pipeline state after start:
     *   channel A is running buf[0] = line 0
     *   channel B is pre-armed with buf[1] = line 1
     *   display_line = 1  (will be output when channel A completes)
     *   render_line  = 2  (next to render)
     */
    int display_line = 1;
    int render_line  = 2;

    while (true) {
        /* ---- Wait for DMA IRQ_1 (fires when a channel completes) ---- */
        while (!s_dma_irq1_pending) {
            __wfe();
        }
        s_dma_irq1_pending = false;

        /*
         * At this point:
         *   • The ISR has set GP4 HIGH for display_line's sync pulse.
         *   • s_idle_buf is the buffer whose channel just finished.
         *   • The other channel is already clocking out display_line.
         *
         * GP4 management:
         *   Vsync lines (0-8): leave GP4 HIGH the entire line (continuous sync).
         *   All other lines: drop GP4 after ~47 sync samples.
         *     Target: 47 × 22 = 1034 cycles HIGH from the line start.
         *     IRQ latency + wakeup ≈ 20-30 cycles; we wait 1000 cycles so
         *     total ≈ 1020-1030 cycles  (error < 1 sample = 22 cycles). ✓
         */
        bool display_is_vsync = (display_line < (int)VIDEO_VSYNC_LINES);
        if (!display_is_vsync) {
            busy_wait_at_least_cycles(1000u);
            gpio_put(MICRONES_VIDEO_SYNC_GPIO, 0);
        }

        /* ---- Render render_line into the now-idle buffer ---- */
        render_ntsc_line(render_line, scanline_buf[s_idle_buf], queue);

        /*
         * Re-arm the idle channel.  It will be triggered by chain when the
         * currently-active channel finishes (~20,000 cycles from now).
         * We have already rendered the new data, so the chain is safe.
         */
        dma_rearm(s_idle_buf);

        /* ---- Advance pipeline counters ---- */
        display_line++;
        if (display_line >= (int)VIDEO_LINES_PER_FRAME) display_line = 0;

        render_line++;
        if (render_line >= (int)VIDEO_LINES_PER_FRAME) render_line = 0;
    }
}
