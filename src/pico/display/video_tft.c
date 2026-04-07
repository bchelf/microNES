#include "video_tft.h"

#include "display_config.h"
#include "display_transport.h"
#include "tft_controller.h"

#include "pico/time.h"

#include <string.h>

#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))
#define RGB565_BE(r, g, b) \
    ((uint16_t)((RGB565((r), (g), (b)) >> 8) | (RGB565((r), (g), (b)) << 8)))

enum {
    TFT_TEST_BAR_COUNT = 8u,
    TFT_DMA_THRESHOLD_BYTES = 32u,
};

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

static PicoTftStats s_stats;
static uint16_t s_line_buffer_be[MICRONES_TFT_MAX_PANEL_WIDTH] __attribute__((aligned(4)));
static uint16_t s_prev_frame_be[NES_FRAME_HEIGHT][NES_FRAME_WIDTH] __attribute__((aligned(4)));
static bool s_prev_frame_valid = false;
static bool s_interlace_enabled = true;
static uint8_t s_interlace_parity = 0u;

_Static_assert(MICRONES_TFT_MAX_PANEL_WIDTH >= NES_FRAME_WIDTH, "panel line buffer too small");

static const TftTransportOps *video_tft_transport(void) {
    return tft_display_transport_get();
}

static const TftController *video_tft_controller(void) {
    return tft_controller_get();
}

static void video_tft_write_pixels(const uint8_t *data, size_t len) {
    const TftTransportOps *transport = video_tft_transport();

    if (len >= TFT_DMA_THRESHOLD_BYTES) {
        transport->write_pixels_dma(data, len);
        transport->wait_idle();
    } else {
        transport->write_pixels_blocking(data, len);
    }
}

static void video_tft_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    tft_controller_write_standard_window(video_tft_transport(), x0, y0, x1, y1);
}

static void video_tft_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color_be) {
    const TftTransportOps *transport = video_tft_transport();

    for (uint16_t i = 0; i < w; ++i) {
        s_line_buffer_be[i] = color_be;
    }

    video_tft_set_window(x, y, x + w - 1u, y + h - 1u);
    transport->begin_pixels();
    for (uint16_t row = 0; row < h; ++row) {
        video_tft_write_pixels((const uint8_t *)(const void *)s_line_buffer_be, (size_t)w * sizeof(uint16_t));
    }
    transport->end_pixels();
}

static void video_tft_blit_rect_rgb565(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *src, uint16_t stride) {
    const TftTransportOps *transport = video_tft_transport();

    video_tft_set_window(x, y, x + w - 1u, y + h - 1u);
    transport->begin_pixels();
    for (uint16_t row = 0; row < h; ++row) {
        const uint8_t *row_bytes = (const uint8_t *)(const void *)&src[row * stride];
        video_tft_write_pixels(row_bytes, (size_t)w * sizeof(uint16_t));
    }
    transport->end_pixels();
}

static bool video_tft_find_dirty_range(
    const uint16_t *cur_be,
    const uint16_t *prev_be,
    uint16_t *x0_out,
    uint16_t *x1_out
) {
    const uint32_t *cur32 = (const uint32_t *)(const void *)cur_be;
    const uint32_t *prev32 = (const uint32_t *)(const void *)prev_be;
    uint16_t min_x = NES_FRAME_WIDTH;
    uint16_t max_x = 0u;
    bool dirty = false;

    for (uint16_t chunk = 0u; chunk < (NES_FRAME_WIDTH / 2u); ++chunk) {
        if (cur32[chunk] != prev32[chunk]) {
            uint16_t chunk_x0 = (uint16_t)(chunk * 2u);
            uint16_t chunk_x1 = (uint16_t)(chunk_x0 + 1u);
            if (!dirty || chunk_x0 < min_x) {
                min_x = chunk_x0;
            }
            if (!dirty || chunk_x1 > max_x) {
                max_x = chunk_x1;
            }
            dirty = true;
        }
    }

    if (!dirty) {
        return false;
    }

    *x0_out = min_x;
    *x1_out = max_x;
    return true;
}

