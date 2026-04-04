#include "video_tft_ili9341.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/time.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum {
    TFT_PIN_MISO = 16u,
    TFT_PIN_CS = 17u,
    TFT_PIN_SCK = 18u,
    TFT_PIN_MOSI = 19u,
    TFT_PIN_DC = 20u,
    TFT_PIN_RST = 21u,
    TFT_PIN_BL = 22u,
    TFT_SPI_BAUD_HZ = 62500000u,
    TFT_WIDTH = 320u,
    TFT_HEIGHT = 240u,
    TFT_NES_X_OFFSET = 32u,
    TFT_TEST_BAR_COUNT = 8u,
};

/*
 * Assumptions for the first-pass TFT target:
 *   - controller is ILI9341-compatible over 4-wire SPI
 *   - panel is configured in 320x240 landscape mode
 *   - the NES 256x240 image is centered horizontally with 32-pixel side bars
 *   - pixel writes are unidirectional; GP16/MISO is configured but unused
 */

enum {
    ILI9341_SWRESET = 0x01,
    ILI9341_SLPOUT = 0x11,
    ILI9341_DISPON = 0x29,
    ILI9341_CASET = 0x2A,
    ILI9341_PASET = 0x2B,
    ILI9341_RAMWR = 0x2C,
    ILI9341_MADCTL = 0x36,
    ILI9341_COLMOD = 0x3A,
};

enum {
    ILI9341_MADCTL_MV = 0x20,
    ILI9341_MADCTL_BGR = 0x08,
};

#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))
#define RGB565_BE(r, g, b) \
    ((uint16_t)((RGB565((r), (g), (b)) >> 8) | (RGB565((r), (g), (b)) << 8)))

static const uint16_t k_nes_palette_be[64] = {
    RGB565_BE( 84,  84,  84), RGB565_BE(  0,  30, 116), RGB565_BE(  8,  16, 144), RGB565_BE( 48,   0, 136),
    RGB565_BE( 68,   0, 100), RGB565_BE( 92,   0,  48), RGB565_BE( 84,   4,   0), RGB565_BE( 60,  24,   0),
    RGB565_BE( 32,  42,   0), RGB565_BE(  8,  58,   0), RGB565_BE(  0,  64,   0), RGB565_BE(  0,  60,   0),
    RGB565_BE(  0,  50,  60), RGB565_BE(  0,   0,   0), RGB565_BE(  0,   0,   0), RGB565_BE(  0,   0,   0),

    RGB565_BE(152, 150, 152), RGB565_BE(  8,  76, 196), RGB565_BE( 48,  50, 236), RGB565_BE( 92,  30, 228),
    RGB565_BE(136,  20, 176), RGB565_BE(160,  20, 100), RGB565_BE(152,  34,  32), RGB565_BE(120,  60,   0),
    RGB565_BE( 84,  90,   0), RGB565_BE( 40, 114,   0), RGB565_BE(  8, 124,   0), RGB565_BE(  0, 118,  40),
    RGB565_BE(  0, 102, 120), RGB565_BE(  0,   0,   0), RGB565_BE(  0,   0,   0), RGB565_BE(  0,   0,   0),

    RGB565_BE(236, 238, 236), RGB565_BE( 76, 154, 236), RGB565_BE(120, 124, 236), RGB565_BE(176,  98, 236),
    RGB565_BE(228,  84, 236), RGB565_BE(236,  88, 180), RGB565_BE(236, 106, 100), RGB565_BE(212, 136,  32),
    RGB565_BE(160, 170,   0), RGB565_BE(116, 196,   0), RGB565_BE( 76, 208,  32), RGB565_BE( 56, 204, 108),
    RGB565_BE( 56, 180, 204), RGB565_BE( 60,  60,  60), RGB565_BE(  0,   0,   0), RGB565_BE(  0,   0,   0),

    RGB565_BE(236, 238, 236), RGB565_BE(168, 204, 236), RGB565_BE(188, 188, 236), RGB565_BE(212, 178, 236),
    RGB565_BE(236, 174, 236), RGB565_BE(236, 174, 212), RGB565_BE(236, 180, 176), RGB565_BE(228, 196, 144),
    RGB565_BE(204, 210, 120), RGB565_BE(180, 222, 120), RGB565_BE(168, 226, 144), RGB565_BE(152, 226, 180),
    RGB565_BE(160, 214, 228), RGB565_BE(160, 162, 160), RGB565_BE(  0,   0,   0), RGB565_BE(  0,   0,   0),
};

