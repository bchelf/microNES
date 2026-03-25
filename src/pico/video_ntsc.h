#ifndef MICRONES_VIDEO_NTSC_H
#define MICRONES_VIDEO_NTSC_H

#include <stdint.h>
#include <stdbool.h>

/*
 * 4-bit binary-weighted composite DAC — hardware pin assignment
 *
 *   GP0–GP3 : DAC bits 0–3 (driven by PIO state machine 0 on PIO0)
 *   GP4     : sync clamp gate (2N7002 N-MOSFET, active-high, driven by CPU)
 *
 * Resistor network (R-series = 75.5 Ω, R-load = 75 Ω TV termination):
 *   GP0 → 1000 Ω → summing node → 75.5 Ω → RCA jack → 75 Ω
 *   GP1 →  485 Ω → summing node
 *   GP2 →  242 Ω → summing node
 *   GP3 →  120 Ω → summing node
 *
 * Analytically derived DAC codes (no R_bias at summing node):
 *   blank_code  = 4   (306 mV ≈ 300 mV NTSC blank)
 *   white_code  = 13  (998 mV ≈ 1000 mV NTSC white)
 *   luma_scale  = 9   (dac = blank_code + luma*9/7, luma ∈ [0..7])
 *   LSB ≈ 74 mV/code
 */

#define MICRONES_VIDEO_PIN_BASE     0u    /* GP0: first DAC bit */
#define MICRONES_VIDEO_PIN_COUNT    4u    /* GP0-GP3 */
#define MICRONES_VIDEO_SYNC_GPIO    4u    /* GP4: N-MOSFET sync clamp, active HIGH */

/* Visible dimensions (unchanged from original) */
enum {
    MICRONES_VIDEO_VISIBLE_WIDTH  = 256,
    MICRONES_VIDEO_VISIBLE_HEIGHT = 240,
};

/*
 * Luma level enum — 8 levels, compatible with existing emulator_video_adapter.c
 * palette table (k_emulator_video_palette_to_gray).
 */
typedef enum {
    MICRONES_VIDEO_LUMA_BLACK      = 0,
    MICRONES_VIDEO_LUMA_DARK       = 1,
    MICRONES_VIDEO_LUMA_MID_DARK   = 2,
    MICRONES_VIDEO_LUMA_MID        = 3,
    MICRONES_VIDEO_LUMA_MID_BRIGHT = 4,
    MICRONES_VIDEO_LUMA_BRIGHT     = 5,
    MICRONES_VIDEO_LUMA_VERY_BRIGHT= 6,
    MICRONES_VIDEO_LUMA_WHITE      = 7,
} micrones_video_luma_t;

/*
 * Performance counters — fields kept binary-compatible with existing main.c
 * references (swap_wait_us_total, swap_wait_us_max).
 */
typedef struct {
    uint64_t begin_frame_calls;      /* kept for compat; always 0 in new impl */
    uint64_t present_calls;          /* kept for compat; always 0 in new impl */
    uint64_t swap_wait_events;
    uint64_t swap_wait_us_total;
    uint64_t swap_wait_us_max;
} MicronesVideoNtscPerfStats;

/* -----------------------------------------------------------------------
 * Public API — all original signatures preserved for compatibility.
 * ----------------------------------------------------------------------- */

/*
 * Initialise GPIO, load PIO program, claim DMA channels.
 * Call from Core 0 before video_ntsc_start().
 */
void video_ntsc_init(void);

/*
 * Enable PIO state machine, fill scanline buffers with blank, start DMA
 * ping-pong.  For the test-pattern path (no Core 1), a lightweight Core 0
 * DMA-IRQ handler keeps the channels re-armed.
 */
void video_ntsc_start(void);

/*
 * Store the palette-to-luma lookup table used by render_scanline_composite().
 * palette_to_luma[i] must be in [0..7].  palette_size must be ≤ 64.
 * Call from Core 0 before launching Core 1.
 */
void video_ntsc_precompute_palette(const uint8_t *palette_to_luma, int palette_size);

/*
 * No-op stubs — kept so existing core1_video.c compiles unchanged.
 * The new architecture drives timing entirely from the DMA-IRQ loop in Core 1.
 */
