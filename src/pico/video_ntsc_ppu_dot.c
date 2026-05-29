#include "video_ntsc_ppu_dot.h"

#include "clock_config.h"
#if defined(MICRONES_PICO_VIDEO_MODE_EMULATOR)
#include "core1_video.h"
#include "scanline_queue.h"
#endif
#include "video_ntsc.h"
#include "video_ntsc_ppu_dot.pio.h"

#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#if defined(MICRONES_PICO_VIDEO_MODE_EMULATOR)
#include "pico/multicore.h"
#endif
#include "pico/stdlib.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if MICRONES_VIDEO_SYNC_GPIO != (MICRONES_VIDEO_PIN_BASE + MICRONES_VIDEO_PIN_COUNT)
#error "PPU-dot NTSC test requires the sync gate pin immediately after the DAC pins"
#endif

enum {
    PPU_DOT_WORDS_PER_LINE = 682u,
    PPU_DOT_LINES_PER_FRAME = 262u,

    PPU_DOT_VSYNC_LINES = 9u,
    PPU_DOT_TOP_BLANK_LINES = 11u,
    PPU_DOT_ACTIVE_START_LINE = PPU_DOT_VSYNC_LINES + PPU_DOT_TOP_BLANK_LINES,
    PPU_DOT_ACTIVE_LINES = 240u,
    PPU_DOT_ACTIVE_END_LINE = PPU_DOT_ACTIVE_START_LINE + PPU_DOT_ACTIVE_LINES,

    PPU_DOT_SYNC_DOTS = 50u,
    PPU_DOT_BURST_START = 64u,
    PPU_DOT_BURST_DOTS = 30u,
    PPU_DOT_ACTIVE_START = 120u,
    PPU_DOT_ACTIVE_DOTS = 512u,
};

/* 3 PIO cycles per symbol, clkdiv = 2503 / 256. */
#define PPU_DOT_CLKDIV 9.77734375f
#define PPU_DOT_SYMBOL_SYNC_GATE 0x10u
#define PPU_DOT_ENABLE_COLOR 1
#define PPU_DOT_ENABLE_ACTIVE_CHROMA 1
#define PPU_DOT_TEST_PALETTE_GRID 1

static PIO s_pio;
static uint s_sm;
static uint s_pio_offset;
static uint s_dma_chan[2];
static dma_channel_config s_dma_cfg[2];

static volatile uint s_render_line = 2u;
static volatile bool s_dma_irq1_pending = false;
static volatile int s_idle_buf = 0;
static volatile uint64_t s_frames_rendered = 0;

static uint32_t __attribute__((aligned(4))) s_line_buf[2][PPU_DOT_WORDS_PER_LINE];
static uint32_t __attribute__((aligned(4))) s_vsync_template[PPU_DOT_WORDS_PER_LINE];
static uint32_t __attribute__((aligned(4))) s_blank_template[3][PPU_DOT_WORDS_PER_LINE];
static uint32_t __attribute__((aligned(4))) s_active_template[3][PPU_DOT_WORDS_PER_LINE];
static volatile uint s_render_phase = 2u;
static uint8_t s_palette_symbols[64][3];
#if defined(MICRONES_PICO_VIDEO_MODE_EMULATOR)
static uint8_t s_cached_pixels[PPU_DOT_ACTIVE_LINES][256];
#endif

static inline uint32_t encode_symbol(uint8_t dac_code, bool sync_gate) {
    uint32_t symbol = ((uint32_t)dac_code & 0x0fu) |
                      (sync_gate ? PPU_DOT_SYMBOL_SYNC_GATE : 0u);
    return symbol << 27;
}

static inline uint8_t burst_code(uint dot, uint line_phase) {
    /* 3FSC-sampled 180-degree colorburst around blank:
     * blank + 2*cos(phase + 180deg) -> {2, 5, 5}.  This matches the palette
     * phase convention where NES hue 8 is aligned with burst. */
    static const uint8_t pattern[3] = { 2u, 5u, 5u };
    return pattern[(dot + line_phase) % 3u];
}

