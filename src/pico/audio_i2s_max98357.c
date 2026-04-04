#include "audio_i2s_max98357.h"

#include "audio_i2s_max98357.pio.h"
#include <stdio.h>
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

enum {
    AUDIO_PCM_RING_SIZE = 4096u,
    AUDIO_DMA_BLOCK_SAMPLES = 128u,
    AUDIO_DMA_BLOCK_WORDS = AUDIO_DMA_BLOCK_SAMPLES * 4u,
    AUDIO_PIO_FRAME_CYCLES = 133u,
};

static int16_t s_pcm_ring[AUDIO_PCM_RING_SIZE];
static volatile uint32_t s_pcm_head = 0u;
static volatile uint32_t s_pcm_tail = 0u;
static volatile uint32_t s_audio_underruns = 0u;
static volatile uint32_t s_audio_overruns = 0u;
static int16_t s_last_sample = 0;

static uint32_t s_dma_blocks[2][AUDIO_DMA_BLOCK_WORDS];

static PIO s_pio = pio0;
static uint s_sm = 0u;
static int s_pio_offset = -1;
static int s_dma_chan_a = -1;
static int s_dma_chan_b = -1;

static inline uint32_t audio_pcm_ring_level(void) {
    return (s_pcm_tail - s_pcm_head) & (AUDIO_PCM_RING_SIZE - 1u);
}

static uint32_t audio_i2s_pack_symbols(uint32_t packed, uint32_t *bit_index, bool ws_high) {
    const uint32_t ws_bits = ws_high ? 2u : 0u;
    uint32_t out = packed;
    /* The caller places the last data symbol in bits [1:0] of `packed` before
     * calling here.  15 further left-shifts move it to bits [31:30] (the first
     * position transmitted with MSB-first shift) and fill the remaining 30 bits
     * with 15 zero-pad symbols.  16 shifts would push it off the uint32_t. */
    for (uint32_t i = 0; i < 15u; ++i) {
        out <<= 2u;
        out |= ws_bits;
        ++(*bit_index);
    }
    return out;
}

static void audio_i2s_encode_sample(int16_t sample, uint32_t *dst_words) {
    uint16_t bits = (uint16_t)sample;
    uint32_t word = 0u;
    uint32_t bit_index = 0u;

    /* Left slot: one dummy bit with WS low, then 16 sample bits, then 15 zero pad bits. */
    word <<= 2u;
    ++bit_index;
    for (uint32_t i = 0; i < 16u; ++i) {
        word <<= 2u;
        word |= (uint32_t)((bits & 0x8000u) ? 1u : 0u);
        bits <<= 1u;
        ++bit_index;
        if ((bit_index & 15u) == 0u) {
            *dst_words++ = word;
            word = 0u;
        }
    }
    word = audio_i2s_pack_symbols(word, &bit_index, false);
    *dst_words++ = word;

    /* Right slot: one dummy bit with WS high, then 16 sample bits, then 15 zero pad bits. */
    bits = (uint16_t)sample;
    word = 2u;
    bit_index = 1u;
    for (uint32_t i = 0; i < 16u; ++i) {
        word <<= 2u;
        word |= 2u | (uint32_t)((bits & 0x8000u) ? 1u : 0u);
        bits <<= 1u;
        ++bit_index;
        if ((bit_index & 15u) == 0u) {
            *dst_words++ = word;
            word = 0u;
        }
    }
    word = audio_i2s_pack_symbols(word, &bit_index, true);
    *dst_words++ = word;
}

static void audio_i2s_fill_dma_block(uint32_t *dst_words) {
    uint32_t head = s_pcm_head;

    for (uint32_t i = 0; i < AUDIO_DMA_BLOCK_SAMPLES; ++i) {
        int16_t sample;
        if (head == s_pcm_tail) {
            sample = s_last_sample;
            ++s_audio_underruns;
        } else {
            sample = s_pcm_ring[head];
            s_last_sample = sample;
            head = (head + 1u) & (AUDIO_PCM_RING_SIZE - 1u);
        }
        audio_i2s_encode_sample(sample, &dst_words[i * 4u]);
    }

    s_pcm_head = head;
}

static void audio_i2s_dma_irq_handler(void) {
    const uint32_t mask_a = 1u << (uint32_t)s_dma_chan_a;
    const uint32_t mask_b = 1u << (uint32_t)s_dma_chan_b;
    uint32_t status = dma_hw->ints0 & (mask_a | mask_b);

    if (status == 0u) {
        return;
    }

    dma_hw->ints0 = status;

    if ((status & mask_a) != 0u) {
        audio_i2s_fill_dma_block(s_dma_blocks[0]);
        dma_channel_set_read_addr((uint)s_dma_chan_a, s_dma_blocks[0], false);
        dma_channel_set_trans_count((uint)s_dma_chan_a, AUDIO_DMA_BLOCK_WORDS, false);
    }
    if ((status & mask_b) != 0u) {
        audio_i2s_fill_dma_block(s_dma_blocks[1]);
        dma_channel_set_read_addr((uint)s_dma_chan_b, s_dma_blocks[1], false);
        dma_channel_set_trans_count((uint)s_dma_chan_b, AUDIO_DMA_BLOCK_WORDS, false);
    }
}