void video_ntsc_begin_frame(void);
void video_ntsc_present(void);

/* Scanline-write stubs (called by core1_video.c; no-ops in new architecture). */
void video_ntsc_write_visible_scanline_mono(
    int visible_y, const uint8_t *pixels, int pixel_count);
void video_ntsc_write_visible_scanline_luma(
    int visible_y, const uint8_t *pixels, int pixel_count);
void video_ntsc_write_visible_scanline_indexed_luma(
    int visible_y,
    const uint8_t *pixels,
    int pixel_count,
    const uint8_t *palette_to_luma,
    int palette_size);

/*
 * Render a static test-pattern (colour bars) into both scanline buffers.
 * Call before video_ntsc_start(); the DMA will loop these two buffers.
 */
void video_ntsc_build_test_pattern_frame(void);

/* Performance counters. */
void video_ntsc_perf_get(MicronesVideoNtscPerfStats *stats_out);

/* -----------------------------------------------------------------------
 * New public API
 * ----------------------------------------------------------------------- */

/*
 * Build the 13×4 chroma LUT at startup.
 * Called automatically by video_ntsc_init(); exposed for testing.
 */
void build_chroma_lut(void);

/*
 * Render one complete NTSC scanline (910 valid samples + 2 padding) into buf.
 *
 *   buf    : output buffer, exactly VIDEO_WORDS_PER_LINE (114) uint32_t words.
 *   pixels : 256 NES palette-index bytes for an active line (NULL if blanking).
 *   active : true = active video, false = blank line with normal horizontal sync.
 *
 * Output format: 8 nibbles packed per word, MSB first (nibble 0 in bits 31:28).
 * GP4 sync timing is handled separately by the Core 1 loop.
 */
void render_scanline_composite(uint32_t *buf, const uint8_t *pixels, bool active);

/*
 * Core 1 entry point.
 * Registers DMA IRQ1 on Core 1, then drives the full NTSC frame loop:
 *   — 9 vsync lines (lines 0-8)
 *   — 11 blank lines with burst (lines 9-19)
 *   — 240 active video lines (lines 20-259), pixels from scanline_queue
 *   — 2 bottom-blank lines (lines 260-261)
 *
 * Launched by main() via multicore_launch_core1(video_ntsc_core1_entry).
 * video_ntsc_init() and video_ntsc_start() must have been called first.
 */
void video_ntsc_core1_entry(void);

/* NTSC frame geometry constants */
#define VIDEO_WORDS_PER_LINE     114u   /* 912 nibbles; 910 valid + 2 padding   */
#define VIDEO_LINES_PER_FRAME    262u
#define VIDEO_VSYNC_LINES          9u   /* lines 0-8: pre-eq + vsync + post-eq  */
#define VIDEO_TOP_BLANK_LINES     11u   /* lines 9-19: blank with burst         */
#define VIDEO_ACTIVE_START_LINE   20u   /* first active NTSC line               */
#define VIDEO_ACTIVE_LINES       240u   /* lines 20-259                         */
#define VIDEO_ACTIVE_END_LINE    260u   /* exclusive upper bound                */
#define VIDEO_BOTTOM_BLANK_LINES   2u   /* lines 260-261                        */

/* Horizontal timing (samples, at 14.318182 MHz) */
#define VIDEO_SYNC_SAMPLES        47u   /* samples 0-46    */
#define VIDEO_BURST_START         72u   /* back-porch burst start sample        */
#define VIDEO_BURST_SAMPLES       40u   /* 10 subcarrier cycles × 4 samp/cycle  */
#define VIDEO_ACTIVE_START       116u   /* first active sample                  */
#define VIDEO_ACTIVE_SAMPLES     758u   /* active video samples                 */

/* Analytically derived DAC level constants */
#define VIDEO_DAC_SYNC             0u   /* sync tip: 0.000 V                    */
#define VIDEO_DAC_BLANK            4u   /* blank:    0.306 V ≈ 0.300 V NTSC     */
#define VIDEO_DAC_WHITE           13u   /* white:    0.998 V ≈ 1.000 V NTSC     */
#define VIDEO_LUMA_SCALE           9u   /* dac = BLANK + (luma * 9) / 7         */

#endif /* MICRONES_VIDEO_NTSC_H */