static inline uint8_t color_bar_code(uint bar, uint dot, uint line_phase) {
    static const uint8_t bars[8][3] = {
        { 13u, 13u, 13u }, /* white */
        { 11u, 12u, 10u }, /* yellow-ish */
        { 10u,  9u, 11u }, /* cyan-ish */
        {  8u,  9u,  7u }, /* green-ish */
        {  8u,  7u,  9u }, /* magenta-ish */
        {  7u,  6u,  8u }, /* red-ish */
        {  5u,  6u,  4u }, /* blue-ish */
        {  4u,  4u,  4u }, /* black */
    };
    return bars[bar & 7u][(dot + line_phase) % 3u];
}

static inline uint8_t luma_bar_code(uint bar) {
    static const uint8_t luma_bars[8] = { 13u, 11u, 10u, 8u, 8u, 7u, 5u, 4u };
    return luma_bars[bar & 7u];
}

static inline uint8_t palette_code(uint8_t color, uint symbol, uint line_phase) {
    return s_palette_symbols[color & 0x3fu][(symbol + line_phase) % 3u];
}

static inline uint color_bar_for_symbol(uint active_symbol, bool *guard_out) {
    /* Put color transitions on 3-symbol carrier boundaries to reduce dot crawl
     * in this synthetic test pattern. */
    uint bar = active_symbol / 63u;
    uint local = active_symbol % 63u;
    if (guard_out != NULL) {
        *guard_out = (local < 3u || local >= 60u);
    }
    return (bar < 7u) ? bar : 7u;
}

static const uint8_t k_ppu_dot_palette_to_luma[64] = {
    MICRONES_VIDEO_LUMA_MID,         MICRONES_VIDEO_LUMA_DARK,       MICRONES_VIDEO_LUMA_DARK,       MICRONES_VIDEO_LUMA_DARK,
    MICRONES_VIDEO_LUMA_DARK,        MICRONES_VIDEO_LUMA_DARK,       MICRONES_VIDEO_LUMA_DARK,       MICRONES_VIDEO_LUMA_DARK,
    MICRONES_VIDEO_LUMA_DARK,        MICRONES_VIDEO_LUMA_DARK,       MICRONES_VIDEO_LUMA_DARK,       MICRONES_VIDEO_LUMA_DARK,
    MICRONES_VIDEO_LUMA_DARK,        MICRONES_VIDEO_LUMA_BLACK,      MICRONES_VIDEO_LUMA_BLACK,      MICRONES_VIDEO_LUMA_BLACK,

    MICRONES_VIDEO_LUMA_MID_BRIGHT,  MICRONES_VIDEO_LUMA_MID,        MICRONES_VIDEO_LUMA_MID,        MICRONES_VIDEO_LUMA_MID,
    MICRONES_VIDEO_LUMA_MID,         MICRONES_VIDEO_LUMA_MID,        MICRONES_VIDEO_LUMA_MID,        MICRONES_VIDEO_LUMA_MID,
    MICRONES_VIDEO_LUMA_MID,         MICRONES_VIDEO_LUMA_MID,        MICRONES_VIDEO_LUMA_MID,        MICRONES_VIDEO_LUMA_MID,
    MICRONES_VIDEO_LUMA_MID,         MICRONES_VIDEO_LUMA_BLACK,      MICRONES_VIDEO_LUMA_BLACK,      MICRONES_VIDEO_LUMA_BLACK,

    MICRONES_VIDEO_LUMA_WHITE,       MICRONES_VIDEO_LUMA_BRIGHT,     MICRONES_VIDEO_LUMA_BRIGHT,     MICRONES_VIDEO_LUMA_BRIGHT,
    MICRONES_VIDEO_LUMA_BRIGHT,      MICRONES_VIDEO_LUMA_BRIGHT,     MICRONES_VIDEO_LUMA_BRIGHT,     MICRONES_VIDEO_LUMA_BRIGHT,
    MICRONES_VIDEO_LUMA_BRIGHT,      MICRONES_VIDEO_LUMA_BRIGHT,     MICRONES_VIDEO_LUMA_BRIGHT,     MICRONES_VIDEO_LUMA_BRIGHT,
    MICRONES_VIDEO_LUMA_BRIGHT,      MICRONES_VIDEO_LUMA_BLACK,      MICRONES_VIDEO_LUMA_BLACK,      MICRONES_VIDEO_LUMA_BLACK,

    MICRONES_VIDEO_LUMA_WHITE,       MICRONES_VIDEO_LUMA_VERY_BRIGHT, MICRONES_VIDEO_LUMA_VERY_BRIGHT, MICRONES_VIDEO_LUMA_VERY_BRIGHT,
    MICRONES_VIDEO_LUMA_VERY_BRIGHT, MICRONES_VIDEO_LUMA_VERY_BRIGHT, MICRONES_VIDEO_LUMA_VERY_BRIGHT, MICRONES_VIDEO_LUMA_VERY_BRIGHT,
    MICRONES_VIDEO_LUMA_VERY_BRIGHT, MICRONES_VIDEO_LUMA_VERY_BRIGHT, MICRONES_VIDEO_LUMA_VERY_BRIGHT, MICRONES_VIDEO_LUMA_VERY_BRIGHT,
    MICRONES_VIDEO_LUMA_VERY_BRIGHT, MICRONES_VIDEO_LUMA_BLACK,      MICRONES_VIDEO_LUMA_BLACK,      MICRONES_VIDEO_LUMA_BLACK,
};

