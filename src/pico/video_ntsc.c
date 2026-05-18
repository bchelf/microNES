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

#include "clock_config.h"
#include "runtime_config.h"
#include "video_ntsc.h"
#include "video_ntsc.pio.h"
#include "core1_video.h"
#include "scanline_queue.h"

#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/clocks.h"
#include "pico/stdlib.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

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
 * phase  : subcarrier phase = sample_index & 3  (VIDEO_ACTIVE_START=116, 116%4=0)
 * value  : clamped DAC code [0..15]
 * ========================================================================= */
static uint8_t s_dac_lut[64][4];

/* Set to 0 to strip chroma from all pixels (luma-only / greyscale output).
 * Overridable from the build, e.g. -DMICRONES_CHROMA_ENABLED=0.
 *
 * Useful as a bisect: if the picture's shear/sync trouble goes away with
 * chroma off, the chroma encoder is producing DAC excursions that the TV
 * mis-reads as sync; if the shear remains, the cause is elsewhere
 * (Core 1 render timing, buffer corruption, etc.). */
#ifndef MICRONES_CHROMA_ENABLED
#define MICRONES_CHROMA_ENABLED 1
#endif

#ifndef MICRONES_ANALOG_VIDEO_DIAG
#define MICRONES_ANALOG_VIDEO_DIAG 0
#endif

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

#if MICRONES_ANALOG_VIDEO_DIAG
    printf("ntsc diag: palette precompute size=%d chroma=%u\n",
           palette_size,
           (unsigned)MICRONES_CHROMA_ENABLED);
    for (int row = 0; row < 4; ++row) {
        printf("ntsc diag: palette row %d:", row);
        for (int hue = 0; hue < 16; ++hue) {
            int c = row * 16 + hue;
            printf(" %02x=%u/%x%x%x%x",
                   c,
                   (unsigned)((c < palette_size) ? palette_to_luma[c] : 0),
                   (unsigned)s_dac_lut[c][0],
                   (unsigned)s_dac_lut[c][1],
                   (unsigned)s_dac_lut[c][2],
                   (unsigned)s_dac_lut[c][3]);
        }
        printf("\n");
    }
#endif
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

/* Diagnostic: build with -DMICRONES_VIDEO_SYNC_CLAMP_DISABLE=1 to stop
 * firing the GP28 MOSFET clamp entirely.  The pin is still configured as
 * output LOW, so the MOSFET is held off and the sync tip falls back to
 * whatever the resistor network produces with all four DAC bits at the
 * GPIO low level (≈ ground for a clean PCB).  Useful for isolating
 * whether a glitch in the clamp drive is causing false mid-line sync
 * triggers — if the picture cleans up with the clamp disabled, the
 * clamp itself (or trace coupling into GP28) is implicated. */
#ifndef MICRONES_VIDEO_SYNC_CLAMP_DISABLE
#define MICRONES_VIDEO_SYNC_CLAMP_DISABLE 0
#endif

#if MICRONES_VIDEO_SYNC_CLAMP_DISABLE
#define MICRONES_SYNC_CLAMP_ENGAGE() ((void)0)
#else
#define MICRONES_SYNC_CLAMP_ENGAGE() gpio_put(MICRONES_VIDEO_SYNC_GPIO, 1)
#endif

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
static volatile uint64_t s_dma_irq0_count     = 0;
static volatile uint64_t s_dma_irq1_count     = 0;
static volatile uint64_t s_lines_rendered     = 0;
static volatile uint64_t s_vsync_lines_rendered = 0;
static volatile uint64_t s_blank_lines_rendered = 0;
static volatile uint64_t s_active_lines_rendered = 0;
static volatile uint64_t s_render_us_total    = 0;
static volatile uint32_t s_render_us_max      = 0;
static volatile uint32_t s_render_over_50us_count = 0;
static volatile uint32_t s_render_over_63us_count = 0;
static volatile uint32_t s_render_over_100us_count = 0;
static volatile uint32_t s_render_us_max_line = 0;
static volatile uint32_t s_render_us_max_active_y = 0xffffffffu;
static volatile uint32_t s_render_us_max_kind = 0;
static volatile uint32_t s_display_line_snapshot = 0;
static volatile uint32_t s_render_line_snapshot = 0;
static volatile uint32_t s_queue_level_snapshot = 0;
static volatile uint32_t s_queue_level_max    = 0;
static volatile uint32_t s_scanline_y_mismatch_count = 0;