static const uint16_t k_test_bar_colors_be[TFT_TEST_BAR_COUNT] = {
    RGB565_BE(255, 255, 255),
    RGB565_BE(255, 255,   0),
    RGB565_BE(  0, 255, 255),
    RGB565_BE(  0, 255,   0),
    RGB565_BE(255,   0, 255),
    RGB565_BE(255,   0,   0),
    RGB565_BE(  0,   0, 255),
    RGB565_BE(  0,   0,   0),
};

/*
 * Dirty-span tracking (Phase 1–4).
 *
 * A TftSpan describes a dirty horizontal run on a single scanline.
 * x and endX are in NES column space [0, NES_FRAME_WIDTH); endX is exclusive.
 *
 * Merge threshold: opening a new CASET/PASET/RAMWR window costs ~8 SPI bytes
 * (≈4 pixels worth of bandwidth at 16bpp). Merge adjacent dirty runs if the
 * clean gap between them is ≤ TFT_SPAN_MERGE_THRESHOLD pixels.
 */
typedef struct {
    uint16_t x;
    uint16_t endX;
} TftSpan;

#define TFT_SPAN_MERGE_THRESHOLD 4u

/*
 * Pool sized for worst-case: 128 alternating 2-pixel spans per scanline.
 * In practice the merge threshold reduces this dramatically.
 */
#define TFT_SPAN_POOL_SIZE 128u

static char s_last_error[128];
static PicoTftIli9341Stats s_stats;

/* Current-scanline conversion buffer (4-byte aligned for uint32_t diff). */
static uint16_t s_line_buffer_be[TFT_WIDTH] __attribute__((aligned(4)));

/* Phase 1: previous-frame RGB565 buffer for dirty-region diff. ~120 KB. */
static uint16_t s_prev_frame_be[NES_FRAME_HEIGHT][NES_FRAME_WIDTH] __attribute__((aligned(4)));
static bool s_prev_frame_valid;

/* Phase 2: per-scanline span pool (reused each scanline, no heap needed). */
static TftSpan s_span_pool[TFT_SPAN_POOL_SIZE];

/*
 * Interlacing: alternate even/odd scanlines each frame to halve SPI traffic.
 * Each scanline is refreshed at 30 Hz instead of 60 Hz; the other half of
 * the display shows data that is one frame old.  Fast vertical motion produces
 * visible horizontal combing but the frame rate on busy scenes roughly doubles.
 *
 * Enabled by default.  Call video_tft_ili9341_set_interlace(false) to disable
 * for comparison.
 */
static bool    s_interlace_enabled = true;
static uint8_t s_interlace_parity  = 0u; /* 0 = process even rows, 1 = odd rows */

/* -------------------------------------------------------------------------
 * Public control
 * ---------------------------------------------------------------------- */

void video_tft_ili9341_set_interlace(bool enabled) {
    s_interlace_enabled = enabled;
    s_interlace_parity  = 0u; /* resync to even on any mode change */
}

/* -------------------------------------------------------------------------
 * SPI / GPIO helpers
 * ---------------------------------------------------------------------- */

static inline spi_inst_t *tft_spi(void) {
    return spi0;
}

static void tft_set_error(const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, args);
    va_end(args);
}

static inline void tft_select(void) {
    gpio_put(TFT_PIN_CS, 0);
}

static inline void tft_deselect(void) {
    gpio_put(TFT_PIN_CS, 1);
}

static void tft_write_command(uint8_t cmd) {
    gpio_put(TFT_PIN_DC, 0);
    tft_select();
    spi_write_blocking(tft_spi(), &cmd, 1);
    tft_deselect();
}

static void tft_write_data(const uint8_t *data, size_t len) {
    if (len == 0) {
        return;
    }

    gpio_put(TFT_PIN_DC, 1);
    tft_select();
    spi_write_blocking(tft_spi(), data, len);
    tft_deselect();
}

static void tft_write_command_with_data(uint8_t cmd, const uint8_t *data, size_t len) {
    tft_write_command(cmd);
    tft_write_data(data, len);
}

static void tft_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t caset[4] = {
        (uint8_t)(x0 >> 8), (uint8_t)x0,
        (uint8_t)(x1 >> 8), (uint8_t)x1,
    };
    uint8_t paset[4] = {
        (uint8_t)(y0 >> 8), (uint8_t)y0,
        (uint8_t)(y1 >> 8), (uint8_t)y1,
    };

    tft_write_command_with_data(ILI9341_CASET, caset, sizeof(caset));
    tft_write_command_with_data(ILI9341_PASET, paset, sizeof(paset));
    tft_write_command(ILI9341_RAMWR);
}