static void render_vsync_line(uint32_t *buf) {
    for (uint i = 0; i < PPU_DOT_WORDS_PER_LINE; ++i) {
        buf[i] = encode_symbol(VIDEO_DAC_SYNC, true);
    }
}

static void render_test_line(uint line, uint32_t *buf) {
    if (line < PPU_DOT_VSYNC_LINES) {
        render_vsync_line(buf);
        return;
    }

    uint line_phase = line % 3u;
    uint pos = 0u;

    for (; pos < PPU_DOT_SYNC_DOTS; ++pos) {
        buf[pos] = encode_symbol(VIDEO_DAC_SYNC, true);
    }
    for (; pos < PPU_DOT_BURST_START; ++pos) {
        buf[pos] = encode_symbol(VIDEO_DAC_BLANK, false);
    }
#if PPU_DOT_ENABLE_COLOR
    for (; pos < PPU_DOT_BURST_START + PPU_DOT_BURST_DOTS; ++pos) {
        buf[pos] = encode_symbol(burst_code(pos, line_phase), false);
    }
#else
    for (; pos < PPU_DOT_BURST_START + PPU_DOT_BURST_DOTS; ++pos) {
        buf[pos] = encode_symbol(VIDEO_DAC_BLANK, false);
    }
#endif
    for (; pos < PPU_DOT_ACTIVE_START; ++pos) {
        buf[pos] = encode_symbol(VIDEO_DAC_BLANK, false);
    }

    bool active = line >= PPU_DOT_ACTIVE_START_LINE && line < PPU_DOT_ACTIVE_END_LINE;
    for (uint active_dot = 0; active_dot < PPU_DOT_ACTIVE_DOTS; ++active_dot, ++pos) {
        if (active) {
#if PPU_DOT_TEST_PALETTE_GRID
            const uint margin = 4u;
            const uint cell_w = 42u;
            uint code = VIDEO_DAC_BLANK;
            if (active_dot >= margin && active_dot < margin + 12u * cell_w) {
                uint cell_x = active_dot - margin;
                uint col = cell_x / cell_w;
                uint local = cell_x % cell_w;
                bool col_guard = local < 3u || local >= (cell_w - 3u);
                if (!col_guard) {
                    uint8_t color = (uint8_t)(0x20u | (col + 1u));
                    code = palette_code(color, pos, line_phase);
                }
            }
            buf[pos] = encode_symbol(code, false);
#else
            bool transition_guard = false;
            uint bar = color_bar_for_symbol(active_dot, &transition_guard);
#if PPU_DOT_ENABLE_COLOR && PPU_DOT_ENABLE_ACTIVE_CHROMA
            if (transition_guard) {
                buf[pos] = encode_symbol(luma_bar_code(bar), false);
            } else {
                buf[pos] = encode_symbol(color_bar_code(bar, pos, line_phase), false);
            }
#else
            buf[pos] = encode_symbol(luma_bar_code(bar), false);
#endif
#endif
        } else {
            buf[pos] = encode_symbol(VIDEO_DAC_BLANK, false);
        }
    }

    for (; pos < PPU_DOT_WORDS_PER_LINE; ++pos) {
        buf[pos] = encode_symbol(VIDEO_DAC_BLANK, false);
    }
}

