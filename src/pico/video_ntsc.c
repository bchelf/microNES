/*
 * video_ntsc.c  —  4-bit binary-weighted composite NTSC video output
 *
 * System clock : 315 MHz  (set by main() via set_sys_clock_pll before calling
 *                           video_ntsc_init)
 * Sample rate  : 315 MHz / 22 = 14.318182 MHz  (NTSC subcarrier × 4)
 * Frame rate   : NES scanlines are 341 PPU dots.  At 4FSC, each PPU dot is
 *                8/3 samples, so the exact scanline average would be
 *                909.333... samples.  For visual stability this path emits
 *                a constant 909-sample line:
 *                14.3181818 MHz / (909 × 262) = 60.1205 Hz.
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
 * Full-width line template (909 samples at 14.318 MHz = 63.49 µs):
 *   Sync tip         : samples   0–46   (47 samp, DAC=0, sync gate HIGH)
 *   Back porch       : samples  47–71   (25 samp, DAC=blank)
 *   Colorburst       : samples  72–111  (40 samp, 10 subcarrier cycles)
 *   Remaining porch  : samples 112–115  ( 4 samp, DAC=blank)
 *   Active video     : samples 116–873  (758 samp, luma+chroma)
 *   Front porch      : samples 874–908  (35 samp, DAC=blank)
 *
 * DMA ping-pong: channels A and B, each 909 words (= one scanline buffer).
 *   A chains→B, B chains→A.
 *   Core 0 IRQ_0: lightweight re-arm for test-pattern / fallback mode.
 *   Core 1 IRQ_1: full render-and-re-arm for emulator mode.
 *
 * Sync clamp gate: emitted by the PIO as bit 4 of each 5-bit sample symbol,
 *   so horizontal sync edges are sample-accurate and do not depend on IRQ
 *   latency or core scheduling.
 */

#include "clock_config.h"
#include "video_ntsc.h"
#include "video_ntsc.pio.h"
#include "core1_video.h"
#include "scanline_queue.h"

#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#if MICRONES_VIDEO_SYNC_GPIO != (MICRONES_VIDEO_PIN_BASE + MICRONES_VIDEO_PIN_COUNT)
#error "video_ntsc PIO sync output requires the sync gate pin immediately after the DAC pins"
#endif

/* =========================================================================
 * NTSC color encoding
 *
 * The 4-bit DAC has only 16 levels (0-15), so color fidelity is limited.
 * Three cooperating tables control the final composite waveform:
 *
 *   k_burst_pattern[4]  — colorburst reference signal (180° cosine, ±3)
 *   chroma_lut[13][4]   — per-hue chroma modulation (built by build_chroma_lut)
 *   k_chroma_scale[4]   — per-brightness-row attenuation (applied in precompute)
 *
 * The TV's ACC (automatic color control) loop locks to the burst amplitude
 * and uses it as the gain reference for decoding active-video chroma.
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * Colorburst pattern — 180° cosine, amplitude ±3 DAC codes around blank (4)
 *
 *   Phase 0 (0°)  : cos(0°+180°)  = −1 → 4−3 = 1
 *   Phase 1 (90°) : cos(90°+180°) =  0 → 4
 *   Phase 2 (180°): cos(180°+180°)= +1 → 4+3 = 7
 *   Phase 3 (270°): cos(270°+180°)=  0 → 4
 *
 * Indexed by (absolute_sample_index & 3).  Burst begins at sample 72;
 * 72 & 3 = 0 → first burst sample = code 1 (peak-negative). ✓
 * ------------------------------------------------------------------------- */
static const uint8_t k_burst_pattern[4] = { 1, 4, 7, 4 };

/* -------------------------------------------------------------------------
 * Chroma LUT  [hue 0-12][subcarrier phase 0-3]
 *
 * value = roundf(k_hue_amp[hue] × cos(s × π/2 + (hue-2) × 30° × π/180))
 *
 * Phase alignment:
 *   The (hue-2) term aligns NES hue 8 with the 180° colorburst.  The NES
 *   PPU generates burst at the same phase as hue 8 (the yellow/olive column),
 *   so hue 8 must map to 180°.  (hue-2)*30° gives hue 8 = 6×30° = 180°. ✓
 *
 * Per-hue amplitude (k_hue_amp[]):
 *   Base amplitude 3.0 → ±3 DAC codes peak (±~222 mV at the TV, 74 mV/LSB).
 *   Hues 5-7 (red, red-orange, orange) are boosted to 4.1 so their
 *   30°-off-axis peaks cross the rounding threshold from ±3 to ±4,
 *   producing richer reds and browns on the 4-bit DAC.
 * ------------------------------------------------------------------------- */
