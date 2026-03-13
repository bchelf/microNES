#include "video_ntsc.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "video_ntsc.pio.h"

#include <string.h>

// This is a simplified 240p-style NTSC cadence for bring-up:
// 262 total lines, 512 timed samples per line, and a broad-sync vertical region.
// It is intentionally not full interlaced broadcast timing; the goal is a stable
// monochrome lock on real composite displays with the two-resistor DAC.
enum {
    VIDEO_VSYNC_LINES = 3,
    VIDEO_TOP_BLANK_LINES = 16,
    VIDEO_VISIBLE_LINES = 240,
    VIDEO_BOTTOM_BLANK_LINES = 3,
    VIDEO_LINES_PER_FRAME = VIDEO_VSYNC_LINES + VIDEO_TOP_BLANK_LINES +
                            VIDEO_VISIBLE_LINES + VIDEO_BOTTOM_BLANK_LINES,
    VIDEO_SAMPLES_PER_LINE = 512,
    VIDEO_WORDS_PER_LINE = VIDEO_SAMPLES_PER_LINE / 16,
    VIDEO_FRAME_WORDS = VIDEO_LINES_PER_FRAME * VIDEO_WORDS_PER_LINE,
    VIDEO_HSYNC_SAMPLES = 38,
    VIDEO_BACK_PORCH_SAMPLES = 38,
    VIDEO_FRONT_PORCH_SAMPLES = 12,
    VIDEO_ACTIVE_SAMPLES = VIDEO_SAMPLES_PER_LINE - VIDEO_HSYNC_SAMPLES -
                           VIDEO_BACK_PORCH_SAMPLES - VIDEO_FRONT_PORCH_SAMPLES,
    VIDEO_SAMPLE_RATE_HZ = 8056010,
    VIDEO_BORDER_X = 10,
    VIDEO_BORDER_Y = 8,
};

typedef enum {
    VIDEO_LEVEL_SYNC = 0x0,
    VIDEO_LEVEL_BLACK = 0x1,
    VIDEO_LEVEL_WHITE = 0x3,
} video_level_t;

static uint32_t frame_words[VIDEO_FRAME_WORDS];
static int video_sm;
static int video_dma_data;

static inline uint32_t *video_line_ptr(int line) {
    return &frame_words[line * VIDEO_WORDS_PER_LINE];
}

static void video_put_sample(uint32_t *line_words, int sample_index, video_level_t level) {
    uint word_index = (uint)sample_index >> 4;
    uint bit_index = ((uint)sample_index & 0xfu) * 2u;
    uint32_t mask = 0x3u << bit_index;
    line_words[word_index] = (line_words[word_index] & ~mask) | ((uint32_t)level << bit_index);
}

static void video_fill_range(uint32_t *line_words, int start, int count, video_level_t level) {
    for (int sample = start; sample < start + count; ++sample) {
        video_put_sample(line_words, sample, level);
    }
}

static bool video_pattern_pixel(int x, int y) {
    if (x < VIDEO_BORDER_X || x >= VIDEO_ACTIVE_SAMPLES - VIDEO_BORDER_X ||
        y < VIDEO_BORDER_Y || y >= VIDEO_VISIBLE_LINES - VIDEO_BORDER_Y) {
        return true;
    }

    if ((x >= VIDEO_ACTIVE_SAMPLES / 2 - 2 && x <= VIDEO_ACTIVE_SAMPLES / 2 + 2) ||
        (y >= VIDEO_VISIBLE_LINES / 2 - 2 && y <= VIDEO_VISIBLE_LINES / 2 + 2)) {
        return true;
    }

    if (y >= 24 && y < 96) {
        int local_x = x - 24;
        if (local_x >= 0) {
            int bar = (local_x / 34) & 1;
            if (bar == 0 && x < VIDEO_ACTIVE_SAMPLES - 24) {
                return true;
            }
        }
    }

    if (x >= 280 && x < 376) {
        int local_y = y - 32;
        if (local_y >= 0 && local_y < 144) {
            int bar = (local_y / 18) & 1;
            if (bar == 0) {
                return true;
            }
        }
    }

    return false;
}