static void render_blank_line_phase(uint32_t *buf, uint line_phase) {
    uint pos = 0u;

    for (; pos < PPU_DOT_SYNC_DOTS; ++pos) {
        buf[pos] = encode_symbol(VIDEO_DAC_SYNC, true);
    }
    for (; pos < PPU_DOT_BURST_START; ++pos) {
        buf[pos] = encode_symbol(VIDEO_DAC_BLANK, false);
    }
    for (; pos < PPU_DOT_BURST_START + PPU_DOT_BURST_DOTS; ++pos) {
        buf[pos] = encode_symbol(burst_code(pos, line_phase), false);
    }
    for (; pos < PPU_DOT_WORDS_PER_LINE; ++pos) {
        buf[pos] = encode_symbol(VIDEO_DAC_BLANK, false);
    }
}

static void render_active_line_phase(uint32_t *buf, const uint8_t *pixels, uint line_phase) {
    uint pos = 0u;

    for (; pos < PPU_DOT_SYNC_DOTS; ++pos) {
        buf[pos] = encode_symbol(VIDEO_DAC_SYNC, true);
    }
    for (; pos < PPU_DOT_BURST_START; ++pos) {
        buf[pos] = encode_symbol(VIDEO_DAC_BLANK, false);
    }
    for (; pos < PPU_DOT_BURST_START + PPU_DOT_BURST_DOTS; ++pos) {
        buf[pos] = encode_symbol(burst_code(pos, line_phase), false);
    }
    for (; pos < PPU_DOT_ACTIVE_START; ++pos) {
        buf[pos] = encode_symbol(VIDEO_DAC_BLANK, false);
    }

    for (uint s = 0u; s < PPU_DOT_ACTIVE_DOTS; ++s, ++pos) {
        uint pixel = s >> 1u;
        if (pixel >= 256u) {
            pixel = 255u;
        }
        buf[pos] = encode_symbol(palette_code(pixels[pixel], pos, line_phase), false);
    }

    for (; pos < PPU_DOT_WORDS_PER_LINE; ++pos) {
        buf[pos] = encode_symbol(VIDEO_DAC_BLANK, false);
    }
}

static const uint32_t *template_for_line(uint line, uint phase) {
    if (line < PPU_DOT_VSYNC_LINES) {
        return s_vsync_template;
    }

    if (line >= PPU_DOT_ACTIVE_START_LINE && line < PPU_DOT_ACTIVE_END_LINE) {
        return s_active_template[phase];
    }

    return s_blank_template[phase];
}

static void build_templates(void) {
    render_test_line(0u, s_vsync_template);

    for (uint phase = 0u; phase < 3u; ++phase) {
        uint blank_line = PPU_DOT_VSYNC_LINES + phase;
        uint active_line = PPU_DOT_ACTIVE_START_LINE + phase;
        render_test_line(active_line, s_active_template[phase]);
        render_test_line(blank_line, s_blank_template[phase]);
    }
}