static int8_t chroma_lut[13][4];

/* Per-hue chroma amplitude.  Most hues use 3.0; red/orange hues are boosted. */
/*                         grey  1     2     3     4     5     6     7     8     9    10    11    12 */
static const float k_hue_amp[13] = {
    0.0f, 3.0f, 3.0f, 3.0f, 3.0f, 4.1f, 4.1f, 4.1f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f
};

void build_chroma_lut(void) {
    memset(chroma_lut[0], 0, sizeof(chroma_lut[0]));  /* hue 0 = greyscale */
    for (int hue = 1; hue < 13; hue++) {
        float phase_rad = (float)(hue - 2) * 30.0f * (float)M_PI / 180.0f;
        for (int s = 0; s < 4; s++) {
            float sc = (float)s * (float)M_PI / 2.0f;
            chroma_lut[hue][s] = (int8_t)roundf(k_hue_amp[hue] * cosf(sc + phase_rad));
        }
    }
}

/* =========================================================================
 * Precomputed DAC LUT: dac_lut[color][phase] — built at palette load time.
 *
 * Eliminates per-pixel multiply, divide-by-7, hue bounds-check, and two
 * separate table lookups from the inner render loop.
 *
 * color  : NES palette index 0-63 (bits [5:0] of pixel byte)
 * phase  : subcarrier phase modulo 4.
 * value  : clamped DAC code [0..15]
 * ========================================================================= */
static uint8_t s_dac_lut[64][4];

/* Set to 0 to strip chroma from all pixels (luma-only / greyscale output).
 * Rebuild + reflash to toggle; no other files need changing. */
#define MICRONES_CHROMA_ENABLED 1

/* -------------------------------------------------------------------------
 * Per-brightness-row chroma scaling (numerator / 8).
 *
 * The real NES 2C02 PPU varies its chroma modulation depth by brightness
 * row.  Dark colors have less chroma than medium/bright; row 3 colors are
 * pastels (high luma, very low chroma).  Without this scaling, dark areas
 * appear oversaturated and pastels appear too vivid on the 4-bit DAC.
 *
 *   Row 0 ($0x): dark   — ×6/8  (moderate chroma)
 *   Row 1 ($1x): medium — ×8/8  (full chroma)
 *   Row 2 ($2x): bright — ×8/8  (full chroma)
 *   Row 3 ($3x): pastel — ×3/8  (desaturated, high luma)
 * ------------------------------------------------------------------------- */
static const int k_chroma_scale[4] = { 6, 8, 8, 3 };

/*
 * Build the precomputed DAC lookup table s_dac_lut[64][4].
 *
 * For each NES palette index (0-63) and each of the 4 subcarrier phases,
 * computes the final clamped 4-bit DAC code:
 *
 *   dac = blank + (luma × 9) / 7 + chroma_lut[hue][phase] × row_scale / 8
 *
 * NES palette index bits:
 *   [3:0] = hue  (0=grey, 1-12=color, 13-15=black)
 *   [5:4] = brightness row (0=dark, 1=medium, 2=bright, 3=pastel)
 *
 * Called once at startup from video_ntsc_precompute_palette().
 */
