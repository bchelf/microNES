#include "audio_pwm.h"

#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "pico/time.h"

enum {
    AUDIO_PWM_WRAP  = 255,
    AUDIO_BUF_SIZE  = 1024,   /* ~21 ms at 48 kHz */
};

/* Single-producer (main loop) / single-consumer (ISR) ring buffer.
 * Both run on core 0, so volatile indices give sufficient ordering. */
static int16_t          audio_buf[AUDIO_BUF_SIZE];
static volatile uint32_t audio_buf_head = 0;   /* consumer read position  */
static volatile uint32_t audio_buf_tail = 0;   /* producer write position */
static volatile uint32_t audio_underruns = 0;

static struct repeating_timer audio_timer;

static uint8_t audio_last_duty = 128u; /* sample-and-hold across underruns; 128 = silence */

static bool audio_timer_cb(struct repeating_timer *timer) {
    (void)timer;

    uint32_t head = audio_buf_head;
    uint8_t  duty;

    if (head == audio_buf_tail) {
        /* Buffer empty: hold last output level to avoid the mid-level step
         * that creates an audible buzz pattern at the underrun rate. */
        duty = audio_last_duty;
        ++audio_underruns;
    } else {
        /* Convert int16 → unsigned 8-bit centred at 128.
         * Reinterpret as uint16 and flip the sign bit: maps
         * [-32768..32767] → [0..255] without signed-shift UB. */
        uint16_t raw = (uint16_t)audio_buf[head];
        duty = (uint8_t)((raw >> 8) ^ 0x80u);
        audio_last_duty = duty;
        audio_buf_head = (head + 1u) % AUDIO_BUF_SIZE;
    }

    pwm_set_gpio_level(SMB2350_AUDIO_PIN, duty);
    return true;
}

void audio_pwm_init(uint32_t sample_rate) {
    gpio_set_function(SMB2350_AUDIO_PIN, GPIO_FUNC_PWM);
    uint audio_slice = pwm_gpio_to_slice_num(SMB2350_AUDIO_PIN);

    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, 1.0f);
    pwm_config_set_wrap(&config, AUDIO_PWM_WRAP);
    pwm_init(audio_slice, &config, true);

    pwm_set_gpio_level(SMB2350_AUDIO_PIN, AUDIO_PWM_WRAP / 2);

    audio_buf_head = 0;
    audio_buf_tail = 0;
    audio_underruns = 0;
    audio_last_duty = 128u;

    /* Fire the playback timer at slightly below sample_rate so the buffer
     * fills rather than underruns as the APU produces at exactly sample_rate.
     * +1 rounds the period up by 1 µs → timer fires at ~47619 Hz for 48 kHz.
     * The ~0.8 % rate difference causes ~372 samples/sec excess production;
     * once the 1024-sample buffer is full, the APU-side overflow handler
     * drops the excess silently. */
    int32_t period_us = -(int32_t)(1000000u / sample_rate + 1u);
    add_repeating_timer_us(period_us, audio_timer_cb, NULL, &audio_timer);
}

size_t audio_pwm_push_samples(const int16_t *samples, size_t count) {
    size_t   written = 0;
    uint32_t tail    = audio_buf_tail;

    for (size_t i = 0; i < count; ++i) {
        uint32_t next_tail = (tail + 1u) % AUDIO_BUF_SIZE;
        if (next_tail == audio_buf_head) {
            break;   /* buffer full — drop remainder */
        }
        audio_buf[tail] = samples[i];
        tail = next_tail;
        ++written;
    }

    audio_buf_tail = tail;
    return written;
}

uint32_t audio_pwm_underrun_count(void) {
    return audio_underruns;
}

uint32_t audio_pwm_buffer_level(void) {
    uint32_t head = audio_buf_head;
    uint32_t tail = audio_buf_tail;
    return (tail >= head) ? (tail - head) : (AUDIO_BUF_SIZE - head + tail);
}