static const uint32_t s_probe_y[4] = { 0u, 32u, 120u, 239u };
static volatile uint32_t s_probe_slot_y[4];
static volatile uint32_t s_probe_hash[4];
static volatile uint32_t s_probe_nonblack[4];
static volatile uint8_t s_probe_min[4];
static volatile uint8_t s_probe_max[4];

/* =========================================================================
 * Test-pattern flag (set by video_ntsc_build_test_pattern_frame)
 * ========================================================================= */
static bool s_test_pattern_filled = false;

enum {
    VIDEO_RENDER_KIND_VSYNC = 1u,
    VIDEO_RENDER_KIND_BLANK = 2u,
    VIDEO_RENDER_KIND_ACTIVE = 3u,
};

static uint32_t diag_render_kind_for_line(int ntsc_line) {
    if (ntsc_line < (int)VIDEO_VSYNC_LINES) {
        return VIDEO_RENDER_KIND_VSYNC;
    }
    if (ntsc_line < (int)(VIDEO_VSYNC_LINES + VIDEO_TOP_BLANK_LINES) ||
        ntsc_line >= (int)VIDEO_ACTIVE_END_LINE) {
        return VIDEO_RENDER_KIND_BLANK;
    }
    return VIDEO_RENDER_KIND_ACTIVE;
}

static void diag_record_queue_level(uint32_t level) {
    s_queue_level_snapshot = level;
    if (level > s_queue_level_max) {
        s_queue_level_max = level;
    }
}

static void diag_record_active_scanline(int active_y, const ScanlineQueueSlot *slot) {
    if (slot->y != (uint16_t)active_y) {
        ++s_scanline_y_mismatch_count;
    }

#if MICRONES_ANALOG_VIDEO_DIAG
    int probe = -1;
    for (int i = 0; i < 4; ++i) {
        if ((uint32_t)active_y == s_probe_y[i]) {
            probe = i;
            break;
        }
    }
    if (probe < 0) {
        return;
    }

    uint32_t hash = 2166136261u;
    uint32_t nonblack = 0;
    uint8_t min_pixel = 0xffu;
    uint8_t max_pixel = 0x00u;
    for (int x = 0; x < 256; ++x) {
        uint8_t pixel = slot->pixels[x] & 0x3fu;
        hash ^= pixel;
        hash *= 16777619u;
        if (pixel != 0u) ++nonblack;
        if (pixel < min_pixel) min_pixel = pixel;
        if (pixel > max_pixel) max_pixel = pixel;
    }

    s_probe_slot_y[probe] = slot->y;
    s_probe_hash[probe] = hash;
    s_probe_nonblack[probe] = nonblack;
    s_probe_min[probe] = min_pixel;
    s_probe_max[probe] = max_pixel;
#else
    (void)active_y;
    (void)slot;
#endif
}

/* =========================================================================
 * render_scanline_composite()
 *
 * Packs 910 NTSC samples + 2 padding nibbles into 114 uint32_t words.
 * 8 nibbles per word, MSB first: nibble 0 → bits[31:28], nibble 7 → bits[3:0].
 * ========================================================================= */