static void tft_stream_pixels(const uint16_t *pixels_be, size_t pixel_count) {
    gpio_put(TFT_PIN_DC, 1);
    tft_select();
    spi_write_blocking(tft_spi(), (const uint8_t *)pixels_be, pixel_count * sizeof(uint16_t));
    tft_deselect();
    s_stats.bytes_sent += pixel_count * sizeof(uint16_t);
}

static void tft_fill_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color_be) {
    uint16_t width = (uint16_t)(x1 - x0 + 1u);
    uint16_t height = (uint16_t)(y1 - y0 + 1u);

    for (uint16_t x = 0; x < width; ++x) {
        s_line_buffer_be[x] = color_be;
    }

    tft_set_window(x0, y0, x1, y1);
    for (uint16_t y = 0; y < height; ++y) {
        tft_stream_pixels(s_line_buffer_be, width);
    }
}

/* -------------------------------------------------------------------------
 * Phase 2+3: dirty-span diff
 *
 * Compares cur_be[0..NES_FRAME_WIDTH) against prev_be[0..NES_FRAME_WIDTH)
 * using 32-bit (2-pixel) chunks. Emits spans into s_span_pool, merging
 * adjacent dirty runs separated by ≤ TFT_SPAN_MERGE_THRESHOLD clean pixels.
 * Returns the number of spans emitted.
 *
 * Both pointers must be 4-byte aligned (guaranteed by __attribute__((aligned(4)))
 * on s_line_buffer_be and s_prev_frame_be).
 * ---------------------------------------------------------------------- */