bool video_tft_init(void) {
    const TftTransportOps *transport = video_tft_transport();
    const TftController *controller = video_tft_controller();

    memset(&s_stats, 0, sizeof(s_stats));
    s_prev_frame_valid = false;
    s_interlace_parity = 0u;

    if (!transport->init()) {
        return false;
    }

    controller->init(transport);
    return true;
}

const char *video_tft_last_error(void) {
    return video_tft_transport()->last_error();
}

const char *video_tft_backend_name(void) {
    return video_tft_transport()->name;
}

const char *video_tft_controller_name(void) {
    return video_tft_controller()->name;
}

void video_tft_set_interlace(bool enabled) {
    s_interlace_enabled = enabled;
    s_interlace_parity = 0u;
}

void video_tft_draw_test_pattern(void) {
    const TftController *controller = video_tft_controller();
    const uint16_t bar_width = NES_FRAME_WIDTH / TFT_TEST_BAR_COUNT;

    s_prev_frame_valid = false;

    video_tft_fill_rect(0u, 0u, controller->width, controller->height, RGB565_BE(0, 0, 0));

    for (uint16_t x = 0; x < NES_FRAME_WIDTH; ++x) {
        uint16_t bar = x / bar_width;
        if (bar >= TFT_TEST_BAR_COUNT) {
            bar = TFT_TEST_BAR_COUNT - 1u;
        }
        s_line_buffer_be[x] = k_test_bar_colors_be[bar];
    }
    video_tft_blit_rect_rgb565(controller->viewport_x, controller->viewport_y,
                               NES_FRAME_WIDTH, NES_FRAME_HEIGHT, s_line_buffer_be, 0u);

    /* Small extra rects exercise partial-window writes independent of emulator output. */
    video_tft_fill_rect(controller->viewport_x + 8u, controller->viewport_y + 8u, 32u, 24u, RGB565_BE(255, 0, 0));
    video_tft_fill_rect(controller->viewport_x + NES_FRAME_WIDTH - 40u, controller->viewport_y + 8u, 32u, 24u, RGB565_BE(0, 255, 0));
    video_tft_fill_rect(controller->viewport_x + 8u, controller->viewport_y + NES_FRAME_HEIGHT - 32u, 32u, 24u, RGB565_BE(0, 0, 255));
}