void MICRONES_HOT_FUNC(render_scanline_composite)(uint32_t *buf, const uint8_t *pixels, bool active) {
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

    /* Colorburst (72-111): 40 samples, 180° cosine ±3 around blank */
    for (uint i = 0; i < VIDEO_BURST_SAMPLES; i++)
        EMIT(k_burst_pattern[(VIDEO_BURST_START + i) & 3u]);

    /* Remaining back porch (112-115): 4 samples blank */
    for (uint abs_s = VIDEO_BURST_START + VIDEO_BURST_SAMPLES;
         abs_s < VIDEO_ACTIVE_START; abs_s++)
        EMIT(VIDEO_DAC_BLANK);

    /* Active video (116-873): 758 samples
     *
     * The NES pixel clock is 5.3693 MHz; at 14.318 MHz sample rate each
     * NES dot = 2.667 samples, so 256 pixels occupy ~683 samples.  The
     * remaining 758−683 = 75 samples are blank border, split 37 left / 38
     * right to centre the image within the NTSC active window.
     *
     * Subcarrier phase for the pixel region: the border adds 37 samples
     * to the offset, so the first pixel sample is at absolute sample
     * 116+37 = 153.  153 % 4 = 1, accounted for below.
     */
#define VIDEO_BORDER_LEFT   37u
#define VIDEO_BORDER_RIGHT  38u
#define VIDEO_PIXEL_SAMPLES (VIDEO_ACTIVE_SAMPLES - VIDEO_BORDER_LEFT - VIDEO_BORDER_RIGHT)  /* 683 */

    if (active && pixels != NULL) {
        /* Left border */
        for (uint i = 0; i < VIDEO_BORDER_LEFT; i++)
            EMIT(VIDEO_DAC_BLANK);

        /*
         * Map 256 NES pixels across 683 active samples using fixed-point
         * integer scaling.  DAC code looked up directly from s_dac_lut.
         *   pixel_inc = (256 << 16) / 683 = 24563  (≈256/683, 16.16 fixed)
         * Phase: first pixel sample is at absolute sample 153; 153 % 4 = 1.
         */
        uint32_t       pixel_fp  = 0u;
        const uint32_t pixel_inc = (256u << 16u) / VIDEO_PIXEL_SAMPLES;

        for (uint s = 0; s < VIDEO_PIXEL_SAMPLES; s++) {
            uint pixel_idx = pixel_fp >> 16u;
            if (pixel_idx >= 256u) pixel_idx = 255u;
            pixel_fp += pixel_inc;
            EMIT(s_dac_lut[pixels[pixel_idx] & 0x3Fu][(VIDEO_BORDER_LEFT + s) & 3u]);
        }

        /* Right border */
        for (uint i = 0; i < VIDEO_BORDER_RIGHT; i++)
            EMIT(VIDEO_DAC_BLANK);
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

static void MICRONES_HOT_FUNC(render_vsync_line)(uint32_t *buf) {
    /* All-sync: DAC code 0 for entire line.  GP4 stays HIGH (Core 1). */
    memset(buf, 0, VIDEO_WORDS_PER_LINE * sizeof(uint32_t));
}

static void MICRONES_HOT_FUNC(render_blank_line)(uint32_t *buf) {
    render_scanline_composite(buf, NULL, false);
}

static void MICRONES_HOT_FUNC(render_test_scanline)(uint32_t *buf) {
    /* 8 vertical bars across the active region — no Core 1 needed.
     *
     * Each bar is four pre-computed DAC codes, one per subcarrier phase
     * (the subcarrier is sample_rate / 4 = 3.579545 MHz, so successive
     * active-video samples cycle through phases 0, 1, 2, 3).  The burst
     * pattern {1, 4, 7, 4} sets phase 0 = peak-negative, so the chroma
     * values in each bar are cosines of (phase × 90° − hue) around the
     * bar's luma centre.
     *
     * With MICRONES_CHROMA_ENABLED=1 (default) the bars carry a
     * SMPTE-ish colour sequence — White, Yellow, Cyan, Green, Magenta,
     * Red, Blue, Black — useful for checking that the colour chain
     * (burst phase, chroma decoder, TV lock) is healthy independent of
     * the emulator's chroma_lut / s_dac_lut path.
     *
     * With chroma disabled the bars collapse to the original greyscale
     * ramp so the same image is comparable to the legacy luma test. */
#if MICRONES_CHROMA_ENABLED
    static const uint8_t bar_dac[8][4] = {
        /* phase  0   1   2   3                                       */
        {       12, 12, 12, 12 },  /* 0: White    luma 12, no chroma  */
        {       13,  9,  9, 13 },  /* 1: Yellow   luma 11, hue 315°   */
        {        8, 12, 12,  8 },  /* 2: Cyan     luma 10, hue 135°   */
        {        5,  8, 11,  8 },  /* 3: Green    luma  8, hue 180°   */
        {       10,  7,  4,  7 },  /* 4: Magenta  luma  7, hue   0°   */
        {        8,  8,  4,  4 },  /* 5: Red      luma  6, hue  45°   */
        {        5,  8,  5,  2 },  /* 6: Blue     luma  5, hue  90°   */
        {        4,  4,  4,  4 },  /* 7: Black    blank level         */
    };
#else
    static const uint8_t bar_dac[8][4] = {
        { 13, 13, 13, 13 },  { 11, 11, 11, 11 },
        {  9,  9,  9,  9 },  {  7,  7,  7,  7 },
        {  6,  6,  6,  6 },  {  5,  5,  5,  5 },
        {  4,  4,  4,  4 },  {  4,  4,  4,  4 },
    };
#endif

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
        uint bar   = (s * 8u) / VIDEO_ACTIVE_SAMPLES;
        uint phase = (VIDEO_ACTIVE_START + s) & 3u;
        EMIT_T(bar_dac[bar][phase]);
    }
    for (uint i = 0; i < 38u; i++)  /* 36 front porch + 2 padding */
        EMIT_T(VIDEO_DAC_BLANK);

#undef EMIT_T
}

static void MICRONES_HOT_FUNC(render_ntsc_line)(int ntsc_line, uint32_t *buf, ScanlineQueue *queue) {
    ++s_lines_rendered;
    if (ntsc_line < (int)VIDEO_VSYNC_LINES) {
        ++s_vsync_lines_rendered;
        render_vsync_line(buf);
    } else if (ntsc_line < (int)(VIDEO_VSYNC_LINES + VIDEO_TOP_BLANK_LINES) ||
               ntsc_line >= (int)VIDEO_ACTIVE_END_LINE) {
        ++s_blank_lines_rendered;
        render_blank_line(buf);
    } else {
        int active_y = ntsc_line - (int)VIDEO_ACTIVE_START_LINE;
        ScanlineQueueSlot slot;
        uint32_t level = queue->head - queue->tail;
        diag_record_queue_level(level);
        scanline_queue_pop_blocking(queue, &slot);
        ++s_active_lines_rendered;
        diag_record_active_scanline(active_y, &slot);
        render_scanline_composite(buf, slot.pixels, true);
        if (active_y == (int)VIDEO_ACTIVE_LINES - 1) {
            s_frames_rendered++;
        }
    }
}

/* =========================================================================
 * DMA helpers
 * ========================================================================= */

static void MICRONES_HOT_FUNC(dma_rearm)(int buf_idx) {
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
    /* High AHB-bus priority: when CPU and DMA both want the same bus
     * cycle, DMA wins.  Test pattern is fine without this because Core 1
     * is idle, but the emulator path has Core 1 hammering SRAM (queue
     * pop, palette LUT reads, scanline buffer writes) at the same time
     * DMA is reading the other ping-pong buffer to feed PIO.  Without
     * high priority, occasional DMA stalls let the PIO FIFO drain by a
     * sample, which lengthens the affected line by ~70 ns and produces
     * the diagonal shear seen on the v0.1 PCB. */
    channel_config_set_high_priority(&s_dma_cfg[0], true);

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
    channel_config_set_high_priority(&s_dma_cfg[1], true);

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
static void __isr MICRONES_HOT_FUNC(dma_irq0_handler)(void) {
    uint32_t mask   = (1u << s_dma_chan[0]) | (1u << s_dma_chan[1]);
    uint32_t status = dma_hw->ints0 & mask;
    dma_hw->ints0   = status;   /* clear all triggered bits in one write */
    if (status != 0u) ++s_dma_irq0_count;
    if (status & (1u << s_dma_chan[0])) dma_rearm(0);
    if (status & (1u << s_dma_chan[1])) dma_rearm(1);
}

/* =========================================================================
 * Core 1  DMA IRQ_1  handler  (emulator render loop)
 * ========================================================================= */
static volatile uint32_t s_irq1_fire_count = 0;

static void __isr MICRONES_HOT_FUNC(dma_irq1_handler)(void) {
    /*
     * The DMA chain has already started the next channel.  Raise GP4
     * immediately for the new line's horizontal sync pulse.
     * IRQ latency on Cortex-M33 ≈ 12 cycles; error < 1 sample (22 cycles).
     */
    s_irq1_fire_count++;
    s_dma_irq1_count++;
    MICRONES_SYNC_CLAMP_ENGAGE();

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

    /* Sync clamp gate — output, initially LOW (MOSFET off).  Keep at the
     * default pad strength on both boards; an earlier attempt at 12 mA +
     * fast slew on the v0.1 PCB caused ringing on the gate that the TV
     * mis-read as sync edges. */
    gpio_init(MICRONES_VIDEO_SYNC_GPIO);
    gpio_set_dir(MICRONES_VIDEO_SYNC_GPIO, GPIO_OUT);
    gpio_put(MICRONES_VIDEO_SYNC_GPIO, 0);

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

#if MICRONES_ANALOG_VIDEO_DIAG
    printf("ntsc diag: init board=%s sys=%lu clkdiv=%.6f dac_base=%u sync_gpio=%u chroma=%u words=%u lines=%u active_start=%u active_lines=%u\n",
#if defined(MICRONES_BOARD_V0_1)
           "v0_1",
#else
           "breadboard",
#endif
           (unsigned long)clock_get_hz(clk_sys),
           (double)MICRONES_PIO_CLKDIV,
           (unsigned)MICRONES_VIDEO_PIN_BASE,
           (unsigned)MICRONES_VIDEO_SYNC_GPIO,
           (unsigned)MICRONES_CHROMA_ENABLED,
           (unsigned)VIDEO_WORDS_PER_LINE,
           (unsigned)VIDEO_LINES_PER_FRAME,
           (unsigned)VIDEO_ACTIVE_START_LINE,
           (unsigned)VIDEO_ACTIVE_LINES);
    printf("ntsc diag: dma ch0=%u ch1=%u pio=%u sm=%u offset=%u sync_func=%u sync_level=%u\n",
           (unsigned)s_dma_chan[0],
           (unsigned)s_dma_chan[1],
           0u,
           (unsigned)s_sm,
           (unsigned)s_pio_offset,
           (unsigned)gpio_get_function(MICRONES_VIDEO_SYNC_GPIO),
           (unsigned)gpio_get(MICRONES_VIDEO_SYNC_GPIO));
    for (uint i = 0; i < MICRONES_VIDEO_PIN_COUNT; ++i) {
        uint pin = MICRONES_VIDEO_PIN_BASE + i;
        printf("ntsc diag: dac pin[%u]=GP%u func=%u level=%u\n",
               (unsigned)i,
               (unsigned)pin,
               (unsigned)gpio_get_function(pin),
               (unsigned)gpio_get(pin));
    }
#endif
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

#if MICRONES_ANALOG_VIDEO_DIAG
    printf("ntsc diag: start test_pattern_filled=%u sm_enabled=%u ch0_busy=%u ch1_busy=%u irq0_en=0x%08lx irq1_en=0x%08lx\n",
           s_test_pattern_filled ? 1u : 0u,
           (unsigned)((s_pio->ctrl >> (PIO_CTRL_SM_ENABLE_LSB + s_sm)) & 1u),
           (unsigned)dma_channel_is_busy(s_dma_chan[0]),
           (unsigned)dma_channel_is_busy(s_dma_chan[1]),
           (unsigned long)dma_hw->inte0,
           (unsigned long)dma_hw->inte1);
#endif
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
    out->frames_rendered    = s_frames_rendered;
    out->irq0_count         = s_dma_irq0_count;
    out->irq1_count         = s_dma_irq1_count;
    out->lines_rendered     = s_lines_rendered;
    out->vsync_lines_rendered = s_vsync_lines_rendered;
    out->blank_lines_rendered = s_blank_lines_rendered;
    out->active_lines_rendered = s_active_lines_rendered;
    out->render_us_total    = s_render_us_total;
    out->render_us_max      = s_render_us_max;
    out->render_over_50us_count = s_render_over_50us_count;
    out->render_over_63us_count = s_render_over_63us_count;
    out->render_over_100us_count = s_render_over_100us_count;
    out->render_us_max_line = s_render_us_max_line;
    out->render_us_max_active_y = s_render_us_max_active_y;
    out->render_us_max_kind = s_render_us_max_kind;
    out->display_line       = s_display_line_snapshot;
    out->render_line        = s_render_line_snapshot;
    out->idle_buf           = (uint32_t)s_idle_buf;
    out->queue_level        = s_queue_level_snapshot;
    out->queue_level_max    = s_queue_level_max;
    out->queue_producer_stall_count = 0;
    out->queue_producer_stall_us_total = 0;
    out->queue_consumer_wait_count = 0;
    out->queue_consumer_wait_us_total = 0;
    out->queue_consumer_wait_us_max = 0;
    out->scanline_y_mismatch_count = s_scanline_y_mismatch_count;
    for (int i = 0; i < 4; ++i) {
        out->probe_y[i] = s_probe_y[i];
        out->probe_slot_y[i] = s_probe_slot_y[i];
        out->probe_hash[i] = s_probe_hash[i];
        out->probe_nonblack[i] = s_probe_nonblack[i];
        out->probe_min[i] = s_probe_min[i];
        out->probe_max[i] = s_probe_max[i];
    }
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

void MICRONES_HOT_FUNC(video_ntsc_core1_entry)(void) {
    uint32_t ch_mask = (1u << s_dma_chan[0]) | (1u << s_dma_chan[1]);

    printf("[c1] entry: chan0=%u chan1=%u ch_mask=0x%08x\n",
           s_dma_chan[0], s_dma_chan[1], (unsigned)ch_mask);

    /*
     * Hand DMA IRQ ownership to Core 1:
     *   • Disable IRQ_0 for our channels (Core 0 handler stops firing).
     *   • Register and enable IRQ_1 on Core 1's NVIC.
     */
    dma_set_irq0_channel_mask_enabled(ch_mask, false);
    printf("[c1] irq0 disabled for our channels\n");

    irq_set_exclusive_handler(DMA_IRQ_1, dma_irq1_handler);
    irq_set_enabled(DMA_IRQ_1, true);
    printf("[c1] irq1 registered and enabled\n");

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

    /* Start: clamp engaged for line 0 sync (no-op when disabled), then
     * start channel A. */
    pio_sm_set_enabled(s_pio, s_sm, true);
    MICRONES_SYNC_CLAMP_ENGAGE();
    dma_channel_start(s_dma_chan[0]);

    printf("[c1] dma started: chan0_busy=%d chan1_busy=%d ints1=0x%08x\n",
           (int)dma_channel_is_busy(s_dma_chan[0]),
           (int)dma_channel_is_busy(s_dma_chan[1]),
           (unsigned)dma_hw->ints1);

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
         *     Target: 47 × 22 = 1034 cycles HIGH from line start (at 315 MHz).
         *     IRQ latency + wakeup ≈ 20-30 cycles; we wait 1000 cycles so
         *     total ≈ 1020-1030 cycles  (error < 1 sample = 22 cycles). ✓
         */
        bool display_is_vsync = (display_line < (int)VIDEO_VSYNC_LINES);
        if (!display_is_vsync) {
            busy_wait_at_least_cycles(1000u);
            gpio_put(MICRONES_VIDEO_SYNC_GPIO, 0);
        }

        /* ---- Render render_line into the now-idle buffer ---- */
#if MICRONES_ANALOG_VIDEO_DIAG
        uint64_t render_started_us = time_us_64();
#endif
        render_ntsc_line(render_line, scanline_buf[s_idle_buf], queue);
#if MICRONES_ANALOG_VIDEO_DIAG
        uint32_t render_us = (uint32_t)(time_us_64() - render_started_us);
        s_render_us_total += render_us;
        if (render_us > 50u) ++s_render_over_50us_count;
        if (render_us > 63u) ++s_render_over_63us_count;
        if (render_us > 100u) ++s_render_over_100us_count;
        if (render_us > s_render_us_max) {
            s_render_us_max = render_us;
            s_render_us_max_line = (uint32_t)render_line;
            s_render_us_max_kind = diag_render_kind_for_line(render_line);
            s_render_us_max_active_y =
                (s_render_us_max_kind == VIDEO_RENDER_KIND_ACTIVE)
                    ? (uint32_t)(render_line - (int)VIDEO_ACTIVE_START_LINE)
                    : 0xffffffffu;
        }
#endif

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
        s_display_line_snapshot = (uint32_t)display_line;
        s_render_line_snapshot = (uint32_t)render_line;
    }
}