void video_ntsc_precompute_palette(const uint8_t *palette_to_luma, int palette_size) {
    for (int c = 0; c < 64; c++) {
        int luma     = (c < palette_size) ? (int)palette_to_luma[c] : 0;
        int dac_base = (int)VIDEO_DAC_BLANK + (luma * (int)VIDEO_LUMA_SCALE) / 7;
        int hue      = c & 0x0F;
        if (hue > 12) hue = 0;  /* $xD-$xF: no chroma (black) */
        int row      = (c >> 4) & 3;
        for (int phase = 0; phase < 4; phase++) {
#if MICRONES_CHROMA_ENABLED
            int chroma = (int)chroma_lut[hue][phase] * k_chroma_scale[row] / 8;
            int dac = dac_base + chroma;
#else
            int dac = dac_base;   /* chroma disabled — luma only */
#endif
            if (dac < 0)  dac = 0;
            if (dac > 15) dac = 15;
            s_dac_lut[c][phase] = (uint8_t)dac;
        }
    }
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

#define VIDEO_SYMBOL_SYNC_GATE     0x10u

static inline uint32_t video_ntsc_encode_symbol(uint8_t dac_code, bool sync_gate) {
    uint32_t symbol = ((uint32_t)dac_code & 0x0fu) |
                      (sync_gate ? VIDEO_SYMBOL_SYNC_GATE : 0u);
    return symbol << 27;
}

static inline uint32_t video_ntsc_next_subcarrier_phase(uint32_t subcarrier_phase,
                                                       uint32_t line_phase) {
    (void)line_phase;
    return (subcarrier_phase + VIDEO_WORDS_PER_LINE) & 3u;
}

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
 * Packs a full-width 909-sample composite line template into 909 uint32_t words.
 * One 5-bit symbol per word, MSB first: symbol → bits[31:27].
 * ========================================================================= */
static void render_scanline_composite_phase(
    uint32_t *buf,
    const uint8_t *pixels,
    bool active,
    uint32_t subcarrier_phase
) {
#define EMIT(code, sync_gate)                                \
    do {                                                     \
        *buf++ = video_ntsc_encode_symbol((uint8_t)(code), (sync_gate)); \
    } while (0)

    /* Sync (0-46): 47 samples at code 0 with sync gate high */
    for (uint i = 0; i < VIDEO_SYNC_SAMPLES; i++)
        EMIT(VIDEO_DAC_SYNC, true);

    /* Back porch before burst (47-71): 25 samples blank */
    for (uint i = VIDEO_SYNC_SAMPLES; i < VIDEO_BURST_START; i++)
        EMIT(VIDEO_DAC_BLANK, false);

    /* Colorburst (72-111): 40 samples, 180° cosine ±3 around blank */
    for (uint i = 0; i < VIDEO_BURST_SAMPLES; i++)
        EMIT(k_burst_pattern[(subcarrier_phase + VIDEO_BURST_START + i) & 3u], false);

    /* Remaining back porch (112-115): 4 samples blank */
    for (uint abs_s = VIDEO_BURST_START + VIDEO_BURST_SAMPLES;
         abs_s < VIDEO_ACTIVE_START; abs_s++)
        EMIT(VIDEO_DAC_BLANK, false);

    /* Active video (116-873): 758 samples
     *
     * The NES pixel clock is 5.3693 MHz; at 14.318 MHz sample rate each
     * NES dot = 2.667 samples, so 256 pixels occupy ~683 samples.  The
     * remaining 758−683 = 75 samples are blank border, split 37 left / 38
     * right to centre the image within the NTSC active window.
     *
     * Subcarrier phase for the pixel region: the border adds 37 samples
     * to the active-video offset, so the first pixel sample is at absolute
     * sample 116+37 = 153 plus the rolling line-start subcarrier phase.
     */
#define VIDEO_BORDER_LEFT   37u
#define VIDEO_BORDER_RIGHT  38u
#define VIDEO_PIXEL_SAMPLES (VIDEO_ACTIVE_SAMPLES - VIDEO_BORDER_LEFT - VIDEO_BORDER_RIGHT)  /* 683 */

    if (active && pixels != NULL) {
        /* Left border */
        for (uint i = 0; i < VIDEO_BORDER_LEFT; i++)
            EMIT(VIDEO_DAC_BLANK, false);

        /*
         * Map 256 NES pixels across 683 active samples using fixed-point
         * integer scaling.  DAC code looked up directly from s_dac_lut.
         *   pixel_inc = (256 << 16) / 683 = 24563  (≈256/683, 16.16 fixed)
         * Phase: first pixel sample is at absolute sample 153 plus the rolling
         * line-start subcarrier phase.
         */
        uint32_t       pixel_fp  = 0u;
        const uint32_t pixel_inc = (256u << 16u) / VIDEO_PIXEL_SAMPLES;

        for (uint s = 0; s < VIDEO_PIXEL_SAMPLES; s++) {
            uint pixel_idx = pixel_fp >> 16u;
            if (pixel_idx >= 256u) pixel_idx = 255u;
            pixel_fp += pixel_inc;
            EMIT(s_dac_lut[pixels[pixel_idx] & 0x3Fu]
                 [(subcarrier_phase + VIDEO_BORDER_LEFT + s) & 3u], false);
        }

        /* Right border */
        for (uint i = 0; i < VIDEO_BORDER_RIGHT; i++)
            EMIT(VIDEO_DAC_BLANK, false);
    } else {
        for (uint s = 0; s < VIDEO_ACTIVE_SAMPLES; s++)
            EMIT(VIDEO_DAC_BLANK, false);
    }

    /* Front porch (874-908): 35 samples blank */
    for (uint i = 0; i < 35u; i++)
        EMIT(VIDEO_DAC_BLANK, false);

#undef EMIT
}

void render_scanline_composite(uint32_t *buf, const uint8_t *pixels, bool active) {
    render_scanline_composite_phase(buf, pixels, active, 0u);
}

/* =========================================================================
 * Private scanline renderers
 * ========================================================================= */

static void render_vsync_line(uint32_t *buf) {
    /* All-sync: DAC code 0 and sync gate high for the entire line. */
    for (uint i = 0; i < VIDEO_WORDS_PER_LINE; ++i) {
        buf[i] = video_ntsc_encode_symbol(VIDEO_DAC_SYNC, true);
    }
}

static void render_blank_line(uint32_t *buf) {
    render_scanline_composite(buf, NULL, false);
}

static void render_test_scanline(uint32_t *buf) {
    /* 8 luma bars across the active region — no Core 1 needed */
    static const uint8_t bar_codes[8] = { 13, 11, 9, 7, 6, 5, 4, 4 };

#define EMIT_T(c) \
    do { \
        *buf++ = video_ntsc_encode_symbol((uint8_t)(c), false); \
    } while (0)

#define EMIT_T_SYNC(c) \
    do { \
        *buf++ = video_ntsc_encode_symbol((uint8_t)(c), true); \
    } while (0)

    for (uint i = 0; i < VIDEO_SYNC_SAMPLES; i++)
        EMIT_T_SYNC(VIDEO_DAC_SYNC);
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
    for (uint i = 0; i < 35u; i++)
        EMIT_T(VIDEO_DAC_BLANK);

#undef EMIT_T
#undef EMIT_T_SYNC
}

static void render_ntsc_line(
    int ntsc_line,
    uint32_t *buf,
    ScanlineQueue *queue,
    uint32_t subcarrier_phase
) {
    if (ntsc_line < (int)VIDEO_VSYNC_LINES) {
        render_vsync_line(buf);
    } else if (ntsc_line < (int)(VIDEO_VSYNC_LINES + VIDEO_TOP_BLANK_LINES) ||
               ntsc_line >= (int)VIDEO_ACTIVE_END_LINE) {
        render_scanline_composite_phase(buf, NULL, false, subcarrier_phase);
    } else {
        int active_y = ntsc_line - (int)VIDEO_ACTIVE_START_LINE;
        ScanlineQueueSlot slot;
        scanline_queue_pop_blocking(queue, &slot);
        render_scanline_composite_phase(buf, slot.pixels, true, subcarrier_phase);
        if (active_y == (int)VIDEO_ACTIVE_LINES - 1) {
            s_frames_rendered++;
        }
    }
}

/* =========================================================================
 * DMA helpers
 * ========================================================================= */

static void dma_rearm_words(int buf_idx, uint32_t transfer_words) {
    /*
     * Reset READ_ADDR and TRANS_COUNT without triggering.  The channel will
     * be triggered automatically when the other channel chains to it.
     */
    dma_channel_configure(
        s_dma_chan[buf_idx],
        &s_dma_cfg[buf_idx],
        &s_pio->txf[s_sm],       /* write addr: PIO TX FIFO        */
        scanline_buf[buf_idx],   /* read addr:  start of this buf  */
        transfer_words,          /* transfer count                 */
        false                    /* do not trigger                 */
    );
}

static void dma_rearm(int buf_idx) {
    dma_rearm_words(buf_idx, VIDEO_WORDS_PER_LINE);
}

static void dma_abort_pair_cleanly(void) {
    /* RP2350-E5: clear EN on chained channels before aborting, otherwise an
     * abort can retrigger the partner and leave the pair half-running. */
    hw_clear_bits(&dma_hw->ch[s_dma_chan[0]].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_clear_bits(&dma_hw->ch[s_dma_chan[1]].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_channel_abort(s_dma_chan[0]);
    dma_channel_abort(s_dma_chan[1]);
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
static volatile uint32_t s_irq1_fire_count = 0;

static void __isr dma_irq1_handler(void) {
    /* The DMA chain has already started the next channel.  The PIO sample
     * stream carries the sync-gate bit, so the ISR only records which buffer
     * is idle. */
    s_irq1_fire_count++;

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

    /* Load PIO program and configure state machine */
    s_pio        = pio0;
    s_sm         = 0;
    s_pio_offset = pio_add_program(s_pio, &video_ntsc_program);
    video_ntsc_program_init(s_pio, s_sm, s_pio_offset, MICRONES_VIDEO_PIN_BASE,
                            MICRONES_PIO_CLKDIV);

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
    dma_channel_start(s_dma_chan[0]);        /* A starts, chains to B, B to A… */
}

void video_ntsc_stop(void) {
    uint32_t ch_mask = (1u << s_dma_chan[0]) | (1u << s_dma_chan[1]);

    irq_set_enabled(DMA_IRQ_0, false);
    irq_set_enabled(DMA_IRQ_1, false);
    dma_set_irq0_channel_mask_enabled(ch_mask, false);
    dma_set_irq1_channel_mask_enabled(ch_mask, false);
    dma_hw->ints0 = ch_mask;
    dma_hw->ints1 = ch_mask;

    dma_abort_pair_cleanly();

    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_clear_fifos(s_pio, s_sm);
    pio_sm_restart(s_pio, s_sm);
    s_dma_irq1_pending = false;
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

    multicore_lockout_victim_init();

    /*
     * Hand DMA IRQ ownership to Core 1:
     *   • Disable IRQ_0 for our channels (Core 0 handler stops firing).
     *   • Register and enable IRQ_1 on Core 1's NVIC.
    */
    dma_set_irq0_channel_mask_enabled(ch_mask, false);
    dma_set_irq1_channel_mask_enabled(ch_mask, false);

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
    video_ntsc_program_init(s_pio, s_sm, s_pio_offset, MICRONES_VIDEO_PIN_BASE,
                            MICRONES_PIO_CLKDIV);
    pio_sm_exec(s_pio, s_sm, pio_encode_jmp(s_pio_offset));

    dma_abort_pair_cleanly();

    /* Render lines 0 and 1 (both vsync: all code 0) */
    render_vsync_line(scanline_buf[0]);
    render_vsync_line(scanline_buf[1]);

    /* Re-arm both channels with fresh vsync data. */
    uint32_t display_line_phase = 1u;
    uint32_t render_line_phase  = 2u;
    uint32_t display_subcarrier_phase = video_ntsc_next_subcarrier_phase(0u, 0u);
    uint32_t render_subcarrier_phase =
        video_ntsc_next_subcarrier_phase(display_subcarrier_phase, display_line_phase);
    dma_rearm(0);
    dma_rearm(1);

    /* Clear any stale IRQ_1 flags */
    dma_hw->ints1      = ch_mask;
    s_dma_irq1_pending = false;
    dma_set_irq1_channel_mask_enabled(ch_mask, true);

    /* Start channel A; the PIO sample stream carries the sync-gate state. */
    pio_sm_set_enabled(s_pio, s_sm, true);
    dma_channel_start(s_dma_chan[0]);

    /*
     * Pipeline state after start:
     *   channel A is running buf[0] = line 0
     *   channel B is pre-armed with buf[1] = line 1
     *   display_line       = 1  (will be output when channel A completes)
     *   render_line        = 2  (next to render)
     *   display/render_subcarrier_phase track each line's 4FSC start phase.
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
         *   • s_idle_buf is the buffer whose channel just finished.
         *   • The other channel is already clocking out display_line.
         */
        /* ---- Render render_line into the now-idle buffer ---- */
        render_ntsc_line(render_line, scanline_buf[s_idle_buf], queue,
                         render_subcarrier_phase);

        /*
         * Re-arm the idle channel.  It will be triggered by chain when the
         * currently-active channel finishes (~20,000 cycles from now).
         * We have already rendered the new data, so the chain is safe.
         */
        dma_rearm(s_idle_buf);

        /* ---- Advance pipeline counters ---- */
        display_line++;
        if (display_line >= (int)VIDEO_LINES_PER_FRAME) display_line = 0;
        display_subcarrier_phase =
            video_ntsc_next_subcarrier_phase(display_subcarrier_phase,
                                             display_line_phase);
        display_line_phase++;
        if (display_line_phase >= 3u) display_line_phase = 0u;

        render_line++;
        if (render_line >= (int)VIDEO_LINES_PER_FRAME) render_line = 0;
        render_subcarrier_phase =
            video_ntsc_next_subcarrier_phase(render_subcarrier_phase,
                                             render_line_phase);
        render_line_phase++;
        if (render_line_phase >= 3u) render_line_phase = 0u;
    }
}