static void dma_rearm_from(int buf_idx, const uint32_t *line_buf) {
    dma_channel_configure(
        s_dma_chan[buf_idx],
        &s_dma_cfg[buf_idx],
        &s_pio->txf[s_sm],
        line_buf,
        PPU_DOT_WORDS_PER_LINE,
        false
    );
}

static void dma_rearm_buf(int buf_idx) {
    dma_rearm_from(buf_idx, s_line_buf[buf_idx]);
}

static void dma_abort_pair_cleanly(void) {
    uint32_t mask = (1u << s_dma_chan[0]) | (1u << s_dma_chan[1]);
    hw_clear_bits(&dma_hw->ch[s_dma_chan[0]].ctrl_trig, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_clear_bits(&dma_hw->ch[s_dma_chan[1]].ctrl_trig, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_channel_abort(s_dma_chan[0]);
    dma_channel_abort(s_dma_chan[1]);
    dma_hw->ints0 = mask;
    dma_hw->ints1 = mask;
}

static void __isr dma_irq0_handler(void) {
    uint32_t mask = (1u << s_dma_chan[0]) | (1u << s_dma_chan[1]);
    uint32_t status = dma_hw->ints0 & mask;
    dma_hw->ints0 = status;

    int idle = -1;
    if (status & (1u << s_dma_chan[0])) {
        idle = 0;
    } else if (status & (1u << s_dma_chan[1])) {
        idle = 1;
    }

    if (idle >= 0) {
        uint line = s_render_line;
        uint phase = s_render_phase;
        dma_rearm_from(idle, template_for_line(line, phase));

        line++;
        if (line >= PPU_DOT_LINES_PER_FRAME) {
            line = 0u;
        }
        s_render_line = line;

        phase++;
        if (phase >= 3u) {
            phase = 0u;
        }
        s_render_phase = phase;
    }
}

#if defined(MICRONES_PICO_VIDEO_MODE_EMULATOR)
static void __isr dma_irq1_handler(void) {
    uint32_t mask = (1u << s_dma_chan[0]) | (1u << s_dma_chan[1]);
    uint32_t status = dma_hw->ints1 & mask;
    dma_hw->ints1 = status;

    if (status & (1u << s_dma_chan[0])) {
        s_idle_buf = 0;
    } else if (status & (1u << s_dma_chan[1])) {
        s_idle_buf = 1;
    }

    s_dma_irq1_pending = true;
    __sev();
}
#endif

static void init_common(void) {
    s_pio = pio0;
    s_sm = 0;
    s_pio_offset = pio_add_program(s_pio, &video_ntsc_ppu_dot_program);
    video_ntsc_ppu_dot_program_init(s_pio, s_sm, s_pio_offset,
                                    MICRONES_VIDEO_PIN_BASE, PPU_DOT_CLKDIV);

    s_dma_chan[0] = dma_claim_unused_channel(true);
    s_dma_chan[1] = dma_claim_unused_channel(true);

    for (int i = 0; i < 2; ++i) {
        s_dma_cfg[i] = dma_channel_get_default_config(s_dma_chan[i]);
        channel_config_set_transfer_data_size(&s_dma_cfg[i], DMA_SIZE_32);
        channel_config_set_read_increment(&s_dma_cfg[i], true);
        channel_config_set_write_increment(&s_dma_cfg[i], false);
        channel_config_set_dreq(&s_dma_cfg[i], pio_get_dreq(s_pio, s_sm, true));
        channel_config_set_chain_to(&s_dma_cfg[i], s_dma_chan[1 - i]);
        dma_channel_set_irq0_enabled(s_dma_chan[i], true);
    }
}

void video_ntsc_ppu_dot_test_init(void) {
    init_common();
}

void video_ntsc_ppu_dot_emulator_init(void) {
    init_common();
}

void video_ntsc_ppu_dot_test_start(void) {
    video_ntsc_ppu_dot_precompute_palette(k_ppu_dot_palette_to_luma, 64);
    build_templates();
    s_render_line = 2u;
    s_render_phase = 2u;

    dma_rearm_from(0, template_for_line(0u, 0u));
    dma_rearm_from(1, template_for_line(1u, 1u));

    uint32_t mask = (1u << s_dma_chan[0]) | (1u << s_dma_chan[1]);
    dma_hw->ints0 = mask;
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq0_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_clear_fifos(s_pio, s_sm);
    pio_sm_restart(s_pio, s_sm);
    video_ntsc_ppu_dot_program_init(s_pio, s_sm, s_pio_offset,
                                    MICRONES_VIDEO_PIN_BASE, PPU_DOT_CLKDIV);
    pio_sm_exec(s_pio, s_sm, pio_encode_jmp(s_pio_offset));

    pio_sm_set_enabled(s_pio, s_sm, true);
    dma_channel_start(s_dma_chan[0]);
}

void video_ntsc_ppu_dot_precompute_palette(const uint8_t *palette_to_luma, int palette_size) {
    for (int c = 0; c < 64; ++c) {
        int luma = (c < palette_size) ? (int)palette_to_luma[c] : 0;
        int hue = c & 0x0f;
        if (hue > 12) {
            hue = 0;
        }
        int row = (c >> 4) & 3;

        if (hue >= 1 && hue <= 4 && row >= 1 && row <= 2 && luma > 0) {
            luma--;
        }
        if (hue >= 5 && hue <= 7 && row >= 1 && row <= 2 && luma > 0) {
            luma--;
        }

        int base = (int)VIDEO_DAC_BLANK + (luma * (int)VIDEO_LUMA_SCALE) / 7;
        float amp = (hue == 0) ? 0.0f : 1.35f;
        if (row == 0) {
            amp *= 0.65f;
        } else if (row == 3) {
            amp *= 0.30f;
        }

        /* Calibrated from the $21-$2C analog palette grid:
         * current hue 4 reads as blue, while SMB sky uses hue 2; current
         * hues 11/12 read green/yellow-green, while SMB pipes live near
         * hues 9/A.  Rotate active palette phase by two hue columns from
         * the previous trial so these families land closer together. */
        int phase_hue = hue - 3;
        if (hue == 9 || hue == 10) {
            phase_hue += 1;
            amp *= 1.12f;
        } else if (hue == 11 || hue == 12) {
            phase_hue += 1;
        } else if (hue >= 5 && hue <= 7) {
            amp *= 1.18f;
        }
        float phase_rad = (float)phase_hue * 30.0f * (float)M_PI / 180.0f;
        for (int s = 0; s < 3; ++s) {
            float carrier = (float)s * 2.0f * (float)M_PI / 3.0f;
            int dac = base + (int)roundf(amp * cosf(carrier + phase_rad));
            if (dac < 0) {
                dac = 0;
            } else if (dac > 15) {
                dac = 15;
            }
            s_palette_symbols[c][s] = (uint8_t)dac;
        }
    }
}

#if defined(MICRONES_PICO_VIDEO_MODE_EMULATOR)
void video_ntsc_ppu_dot_start(void) {
    render_vsync_line(s_line_buf[0]);
    render_vsync_line(s_line_buf[1]);

    dma_rearm_buf(0);
    dma_rearm_buf(1);

    uint32_t mask = (1u << s_dma_chan[0]) | (1u << s_dma_chan[1]);
    dma_hw->ints0 = mask;
    dma_hw->ints1 = mask;
    dma_set_irq0_channel_mask_enabled(mask, false);
    dma_set_irq1_channel_mask_enabled(mask, false);

    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_clear_fifos(s_pio, s_sm);
    pio_sm_restart(s_pio, s_sm);
    video_ntsc_ppu_dot_program_init(s_pio, s_sm, s_pio_offset,
                                    MICRONES_VIDEO_PIN_BASE, PPU_DOT_CLKDIV);
    pio_sm_exec(s_pio, s_sm, pio_encode_jmp(s_pio_offset));

    pio_sm_set_enabled(s_pio, s_sm, true);
    dma_channel_start(s_dma_chan[0]);
}

void video_ntsc_ppu_dot_stop(void) {
    uint32_t mask = (1u << s_dma_chan[0]) | (1u << s_dma_chan[1]);
    dma_set_irq0_channel_mask_enabled(mask, false);
    dma_set_irq1_channel_mask_enabled(mask, false);
    irq_set_enabled(DMA_IRQ_0, false);
    irq_set_enabled(DMA_IRQ_1, false);
    dma_abort_pair_cleanly();
    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_clear_fifos(s_pio, s_sm);
    pio_sm_restart(s_pio, s_sm);
    s_dma_irq1_pending = false;
}

void video_ntsc_ppu_dot_core1_entry(void) {
    uint32_t ch_mask = (1u << s_dma_chan[0]) | (1u << s_dma_chan[1]);

    multicore_lockout_victim_init();

    dma_set_irq0_channel_mask_enabled(ch_mask, false);
    dma_set_irq1_channel_mask_enabled(ch_mask, false);
    irq_set_exclusive_handler(DMA_IRQ_1, dma_irq1_handler);
    irq_set_enabled(DMA_IRQ_1, true);

    ScanlineQueue *queue = core1_video_get_queue();

    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_clear_fifos(s_pio, s_sm);
    pio_sm_restart(s_pio, s_sm);
    video_ntsc_ppu_dot_program_init(s_pio, s_sm, s_pio_offset,
                                    MICRONES_VIDEO_PIN_BASE, PPU_DOT_CLKDIV);
    pio_sm_exec(s_pio, s_sm, pio_encode_jmp(s_pio_offset));

    dma_abort_pair_cleanly();

    render_vsync_line(s_line_buf[0]);
    render_vsync_line(s_line_buf[1]);
    dma_rearm_buf(0);
    dma_rearm_buf(1);

    dma_hw->ints1 = ch_mask;
    s_dma_irq1_pending = false;
    dma_set_irq1_channel_mask_enabled(ch_mask, true);

    pio_sm_set_enabled(s_pio, s_sm, true);
    dma_channel_start(s_dma_chan[0]);

    int render_line = 2;
    uint render_phase = 2u;

    while (true) {
        while (!s_dma_irq1_pending) {
            __wfe();
        }
        s_dma_irq1_pending = false;

        if (render_line < (int)PPU_DOT_VSYNC_LINES) {
            render_vsync_line(s_line_buf[s_idle_buf]);
        } else if (render_line < (int)PPU_DOT_ACTIVE_START_LINE ||
                   render_line >= (int)PPU_DOT_ACTIVE_END_LINE) {
            render_blank_line_phase(s_line_buf[s_idle_buf], render_phase);
        } else {
            ScanlineQueueSlot slot;
            for (uint pulls = 0u; pulls < 4u && scanline_queue_try_pop(queue, &slot); ++pulls) {
                if (slot.y < PPU_DOT_ACTIVE_LINES) {
                    memcpy(s_cached_pixels[slot.y], slot.pixels, 256u);
                }
            }
            uint active_y = (uint)(render_line - (int)PPU_DOT_ACTIVE_START_LINE);
            render_active_line_phase(s_line_buf[s_idle_buf],
                                     s_cached_pixels[active_y],
                                     render_phase);
            if (render_line == (int)PPU_DOT_ACTIVE_END_LINE - 1) {
                s_frames_rendered++;
            }
        }

        dma_rearm_buf(s_idle_buf);

        render_line++;
        if (render_line >= (int)PPU_DOT_LINES_PER_FRAME) {
            render_line = 0;
        }

        render_phase++;
        if (render_phase >= 3u) {
            render_phase = 0u;
        }
    }
}
#endif