void video_tft_present_frame(const NesFrameBuffer *frame) {
    const TftTransportOps *transport = video_tft_transport();
    const TftController *controller = video_tft_controller();
    uint64_t start_us;
    uint64_t convert_us = 0u;
    uint64_t diff_us = 0u;
    uint64_t bus_us = 0u;
    uint64_t frame_rects = 0u;

    if (frame == NULL) {
        return;
    }

    start_us = time_us_64();

    if (!s_prev_frame_valid) {
        video_tft_set_window(controller->viewport_x, controller->viewport_y,
                             controller->viewport_x + NES_FRAME_WIDTH - 1u,
                             controller->viewport_y + NES_FRAME_HEIGHT - 1u);
        transport->begin_pixels();

        for (uint16_t y = 0u; y < NES_FRAME_HEIGHT; ++y) {
            const uint8_t *src = nes_framebuffer_scanline_const(frame, y);
            uint64_t t0 = time_us_64();

            for (uint16_t x = 0u; x < NES_FRAME_WIDTH; ++x) {
                s_line_buffer_be[x] = k_nes_palette_be[src[x] & 0x3fu];
            }
            convert_us += time_us_64() - t0;

            t0 = time_us_64();
            video_tft_write_pixels((const uint8_t *)(const void *)s_line_buffer_be,
                                   NES_FRAME_WIDTH * sizeof(uint16_t));
            bus_us += time_us_64() - t0;

            memcpy(s_prev_frame_be[y], s_line_buffer_be, NES_FRAME_WIDTH * sizeof(uint16_t));
        }

        transport->end_pixels();
        s_stats.bytes_sent += (uint64_t)NES_FRAME_WIDTH * NES_FRAME_HEIGHT * sizeof(uint16_t);
        s_prev_frame_valid = true;
        ++s_stats.full_frames_sent;
    } else {
        uint16_t y_start = s_interlace_enabled ? s_interlace_parity : 0u;
        uint16_t y_step = s_interlace_enabled ? 2u : 1u;
        uint16_t dirty_x0 = NES_FRAME_WIDTH;
        uint16_t dirty_x1 = 0u;
        uint16_t dirty_y0 = NES_FRAME_HEIGHT;
        uint16_t dirty_y1 = 0u;
        bool dirty_any = false;

        for (uint16_t y = y_start; y < NES_FRAME_HEIGHT; y += y_step) {
            const uint8_t *src = nes_framebuffer_scanline_const(frame, y);
            uint64_t t0 = time_us_64();
            uint16_t row_x0;
            uint16_t row_x1;
            bool row_dirty;

            for (uint16_t x = 0u; x < NES_FRAME_WIDTH; ++x) {
                s_line_buffer_be[x] = k_nes_palette_be[src[x] & 0x3fu];
            }
            convert_us += time_us_64() - t0;

            t0 = time_us_64();
            row_dirty = video_tft_find_dirty_range(s_line_buffer_be, s_prev_frame_be[y], &row_x0, &row_x1);
            diff_us += time_us_64() - t0;

            if (row_dirty) {
                memcpy(s_prev_frame_be[y], s_line_buffer_be, NES_FRAME_WIDTH * sizeof(uint16_t));
                if (!dirty_any || row_x0 < dirty_x0) {
                    dirty_x0 = row_x0;
                }
                if (!dirty_any || row_x1 > dirty_x1) {
                    dirty_x1 = row_x1;
                }
                if (!dirty_any || y < dirty_y0) {
                    dirty_y0 = y;
                }
                if (!dirty_any || y > dirty_y1) {
                    dirty_y1 = y;
                }
                dirty_any = true;
            }
        }

        /*
         * Write-only streaming path:
         *   1. Diff against the cached previous RGB565 frame.
         *   2. Open one CASET/PASET/RAMWR window for the bounding dirty rect.
         *   3. Stream rows continuously with no further command traffic.
         *
         * Rows inside the bounding rect that were not freshly diffed this frame
         * (for example skipped interlace rows) are streamed from s_prev_frame_be
         * so the panel still receives a valid contiguous rectangle.
         */
        if (dirty_any) {
            uint16_t rect_w = (uint16_t)(dirty_x1 - dirty_x0 + 1u);
            uint16_t rect_h = (uint16_t)(dirty_y1 - dirty_y0 + 1u);
            uint64_t t0;

            video_tft_set_window((uint16_t)(controller->viewport_x + dirty_x0),
                                 (uint16_t)(controller->viewport_y + dirty_y0),
                                 (uint16_t)(controller->viewport_x + dirty_x1),
                                 (uint16_t)(controller->viewport_y + dirty_y1));
            transport->begin_pixels();
            t0 = time_us_64();
            for (uint16_t y = dirty_y0; y <= dirty_y1; ++y) {
                video_tft_write_pixels(
                    (const uint8_t *)(const void *)&s_prev_frame_be[y][dirty_x0],
                    rect_w * sizeof(uint16_t)
                );
            }
            bus_us += time_us_64() - t0;
            transport->end_pixels();
            s_stats.bytes_sent += (uint64_t)rect_w * rect_h * sizeof(uint16_t);
            frame_rects = 1u;
        }
    }

    if (s_interlace_enabled) {
        s_interlace_parity ^= 1u;
    }

    ++s_stats.frames_presented;
    s_stats.present_us_total += time_us_64() - start_us;
    s_stats.present_convert_us_total += convert_us;
    s_stats.present_diff_us_total += diff_us;
    s_stats.present_bus_us_total += bus_us;
    s_stats.spans_sent_total += frame_rects;
    if ((time_us_64() - start_us) > s_stats.present_us_max) {
        s_stats.present_us_max = time_us_64() - start_us;
    }
}

void video_tft_get_stats(PicoTftStats *stats_out) {
    if (stats_out == NULL) {
        return;
    }
    *stats_out = s_stats;
}