static uint16_t tft_diff_scanline(const uint16_t *cur_be, const uint16_t *prev_be) {
    uint16_t count = 0u;
    const uint32_t *cur32  = (const uint32_t *)(const void *)cur_be;
    const uint32_t *prev32 = (const uint32_t *)(const void *)prev_be;
    uint16_t chunk = 0u; /* each chunk covers 2 pixels */

    while (chunk < (NES_FRAME_WIDTH / 2u)) {
        /* Advance past matching 2-pixel chunks. */
        while (chunk < (NES_FRAME_WIDTH / 2u) && cur32[chunk] == prev32[chunk])
            ++chunk;
        if (chunk >= (NES_FRAME_WIDTH / 2u))
            break;

        /* First dirty chunk: open a new span. */
        uint16_t span_start  = (uint16_t)(chunk * 2u);
        uint16_t span_end    = span_start; /* exclusive; extended below */
        uint16_t gap_pixels  = 0u;

        /* Extend the span, merging short clean runs (≤ threshold). */
        while (chunk < (NES_FRAME_WIDTH / 2u)) {
            if (cur32[chunk] != prev32[chunk]) {
                span_end   = (uint16_t)((chunk + 1u) * 2u);
                gap_pixels = 0u;
            } else {
                gap_pixels += 2u;
                if (gap_pixels > TFT_SPAN_MERGE_THRESHOLD)
                    break;
            }
            ++chunk;
        }

        /* Emit span. On pool overflow, extend the last span to avoid dropping
         * dirty pixels (sends a few extra pixels but stays correct). */
        if (count < TFT_SPAN_POOL_SIZE) {
            s_span_pool[count].x    = span_start;
            s_span_pool[count].endX = span_end;
            ++count;
        } else {
            s_span_pool[TFT_SPAN_POOL_SIZE - 1u].endX = span_end;
        }

        /* Resume outer scan from the end of this span.
         * The gap chunks between span_end and chunk are clean; the outer
         * skip loop will step past them on the next iteration. */
        chunk = span_end / 2u;
    }

    return count;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

bool video_tft_ili9341_init(void) {
    const uint32_t baud = spi_init(tft_spi(), TFT_SPI_BAUD_HZ);
    const uint8_t colmod = 0x55;
    printf("tft: spi baud actual=%lu Hz  target=%u Hz\n",
           (unsigned long)baud, TFT_SPI_BAUD_HZ);
    const uint8_t madctl = (uint8_t)(ILI9341_MADCTL_MV | ILI9341_MADCTL_BGR);

    memset(&s_stats, 0, sizeof(s_stats));
    s_last_error[0] = '\0';
    s_prev_frame_valid = false;
    s_interlace_parity = 0u;

    gpio_init(TFT_PIN_CS);
    gpio_set_dir(TFT_PIN_CS, GPIO_OUT);
    tft_deselect();

    gpio_init(TFT_PIN_DC);
    gpio_set_dir(TFT_PIN_DC, GPIO_OUT);
    gpio_put(TFT_PIN_DC, 1);

    gpio_init(TFT_PIN_RST);
    gpio_set_dir(TFT_PIN_RST, GPIO_OUT);
    gpio_put(TFT_PIN_RST, 1);

    gpio_init(TFT_PIN_BL);
    gpio_set_dir(TFT_PIN_BL, GPIO_OUT);
    gpio_put(TFT_PIN_BL, 0);

    gpio_set_function(TFT_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(TFT_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(TFT_PIN_MISO, GPIO_FUNC_SPI);
    spi_set_format(tft_spi(), 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    sleep_ms(5);
    gpio_put(TFT_PIN_RST, 0);
    sleep_ms(20);
    gpio_put(TFT_PIN_RST, 1);
    sleep_ms(150);

    tft_write_command(ILI9341_SWRESET);
    sleep_ms(150);

    tft_write_command(ILI9341_SLPOUT);
    sleep_ms(120);

    tft_write_command_with_data(ILI9341_COLMOD, &colmod, 1);
    tft_write_command_with_data(ILI9341_MADCTL, &madctl, 1);
    tft_write_command(ILI9341_DISPON);
    sleep_ms(120);

    gpio_put(TFT_PIN_BL, 1);
    tft_fill_window(0, 0, TFT_WIDTH - 1u, TFT_HEIGHT - 1u, RGB565_BE(0, 0, 0));

    if (baud == 0u) {
        tft_set_error("spi_init returned 0");
        return false;
    }

    return true;
}

const char *video_tft_ili9341_last_error(void) {
    return s_last_error;
}

void video_tft_ili9341_draw_test_pattern(void) {
    const uint16_t bar_width = NES_FRAME_WIDTH / TFT_TEST_BAR_COUNT;

    /* Invalidate the diff buffer: display state will no longer match s_prev_frame_be. */
    s_prev_frame_valid = false;

    tft_fill_window(0, 0, TFT_WIDTH - 1u, TFT_HEIGHT - 1u, RGB565_BE(0, 0, 0));

    for (uint16_t x = 0; x < NES_FRAME_WIDTH; ++x) {
        uint16_t bar = x / bar_width;
        if (bar >= TFT_TEST_BAR_COUNT) {
            bar = TFT_TEST_BAR_COUNT - 1u;
        }
        s_line_buffer_be[x] = k_test_bar_colors_be[bar];
    }

    tft_set_window(TFT_NES_X_OFFSET, 0, TFT_NES_X_OFFSET + NES_FRAME_WIDTH - 1u, NES_FRAME_HEIGHT - 1u);
    for (uint16_t y = 0; y < NES_FRAME_HEIGHT; ++y) {
        tft_stream_pixels(s_line_buffer_be, NES_FRAME_WIDTH);
    }
}

void video_tft_ili9341_present_frame(const NesFrameBuffer *frame) {
    uint64_t start_us, elapsed_us;
    uint64_t convert_us = 0u, diff_us = 0u, spi_us = 0u;
    uint64_t t0, t1, t2;
    uint64_t frame_spans = 0u;

    if (frame == NULL) {
        return;
    }

    start_us = time_us_64();

    if (!s_prev_frame_valid) {
        /*
         * First frame (or after init/test-pattern): send the full frame and
         * populate s_prev_frame_be so subsequent frames can diff against it.
         * Keep CS asserted for the entire pixel stream to avoid 240 per-line
         * CS toggles.
         */
        tft_set_window(TFT_NES_X_OFFSET, 0u,
                       TFT_NES_X_OFFSET + NES_FRAME_WIDTH - 1u,
                       NES_FRAME_HEIGHT - 1u);

        gpio_put(TFT_PIN_DC, 1);
        tft_select();

        for (uint16_t y = 0u; y < NES_FRAME_HEIGHT; ++y) {
            const uint8_t *src = nes_framebuffer_scanline_const(frame, y);

            t0 = time_us_64();
            for (uint16_t x = 0u; x < NES_FRAME_WIDTH; ++x)
                s_line_buffer_be[x] = k_nes_palette_be[src[x] & 0x3fu];
            t1 = time_us_64();
            convert_us += t1 - t0;

            spi_write_blocking(tft_spi(), (const uint8_t *)s_line_buffer_be,
                               NES_FRAME_WIDTH * sizeof(uint16_t));
            spi_us += time_us_64() - t1;

            memcpy(s_prev_frame_be[y], s_line_buffer_be,
                   NES_FRAME_WIDTH * sizeof(uint16_t));
        }

        tft_deselect();
        s_stats.bytes_sent += (uint64_t)NES_FRAME_WIDTH * NES_FRAME_HEIGHT * sizeof(uint16_t);
        s_prev_frame_valid = true;
        ++s_stats.full_frames_sent;
    } else {
        /*
         * Subsequent frames: diff each scanline against the previous frame and
         * send only the changed spans.
         *
         * Interlacing: when enabled, alternate between even and odd scanlines
         * each frame.  Each scanline is refreshed at ~30 Hz; the display shows
         * the previous frame's data for the skipped rows.  s_prev_frame_be for
         * skipped rows is left untouched so the next interlaced pass diffs
         * correctly against what the display is actually showing.
         *
         * Phase 4 cursor cache: the ILI9341 row register (PASET) holds its
         * value across CS transitions.  When consecutive spans fall on the same
         * scanline, skip the PASET command — only CASET + RAMWR are needed.
         * cur_cursor_y tracks the row last written to PASET this frame;
         * 0xFFFF is used as an invalid sentinel so the first span always writes
         * a fresh PASET.
         */
        uint16_t cur_cursor_y = 0xFFFFu;
        uint16_t y_start = s_interlace_enabled ? s_interlace_parity : 0u;
        uint16_t y_step  = s_interlace_enabled ? 2u : 1u;

        for (uint16_t y = y_start; y < NES_FRAME_HEIGHT; y += y_step) {
            const uint8_t *src = nes_framebuffer_scanline_const(frame, y);
            uint16_t span_count;

            /* Convert palette-indexed scanline to RGB565 big-endian. */
            t0 = time_us_64();
            for (uint16_t x = 0u; x < NES_FRAME_WIDTH; ++x)
                s_line_buffer_be[x] = k_nes_palette_be[src[x] & 0x3fu];
            t1 = time_us_64();
            convert_us += t1 - t0;

            /* Diff against previous frame to find changed spans. */
            span_count = tft_diff_scanline(s_line_buffer_be, s_prev_frame_be[y]);
            t2 = time_us_64();
            diff_us += t2 - t1;

            /* Send each changed span. */
            for (uint16_t i = 0u; i < span_count; ++i) {
                const TftSpan *sp = &s_span_pool[i];
                uint16_t abs_x0 = (uint16_t)(TFT_NES_X_OFFSET + sp->x);
                uint16_t abs_x1 = (uint16_t)(TFT_NES_X_OFFSET + sp->endX - 1u);
                uint16_t n_pixels = (uint16_t)(sp->endX - sp->x);

                /* CASET (always), PASET (only on row change), RAMWR. */
                {
                    uint8_t caset[4] = {
                        (uint8_t)(abs_x0 >> 8), (uint8_t)abs_x0,
                        (uint8_t)(abs_x1 >> 8), (uint8_t)abs_x1,
                    };
                    tft_write_command_with_data(ILI9341_CASET, caset, sizeof(caset));
                    if (cur_cursor_y != y) {
                        uint8_t paset[4] = {
                            (uint8_t)(y >> 8), (uint8_t)y,
                            (uint8_t)(y >> 8), (uint8_t)y,
                        };
                        tft_write_command_with_data(ILI9341_PASET, paset, sizeof(paset));
                        cur_cursor_y = y;
                    }
                    tft_write_command(ILI9341_RAMWR);
                }

                /* Stream span pixels. */
                {
                    uint64_t ts = time_us_64();
                    gpio_put(TFT_PIN_DC, 1);
                    tft_select();
                    spi_write_blocking(tft_spi(),
                                       (const uint8_t *)&s_line_buffer_be[sp->x],
                                       n_pixels * sizeof(uint16_t));
                    tft_deselect();
                    spi_us += time_us_64() - ts;
                }
                s_stats.bytes_sent += n_pixels * sizeof(uint16_t);
            }

            frame_spans += span_count;

            /* Update prev-frame buffer. Only needed when something changed, but
             * the converted line is already in s_line_buffer_be; memcpy of a
             * matching line is harmless and avoids a branch in the common case. */
            if (span_count > 0u) {
                memcpy(s_prev_frame_be[y], s_line_buffer_be,
                       NES_FRAME_WIDTH * sizeof(uint16_t));
            }
        }
    }

    if (s_interlace_enabled) {
        s_interlace_parity ^= 1u;
    }

    elapsed_us = time_us_64() - start_us;
    ++s_stats.frames_presented;
    s_stats.present_us_total += elapsed_us;
    s_stats.present_convert_us_total += convert_us;
    s_stats.present_diff_us_total += diff_us;
    s_stats.present_spi_us_total += spi_us;
    s_stats.spans_sent_total += frame_spans;
    if (elapsed_us > s_stats.present_us_max) {
        s_stats.present_us_max = elapsed_us;
    }
}

void video_tft_ili9341_get_stats(PicoTftIli9341Stats *stats_out) {
    if (stats_out == NULL) {
        return;
    }
    *stats_out = s_stats;
}