void audio_i2s_max98357_init(uint32_t sample_rate) {
    pio_sm_config config;
    dma_channel_config dma_cfg_a;
    dma_channel_config dma_cfg_b;
    const float effective_sample_rate = (float)(sample_rate != 0u ? sample_rate : 48000u);
    /*
     * The PIO program spends 133 state-machine cycles per stereo frame:
     *   set y          = 1
     *   left  set x    = 1
     *   left  32 bits  = 64   (32 × out+jmp pairs)
     *   inter-half jmp = 1
     *   right set x    = 1
     *   right 32 bits  = 64
     *   wrap           = 1    (jmp y-- falling through on y=0, free wrap)
     *                  ───
     *                  133
     * To keep LRCLK at the requested sample rate, derive clkdiv from the true
     * frame-cycle count rather than the idealized 128-bit-only count.
     */
    const float sm_clk_hz = effective_sample_rate * (float)AUDIO_PIO_FRAME_CYCLES;
    const float clk_div = (float)clock_get_hz(clk_sys) / sm_clk_hz;

    s_pcm_head = 0u;
    s_pcm_tail = 0u;
    s_audio_underruns = 0u;
    s_audio_overruns = 0u;
    s_last_sample = 0;

    if (s_pio_offset < 0) {
        s_pio_offset = (int)pio_add_program(s_pio, &audio_i2s_max98357_program);
    }

    pio_gpio_init(s_pio, MICRONES_MAX98357_BCLK_PIN);
    pio_gpio_init(s_pio, MICRONES_MAX98357_DIN_PIN);
    pio_gpio_init(s_pio, MICRONES_MAX98357_LRCLK_PIN);
    pio_sm_set_consecutive_pindirs(s_pio, s_sm, MICRONES_MAX98357_BCLK_PIN, 1u, true);
    pio_sm_set_consecutive_pindirs(s_pio, s_sm, MICRONES_MAX98357_DIN_PIN, 2u, true);

    config = audio_i2s_max98357_program_get_default_config((uint)s_pio_offset);
    sm_config_set_sideset_pins(&config, MICRONES_MAX98357_BCLK_PIN);
    sm_config_set_out_pins(&config, MICRONES_MAX98357_DIN_PIN, 2u);
    /* shift_right=false → shift OSR LEFT → MSB first.
     * The encoding packs words with the first I2S symbol in bits [31:30] and
     * the last in bits [1:0].  Left-shift (shift_right=false) emits bits
     * [31:30] first, which is MSB-first / standard I2S order.
     * Right-shift (shift_right=true) would emit LSB first, reversing the
     * entire bit stream. */
    sm_config_set_out_shift(&config, false, true, 32u);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&config, clk_div);

    pio_sm_init(s_pio, s_sm, (uint)s_pio_offset, &config);
    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_clear_fifos(s_pio, s_sm);
    pio_sm_restart(s_pio, s_sm);

    audio_i2s_fill_dma_block(s_dma_blocks[0]);
    audio_i2s_fill_dma_block(s_dma_blocks[1]);

    if (s_dma_chan_a < 0) {
        s_dma_chan_a = dma_claim_unused_channel(true);
    }
    if (s_dma_chan_b < 0) {
        s_dma_chan_b = dma_claim_unused_channel(true);
    }

    dma_cfg_a = dma_channel_get_default_config((uint)s_dma_chan_a);
    channel_config_set_transfer_data_size(&dma_cfg_a, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_cfg_a, true);
    channel_config_set_write_increment(&dma_cfg_a, false);
    channel_config_set_dreq(&dma_cfg_a, pio_get_dreq(s_pio, s_sm, true));
    channel_config_set_chain_to(&dma_cfg_a, (uint)s_dma_chan_b);

    dma_cfg_b = dma_cfg_a;
    channel_config_set_chain_to(&dma_cfg_b, (uint)s_dma_chan_a);

    dma_channel_configure((uint)s_dma_chan_a, &dma_cfg_a,
                          &s_pio->txf[s_sm], s_dma_blocks[0],
                          AUDIO_DMA_BLOCK_WORDS, false);
    dma_channel_configure((uint)s_dma_chan_b, &dma_cfg_b,
                          &s_pio->txf[s_sm], s_dma_blocks[1],
                          AUDIO_DMA_BLOCK_WORDS, false);

    dma_channel_set_irq0_enabled((uint)s_dma_chan_a, true);
    dma_channel_set_irq0_enabled((uint)s_dma_chan_b, true);
    irq_set_exclusive_handler(DMA_IRQ_0, audio_i2s_dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    pio_sm_set_enabled(s_pio, s_sm, true);
    dma_start_channel_mask((1u << (uint)s_dma_chan_a));
}

size_t audio_i2s_max98357_push_samples(const int16_t *samples, size_t count) {
    size_t written = 0u;
    uint32_t tail = s_pcm_tail;

    for (size_t i = 0; i < count; ++i) {
        uint32_t next_tail = (tail + 1u) & (AUDIO_PCM_RING_SIZE - 1u);
        if (next_tail == s_pcm_head) {
            /* Buffer full: evict the oldest sample to make room for the new one.
             * Briefly disable interrupts so the DMA ISR cannot advance s_pcm_head
             * concurrently — a torn read-modify-write here would corrupt the head
             * pointer.  The critical section is only ~4 instructions (~16 ns). */
            uint32_t save = save_and_disable_interrupts();
            s_pcm_head = (s_pcm_head + 1u) & (AUDIO_PCM_RING_SIZE - 1u);
            restore_interrupts(save);
            ++s_audio_overruns;
        }
        s_pcm_ring[tail] = samples[i];
        tail = next_tail;
        ++written;
    }

    s_pcm_tail = tail;
    return written;
}

uint32_t audio_i2s_max98357_underrun_count(void) {
    return s_audio_underruns;
}

uint32_t audio_i2s_max98357_buffer_level(void) {
    return audio_pcm_ring_level();
}

uint32_t audio_i2s_max98357_overrun_count(void) {
    return s_audio_overruns;
}