static void video_build_line(uint32_t *line_words, int frame_line) {
    memset(line_words, 0, VIDEO_WORDS_PER_LINE * sizeof(uint32_t));

    // A short broad-sync region is simpler than full equalizing/serration pulses
    // and is usually good enough for first-pass NTSC monitor bring-up.
    if (frame_line < VIDEO_VSYNC_LINES) {
        video_fill_range(line_words, 0, VIDEO_SAMPLES_PER_LINE, VIDEO_LEVEL_SYNC);
        return;
    }

    video_fill_range(line_words, 0, VIDEO_HSYNC_SAMPLES, VIDEO_LEVEL_SYNC);
    video_fill_range(line_words, VIDEO_HSYNC_SAMPLES, VIDEO_BACK_PORCH_SAMPLES, VIDEO_LEVEL_BLACK);
    video_fill_range(
        line_words,
        VIDEO_HSYNC_SAMPLES + VIDEO_BACK_PORCH_SAMPLES,
        VIDEO_ACTIVE_SAMPLES,
        VIDEO_LEVEL_BLACK
    );
    video_fill_range(
        line_words,
        VIDEO_HSYNC_SAMPLES + VIDEO_BACK_PORCH_SAMPLES + VIDEO_ACTIVE_SAMPLES,
        VIDEO_FRONT_PORCH_SAMPLES,
        VIDEO_LEVEL_BLACK
    );

    int visible_line = frame_line - (VIDEO_VSYNC_LINES + VIDEO_TOP_BLANK_LINES);
    if (visible_line < 0 || visible_line >= VIDEO_VISIBLE_LINES) {
        return;
    }

    int active_start = VIDEO_HSYNC_SAMPLES + VIDEO_BACK_PORCH_SAMPLES;
    for (int x = 0; x < VIDEO_ACTIVE_SAMPLES; ++x) {
        if (video_pattern_pixel(x, visible_line)) {
            video_put_sample(line_words, active_start + x, VIDEO_LEVEL_WHITE);
        }
    }
}

static void video_build_frame(void) {
    for (int line = 0; line < VIDEO_LINES_PER_FRAME; ++line) {
        video_build_line(video_line_ptr(line), line);
    }
}

static void video_restart_dma(void) {
    dma_channel_set_read_addr(video_dma_data, frame_words, false);
    dma_channel_set_trans_count(video_dma_data, VIDEO_FRAME_WORDS, true);
}

static void video_dma_handler(void) {
    dma_hw->ints0 = 1u << video_dma_data;
    video_restart_dma();
}

void video_ntsc_init(void) {
    PIO pio = pio0;
    uint offset = pio_add_program(pio, &video_ntsc_program);
    video_sm = pio_claim_unused_sm(pio, true);

    for (uint pin = SMB2350_VIDEO_PIN_BASE; pin < SMB2350_VIDEO_PIN_BASE + SMB2350_VIDEO_PIN_COUNT; ++pin) {
        pio_gpio_init(pio, pin);
    }

    pio_sm_config config = video_ntsc_program_get_default_config(offset);
    sm_config_set_out_pins(&config, SMB2350_VIDEO_PIN_BASE, SMB2350_VIDEO_PIN_COUNT);
    sm_config_set_out_shift(&config, true, true, 32);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&config, (float)clock_get_hz(clk_sys) / (float)VIDEO_SAMPLE_RATE_HZ);

    pio_sm_set_consecutive_pindirs(pio, video_sm, SMB2350_VIDEO_PIN_BASE, SMB2350_VIDEO_PIN_COUNT, true);
    pio_sm_init(pio, video_sm, offset, &config);

    video_build_frame();

    video_dma_data = dma_claim_unused_channel(true);

    dma_channel_config data_config = dma_channel_get_default_config(video_dma_data);
    channel_config_set_transfer_data_size(&data_config, DMA_SIZE_32);
    channel_config_set_read_increment(&data_config, true);
    channel_config_set_write_increment(&data_config, false);
    channel_config_set_dreq(&data_config, pio_get_dreq(pio, video_sm, true));

    dma_channel_configure(
        video_dma_data,
        &data_config,
        &pio->txf[video_sm],
        frame_words,
        VIDEO_FRAME_WORDS,
        false
    );

    dma_channel_set_irq0_enabled(video_dma_data, true);
    irq_set_exclusive_handler(DMA_IRQ_0, video_dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);
}

void video_ntsc_start(void) {
    PIO pio = pio0;

    pio_sm_set_enabled(pio, video_sm, true);
    video_restart_dma();
}
