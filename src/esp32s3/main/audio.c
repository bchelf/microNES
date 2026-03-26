#include "audio.h"
#include "board.h"

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include <stdint.h>
#include <string.h>

static const char *TAG = "audio";

// ─────────────────────────────────────────────────────────────
//  Ring buffer  (int16_t samples, power-of-two size)
// ─────────────────────────────────────────────────────────────
#define AUDIO_BUF_SAMPLES  4096   // ~85 ms at 48 kHz
#define AUDIO_BUF_MASK     (AUDIO_BUF_SAMPLES - 1)

static volatile int16_t  s_buf[AUDIO_BUF_SAMPLES];
static volatile uint32_t s_head = 0;   // written by emulator task
static volatile uint32_t s_tail = 0;   // read    by audio task
static AudioStats        s_stats = {0};

static i2s_chan_handle_t s_tx_chan;

// ─────────────────────────────────────────────────────────────
//  Audio task – drains the ring buffer into the I2S DMA FIFO
// ─────────────────────────────────────────────────────────────
#define AUDIO_CHUNK_SAMPLES  256

static void audio_task(void *arg)
{
    static int16_t chunk[AUDIO_CHUNK_SAMPLES];

    while (1) {
        uint32_t head = s_head;
        uint32_t tail = s_tail;
        uint32_t avail = (head - tail) & AUDIO_BUF_MASK;

        if (avail == 0) {
            // Underrun: send silence and wait briefly
            memset(chunk, 0, sizeof(chunk));
            size_t written;
            i2s_channel_write(s_tx_chan, chunk,
                              AUDIO_CHUNK_SAMPLES * sizeof(int16_t),
                              &written, portMAX_DELAY);
            continue;
        }

        uint32_t count = avail < AUDIO_CHUNK_SAMPLES ? avail : AUDIO_CHUNK_SAMPLES;
        for (uint32_t i = 0; i < count; i++) {
            chunk[i] = s_buf[(tail + i) & AUDIO_BUF_MASK];
        }
        s_tail = (tail + count) & AUDIO_BUF_MASK;

        size_t written;
        i2s_channel_write(s_tx_chan, chunk, count * sizeof(int16_t),
                          &written, portMAX_DELAY);
    }
}

// ─────────────────────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────────────────────

void audio_init(uint32_t sample_rate)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(BOARD_AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk       = I2S_GPIO_UNUSED,
            .bclk       = BOARD_AUDIO_BCLK_PIN,
            .ws         = BOARD_AUDIO_WS_PIN,
            .dout       = BOARD_AUDIO_DOUT_PIN,
            .din        = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));

    // Pin to Core 0 so the high-priority audio task cannot preempt the
    // emulator task on Core 1.  Display task also runs on Core 0 (priority 4);
    // audio at configMAX_PRIORITIES-2 takes precedence there, which is fine.
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL,
                            configMAX_PRIORITIES - 2, NULL, 0);

    ESP_LOGI(TAG, "I2S audio on BCLK=%d WS=%d DOUT=%d, rate=%" PRIu32 " Hz",
             BOARD_AUDIO_BCLK_PIN, BOARD_AUDIO_WS_PIN, BOARD_AUDIO_DOUT_PIN,
             sample_rate);
}

size_t audio_push_samples(const int16_t *samples, size_t n_samples)
{
    size_t pushed = 0;
    for (size_t i = 0; i < n_samples; i++) {
        uint32_t next_head = (s_head + 1) & AUDIO_BUF_MASK;
        if (next_head == s_tail) {
            s_stats.overflow_samples += (n_samples - i);
            break;
        }
        s_buf[s_head] = samples[i];
        s_head = next_head;
        pushed++;
    }
    s_stats.pushed_samples += pushed;
    return pushed;
}

size_t audio_free_slots(void)
{
    uint32_t used = (s_head - s_tail) & AUDIO_BUF_MASK;
    return AUDIO_BUF_SAMPLES - used - 1;
}

AudioStats audio_stats_snapshot(void)
{
    return s_stats;
}
