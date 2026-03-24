#include "audio.h"
#include "board.h"

#include "driver/ledc.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_log.h"

#include <stdint.h>
#include <string.h>

static const char *TAG = "audio";

enum {
    AUDIO_INPUT_RATE = 48000,
};

// ─────────────────────────────────────────────────────────────
//  Ring buffer
// ─────────────────────────────────────────────────────────────
#define AUDIO_BUF_SIZE  4096   // Must be a power of two
#define AUDIO_BUF_MASK  (AUDIO_BUF_SIZE - 1)

static volatile uint8_t  s_buf[AUDIO_BUF_SIZE];  // 8-bit PWM duties
static volatile uint32_t s_head = 0;  // writer index
static volatile uint32_t s_tail = 0;  // reader index (ISR)

static uint8_t           s_last_duty = 128;  // silence level (mid-rail)
static uint32_t          s_output_rate = 0;
static uint32_t          s_resample_phase = 0;
static AudioStats        s_stats = {0};

// ─────────────────────────────────────────────────────────────
//  LEDC configuration
//
//  PWM carrier: ~312 kHz  (APB 80 MHz / 256 ticks)
//  Resolution:  8-bit (256 levels)
// ─────────────────────────────────────────────────────────────
#define LEDC_DUTY_RES   LEDC_TIMER_8_BIT
#define LEDC_BASE_FREQ  (80000000 / 256)   // ~312 500 Hz

static void ledc_setup(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = BOARD_AUDIO_LEDC_SPEED,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num       = BOARD_AUDIO_LEDC_TIMER,
        .freq_hz         = LEDC_BASE_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channel = {
        .gpio_num   = BOARD_AUDIO_PIN,
        .speed_mode = BOARD_AUDIO_LEDC_SPEED,
        .channel    = BOARD_AUDIO_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = BOARD_AUDIO_LEDC_TIMER,
        .duty       = 128,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

// ─────────────────────────────────────────────────────────────
//  esp_timer ISR – fires at the configured sample rate
// ─────────────────────────────────────────────────────────────
static void audio_timer_cb(void *arg)
{
    uint32_t head = s_head;
    uint32_t tail = s_tail;

    if (head != tail) {
        uint8_t duty = s_buf[tail & AUDIO_BUF_MASK];
        s_tail = (tail + 1) & AUDIO_BUF_MASK;
        s_last_duty = duty;
    }
    // On underrun, hold the last sample to avoid buzz
    ledc_set_duty(BOARD_AUDIO_LEDC_SPEED, BOARD_AUDIO_LEDC_CHANNEL, s_last_duty);
    ledc_update_duty(BOARD_AUDIO_LEDC_SPEED, BOARD_AUDIO_LEDC_CHANNEL);
}

void audio_init(uint32_t sample_rate)
{
    ledc_setup();
    s_output_rate = sample_rate;
    s_resample_phase = 0;
    memset(&s_stats, 0, sizeof(s_stats));

    // Period in microseconds: 1 000 000 / sample_rate
    uint64_t period_us = 1000000ULL / sample_rate;

    esp_timer_handle_t timer;
    esp_timer_create_args_t args = {
        .callback        = audio_timer_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "audio",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, period_us));

    ESP_LOGI(TAG, "PWM audio on GPIO%d, sample_rate=%lu Hz, carrier~%d Hz",
             BOARD_AUDIO_PIN, (unsigned long)sample_rate, LEDC_BASE_FREQ);
}

size_t audio_push_samples(const int16_t *samples, size_t n_samples)
{
    size_t pushed = 0;
    for (size_t i = 0; i < n_samples; i++) {
        s_resample_phase += s_output_rate;
        if (s_resample_phase < AUDIO_INPUT_RATE) {
            ++s_stats.skipped_samples;
            continue;
        }
        s_resample_phase -= AUDIO_INPUT_RATE;

        uint32_t next_head = (s_head + 1) & AUDIO_BUF_MASK;
        if (next_head == s_tail) {
            s_stats.overflow_samples += (n_samples - i);
            break;  // buffer full
        }

        // Convert signed 16-bit PCM → unsigned 8-bit duty cycle
        // Maps [-32768..32767] → [0..255]  with mid-point at 128
        uint8_t duty = (uint8_t)(((uint16_t)samples[i] >> 8) ^ 0x80u);
        s_buf[s_head & AUDIO_BUF_MASK] = duty;
        s_head = next_head;
        pushed++;
    }
    s_stats.pushed_samples += pushed;
    return pushed;
}

size_t audio_free_slots(void)
{
    uint32_t used = (s_head - s_tail) & AUDIO_BUF_MASK;
    return AUDIO_BUF_SIZE - used - 1;
}

AudioStats audio_stats_snapshot(void)
{
    return s_stats;
}
